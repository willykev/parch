// EditorBinding's paste and drop: rich text as Markdown, images and files
// attached through Attachments, the questions a drop asks, and plain text.

#include "editorbinding.h"

#include "attachments.h"
#include "htmlmarkdown.h"
#include "sourceboundaries.h"

#include <QBuffer>
#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>

namespace {

// The local files among a list of URLs, or nothing unless every one is local.
QStringList localFiles(const QList<QUrl> &urls)
{
    QStringList files;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile()) return {};
        files << url.toLocalFile();
    }
    return files;
}

// The image formats a pasted image is saved in as it comes, best first;
// anything else is saved as PNG.
const QStringList &keptImageTypes()
{
    static const QStringList types = {QStringLiteral("image/png"), QStringLiteral("image/jpeg"),
                                      QStringLiteral("image/webp"), QStringLiteral("image/gif"),
                                      QStringLiteral("image/svg+xml"), QStringLiteral("image/avif")};
    return types;
}

bool holdsImage(const QMimeData *mime)
{
    if (mime->hasImage()) return true;
    const QStringList formats = mime->formats();
    return std::any_of(formats.cbegin(), formats.cend(),
                       [](const QString &format) { return format.startsWith(QStringLiteral("image/")); });
}

// HTML converted off the UI thread, with the images it carried saved.
struct Converted {
    QString markdown;
    bool inlineOnly = true;
    QStringList errors;
};

// HTML up to this many characters is converted at once; more goes to a
// worker, so that the window never waits on it, and comes in when ready.
constexpr qsizetype ConvertAtOnceChars = 64 * 1024;
// Markdown that grows past this many times its HTML, and past
// MaxGrowthFloor characters, as deeply nested lists do, is not pasted:
// the plain text the clipboard holds is pasted in its place.
constexpr qsizetype MaxGrowth = 4;
constexpr qsizetype MaxGrowthFloor = 1024 * 1024;

bool overgrown(const QString &markdown, const QString &html)
{
    return markdown.size() > MaxGrowthFloor && markdown.size() > MaxGrowth * html.size();
}

bool mergesIn(const Document *document)
{
    return TableGrids::dialectOf(document->markdownOptions()).mergedCells;
}

void convertPaste(Attachments *attachments, Insertion *insertion, const QString &html, const QString &plain,
                  const QString &folder, const QDate &date, bool images)
{
    const bool mergedCells = mergesIn(insertion->document());
    attachments->run<Converted>(
        insertion,
        [html, folder, date, images, mergedCells] {
            Converted converted;
            HtmlMarkdown::ImageSink save;
            if (images) {
                save = [&](const QByteArray &bytes, const QString &type) -> QString {
                    QString extension = Attachments::extensionFor(type);
                    QByteArray data = bytes;
                    if (extension.isEmpty()) {
                        QImage image;
                        if (!image.loadFromData(bytes)) return {};
                        data.clear();
                        QBuffer buffer(&data);
                        if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
                        extension = QStringLiteral("png");
                    }
                    const Attachments::Placed placed = Attachments::savePasted(folder, date, extension, data);
                    if (!placed.error.isEmpty()) converted.errors << placed.error;
                    return placed.relative;
                };
            }
            converted.markdown = HtmlMarkdown::convert(html, save, &converted.inlineOnly, mergedCells);
            if (!images && overgrown(converted.markdown, html)) converted.markdown.clear();
            return converted;
        },
        [insertion, attachments, html, plain, folder, date, images, mergedCells](Converted converted) {
            // The insertion still follows the source while a changed dialect
            // gets its own conversion. Any saved images are reused by bytes.
            if (insertion->document() && mergesIn(insertion->document()) != mergedCells) {
                convertPaste(attachments, insertion, html, plain, folder, date, images);
                return;
            }
            if (!converted.errors.isEmpty())
                attachments->report(EditorBinding::tr("Pasted images not saved"),
                                    EditorBinding::tr("Their alt text was pasted instead. %1").arg(converted.errors.join(u' ')));
            if (!converted.markdown.isEmpty()) insertion->insert(converted.markdown, !converted.inlineOnly);
            else if (!plain.isEmpty()) insertion->insert(plain, false);
            else insertion->cancel();
        });
}

} // namespace

// A drop of files on its way in: looked at first, then asked about, then copied.
struct EditorBinding::DropRequest {
    QPointer<Insertion> insertion;
    QString folder;
    bool draft = false;
    QList<Attachments::Dropped> files;
    bool include = false;
    bool copyAgreed = false;
    // Handed on to be placed, after which the insertion is the copy's to use.
    bool placing = false;

    // A drop nobody answers any more, as one whose view has closed, puts nothing in.
    ~DropRequest()
    {
        if (!placing && insertion) insertion->cancel();
    }
};

Attachments *EditorBinding::attachments() const { return m_attachments; }

void EditorBinding::setAttachments(Attachments *attachments)
{
    if (m_attachments == attachments) return;
    m_attachments = attachments;
    emit attachmentsChanged();
}

void EditorBinding::writeThrough(Insertion *insertion)
{
    QPointer<EditorBinding> view(this);
    // Whether this view showed tables as grids when the text was asked for.
    const bool grids = m_tables->enabled();
    insertion->setWriter([view, grids](Document *document, SourceRange range, const QString &text, bool block) -> int {
        // Another document now in the view: the insertion writes it itself,
        // into a table's cell as grids have it if the view showed them.
        if (!view || view->m_document != document || !view->m_loaded)
            return grids ? Insertion::Buffer : Insertion::Plain;
        // No grid in the view, tables shown as their Markdown: as text.
        if (view->m_tables->grids().isEmpty()) return Insertion::Plain;
        // In a cell the text goes in on the cell's line, its breaks spaces.
        const bool inCell = view->m_tables->gridAt(range.start)
                            && view->m_tables->inCellText(range.start, range.start + range.length);
        QString written;
        QString lead;
        {
            const QString source = view->m_buffer->text();
            written = Insertion::fitted(source, range, text, block && !inCell);
            lead = written.left(written.indexOf(text));
        }
        const QString newline = view->m_document->newline();
        if (newline != u"\n") {
            written.replace(u'\n', newline);
            lead.replace(u'\n', newline);
        }
        // The caret goes after the text while it is still where the text
        // was pasted; one taken elsewhere meanwhile stays there.
        const SourceRange selected = view->selection();
        const bool following = selected.start == range.start && selected.length == range.length;
        view->m_keepCaret = !following;
        const bool written_ = view->replace(range, written, EditKind::Other, following);
        view->m_keepCaret = false;
        if (!written_) return -1;
        // A cell's text is where the caret now ends it, the table written anew around it.
        if (following && inCell) return view->m_cursorSource - int(written.size());
        return range.start + int(lead.size());
    });
}

void EditorBinding::pastePlainText()
{
    if (!editable()) return;
    const QString text = QGuiApplication::clipboard()->text();
    if (text.isEmpty()) return;
    QMimeData plain;
    plain.setText(text);
    if (m_tables->pasteCells(&plain, true) == TableGrids::Paste::NotCells)
        insertText(withDocumentBreaks(text), EditKind::Other);
}

void EditorBinding::pasteFrom(const QMimeData *mime)
{
    // Cells from a spreadsheet, or another grid, fill a grid's cells from the caret's.
    const bool hasImage = holdsImage(mime);
    const auto pasted = m_tables->pasteCells(mime, !mime->hasUrls() && !hasImage);
    if (pasted == TableGrids::Paste::PlainText) {
        const QString text = mime->text();
        if (!text.isEmpty()) insertText(withDocumentBreaks(text), EditKind::Other);
        return;
    }
    if (pasted != TableGrids::Paste::NotCells) return;
    cancelComposition();
    // Files copied in a file manager come in as when they are dropped.
    const QStringList files = mime->hasUrls() ? localFiles(mime->urls()) : QStringList();
    if (!files.isEmpty() && m_attachments) {
        attachFiles(files, selection());
        return;
    }
    const bool image = m_attachments && hasImage;
    if (mime->hasHtml()) {
        const QString html = mime->html();
        QString alt;
        // A copied image brings the HTML of that image too: the image itself
        // is kept, with the alt text the HTML gives it.
        if (image && HtmlMarkdown::isLoneImage(html, &alt)) {
            pasteImage(mime, alt, selection());
            return;
        }
        if (insertHtml(html, selection(), mime->text())) return;
    }
    if (image) {
        pasteImage(mime, {}, selection());
        return;
    }
    const QString text = mime->text();
    if (!text.isEmpty()) insertText(withDocumentBreaks(text), EditKind::Other);
}

bool EditorBinding::insertHtml(const QString &html, SourceRange range, const QString &plain)
{
    if (!editable() || m_blocked || !m_document) return false;
    // Images the HTML carries are saved first, off the UI thread, and the
    // text goes in once they are in place; so does the text of a large page,
    // whose conversion would hold up the window.
    const bool images = HtmlMarkdown::mayEmbedImages(html);
    if (m_attachments && (images || html.size() > ConvertAtOnceChars)) {
        auto *insertion = new Insertion(m_document, range);
        writeThrough(insertion);
        const QString folder = m_attachments->documentFolder(m_document);
        const QDate date = m_attachments->today();
        convertPaste(m_attachments, insertion, html, plain, folder, date, images);
        return true;
    }
    bool inlineOnly = true;
    const QString markdown = HtmlMarkdown::convert(html, {}, &inlineOnly, mergesIn(m_document));
    if (markdown.isEmpty() || overgrown(markdown, html)) return false;
    cancelComposition();
    // One edit, so that one undo takes the whole paste back.
    replace(range, withDocumentBreaks(Insertion::fitted(m_buffer->text(), range, markdown, !inlineOnly)),
            EditKind::Other);
    return true;
}

void EditorBinding::pasteImage(const QMimeData *mime, const QString &alt, SourceRange range)
{
    if (!m_attachments || !m_document) return;
    // Bytes in a format worth keeping as it is; or else bytes of another
    // format, or the image itself, made a PNG on the worker.
    QString extension;
    QByteArray bytes, other;
    for (const QString &type : keptImageTypes()) {
        if (!mime->hasFormat(type)) continue;
        bytes = mime->data(type);
        if (bytes.isEmpty()) continue;
        extension = Attachments::extensionFor(type);
        break;
    }
    QImage image;
    if (bytes.isEmpty()) {
        const QStringList formats = mime->formats();
        for (const QString &format : formats)
            if (other.isEmpty() && format.startsWith(QStringLiteral("image/"))) other = mime->data(format);
        if (other.isEmpty()) image = qvariant_cast<QImage>(mime->imageData());
        if (other.isEmpty() && image.isNull()) return;
        extension = QStringLiteral("png");
    }
    auto *insertion = new Insertion(m_document, range);
    writeThrough(insertion);
    const QString folder = m_attachments->documentFolder(m_document);
    const QDate date = m_attachments->today();
    Attachments *attachments = m_attachments;
    const QPointer<EditorBinding> view(this);
    attachments->run<Attachments::Placed>(
        insertion,
        [folder, date, extension, bytes, other, image] {
            QByteArray data = bytes;
            if (data.isEmpty()) {
                QImage decoded = image;
                if (!other.isEmpty()) decoded.loadFromData(other);
                QBuffer buffer(&data);
                if (decoded.isNull() || !buffer.open(QIODevice::WriteOnly) || !decoded.save(&buffer, "PNG"))
                    return Attachments::Placed{{}, tr("The image on the clipboard could not be read")};
            }
            return Attachments::savePasted(folder, date, extension, data);
        },
        [insertion, attachments, alt, view](Attachments::Placed placed) {
            if (!placed.error.isEmpty()) {
                insertion->cancel();
                attachments->report(tr("Image not pasted"), placed.error);
                return;
            }
            const QString markdown = Attachments::imageMarkdown(alt, placed.relative);
            const Document *document = insertion->document();
            if (insertion->insert(markdown, false) && alt.isEmpty() && view)
                view->inviteAltText(document, insertion->insertedAt(), int(markdown.size()));
        });
}

void EditorBinding::inviteAltText(const Document *document, int at, int length)
{
    if (document != m_document || !m_loaded || m_composing) return;
    if (m_cursorSource != at + length || m_anchorSource != m_cursorSource) return;
    // Past "![".
    select(at + 2, at + 2);
}

bool EditorBinding::dropAt(qreal x, qreal y, const QList<QUrl> &urls, const QString &html, const QString &text)
{
    if (!editable() || m_blocked || !m_loaded) return false;
    int shown = 0;
    m_positionAt.invoke(m_editor, Q_RETURN_ARG(int, shown), Q_ARG(qreal, x), Q_ARG(qreal, y));
    int position = 0;
    {
        const QString source = m_buffer->text();
        position = m_pairs.toSource(qBound(0, shown, m_pairs.toEditor(int(source.size()))));
        if (!sourceGraphemeBoundary(source, position)) position = previousSourceGrapheme(source, position);
    }
    const SourceRange at{qMax(0, position), 0};
    QStringList files;
    QStringList links;
    for (const QUrl &url : urls) {
        if (url.isLocalFile()) files << url.toLocalFile();
        else if (url.isValid() && !url.isEmpty()) links << u'<' + url.toString(QUrl::FullyEncoded) + u'>';
    }
    cancelComposition();
    if (!files.isEmpty()) {
        if (!m_attachments) return false;
        attachFiles(files, at);
        return true;
    }
    if (!html.isEmpty() && insertHtml(html, at, text)) return true;
    if (!links.isEmpty()) return replace(at, links.join(u' '), EditKind::Other);
    if (!text.isEmpty()) return replace(at, withDocumentBreaks(text), EditKind::Other);
    return false;
}

void EditorBinding::attachFiles(const QStringList &paths, SourceRange range)
{
    if (!m_attachments || !m_document) return;
    // A drop still waiting for an answer gives way to this one.
    if (m_drop && m_drop->insertion) m_drop->insertion->cancel();
    auto request = std::make_shared<DropRequest>();
    request->insertion = new Insertion(m_document, range);
    writeThrough(request->insertion);
    request->folder = m_attachments->documentFolder(m_document);
    request->draft = m_document->isDraft();
    m_drop = request;
    const QStringList libraries = m_attachments->libraryFolders();
    // The view asks about the drop; one closed meanwhile gives it up.
    const QPointer<EditorBinding> view(this);
    m_attachments->run<QList<Attachments::Dropped>>(
        request->insertion,
        [paths, folder = request->folder, draft = request->draft, libraries] {
            return Attachments::plan(paths, folder, draft, libraries);
        },
        [this, view, request](QList<Attachments::Dropped> files) {
            if (!view) {
                if (request->insertion) request->insertion->cancel();
                return;
            }
            if (m_drop != request || !request->insertion) return;
            QStringList skipped;
            for (const Attachments::Dropped &file : std::as_const(files)) {
                if (file.folder) skipped << tr("%1 is a folder").arg(file.name);
                else if (file.missing) skipped << tr("%1 is not a file that can be read").arg(file.name);
                else request->files.append(file);
            }
            if (!skipped.isEmpty() && m_attachments)
                m_attachments->report(tr("Not attached"), skipped.join(QStringLiteral("; ")));
            if (request->files.isEmpty()) {
                request->insertion->cancel();
                m_drop.reset();
                return;
            }
            const auto markdown = std::find_if(request->files.cbegin(), request->files.cend(),
                                               [](const Attachments::Dropped &file) { return file.markdown; });
            if (markdown != request->files.cend()) {
                emit includeAsked(markdown->name, !request->draft);
                return;
            }
            continueDrop(request);
        });
}

void EditorBinding::answerInclude(int choice)
{
    const std::shared_ptr<DropRequest> request = m_drop;
    if (!request) return;
    if (choice != InsertLink && choice != Include) {
        if (request->insertion) request->insertion->cancel();
        m_drop.reset();
        return;
    }
    request->include = choice == Include && !request->draft;
    continueDrop(request);
}

void EditorBinding::answerCopy(bool copy)
{
    const std::shared_ptr<DropRequest> request = m_drop;
    if (!request) return;
    if (!copy) {
        if (request->insertion) request->insertion->cancel();
        m_drop.reset();
        return;
    }
    request->copyAgreed = true;
    continueDrop(request);
}

namespace {
// Whether the file is copied into the assets rather than linked where it is.
bool copied(const Attachments::Dropped &file, bool include)
{
    return !(file.markdown && include) && file.linked.isEmpty();
}
} // namespace

void EditorBinding::continueDrop(const std::shared_ptr<DropRequest> &request)
{
    if (!request->insertion || !m_attachments) {
        m_drop.reset();
        return;
    }
    if (!request->copyAgreed) {
        QStringList names;
        qint64 bytes = 0;
        for (const Attachments::Dropped &file : std::as_const(request->files)) {
            if (!copied(file, request->include) || file.size <= m_attachments->largeFileBytes()) continue;
            names << file.name;
            bytes += file.size;
        }
        if (!names.isEmpty()) {
            emit copyAsked(names, bytes);
            return;
        }
    }
    if (m_drop == request) m_drop.reset();
    placeDrop(request);
}

void EditorBinding::placeDrop(const std::shared_ptr<DropRequest> &request)
{
    Attachments *attachments = m_attachments;
    Insertion *insertion = request->insertion;
    request->placing = true;
    const std::atomic<bool> *stopping = attachments->stopping();
    // From here on the drop belongs to its document, not to this view: it
    // lands even once the view shows another document or has closed.
    attachments->run<QList<Attachments::Placed>>(
        insertion,
        [files = request->files, folder = request->folder, include = request->include, stopping] {
            QList<Attachments::Placed> placed;
            for (const Attachments::Dropped &file : files)
                placed.append(copied(file, include) ? Attachments::copyIn(folder, file.source, stopping)
                                                    : Attachments::Placed{file.linked, {}});
            return placed;
        },
        [insertion, attachments, files = request->files, include = request->include](QList<Attachments::Placed> placed) {
            QStringList pieces, failures;
            bool block = false;
            for (qsizetype i = 0; i < files.size(); ++i) {
                const Attachments::Dropped &file = files[i];
                if (!placed[i].error.isEmpty()) {
                    failures << placed[i].error;
                    continue;
                }
                if (file.markdown && include) {
                    pieces << Attachments::includeLine(file.included);
                    block = true;
                } else if (file.image) {
                    pieces << Attachments::imageMarkdown(QFileInfo(file.name).completeBaseName(), placed[i].relative);
                } else {
                    pieces << Attachments::linkMarkdown(file.name, placed[i].relative);
                }
            }
            if (!failures.isEmpty()) attachments->report(tr("Not attached"), failures.join(QStringLiteral("; ")));
            block = block || pieces.size() > 1;
            if (pieces.isEmpty()) insertion->cancel();
            else insertion->insert(pieces.join(block ? QStringLiteral("\n\n") : QString()), block);
        });
}
