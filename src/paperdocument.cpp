#include "paperdocument.h"

#include "exporter.h"
#include "picturedata.h"
#include "saferead.h"
#include "svgpaper.h"

#include <QAbstractTextDocumentLayout>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QPagedPaintDevice>
#include <QPainter>
#include <QScopeGuard>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextLayout>
#include <QTextTable>
#include <QTextObjectInterface>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace paper_detail {

// The pictures of a paper document as the layout sizes and draws them: a
// formula or diagram with vectors is drawn as vectors, anything else as its
// picture, whole. A picture never comes out wider than its column nor taller
// than a page.
class Objects : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

public:
    // Vectors by picture name, and the height of a page's text.
    struct Vector {
        QByteArray plain;
        QSizeF size;
        std::shared_ptr<QSvgRenderer> renderer;
    };
    QHash<QString, Vector> vectors;
    qreal bodyHeight = 0;
    // A formula in the text: its height and its baseline from its top, by
    // picture name.
    struct Formula {
        qreal height = 0;
        qreal baseline = 0;
    };
    QHash<QString, Formula> formulas;
    // Where each formula in the text was drawn, by its place in the document.
    QHash<int, QRectF> drawn;

    QSizeF intrinsicSize(QTextDocument *document, int position, const QTextFormat &format) override
    {
        const QTextImageFormat image = format.toImageFormat();
        const QSizeF natural = naturalSize(document, image.name());
        qreal width = image.width() > 0 ? image.width() : natural.width();
        qreal height = image.height() > 0 ? image.height()
                       : natural.width() > 0 ? width * natural.height() / natural.width()
                                             : natural.height();
        if (image.hasProperty(QTextFormat::ImageMaxWidth)) {
            const QTextBlockFormat block = document->findBlock(position).blockFormat();
            const qreal column = std::max<qreal>(1, document->pageSize().width() - block.leftMargin() -
                                                        block.rightMargin() - std::max<qreal>(0, block.textIndent()));
            const QTextLength limit = image.maximumWidth();
            const qreal most = limit.type() == QTextLength::PercentageLength ? column * limit.rawValue() / 100
                                                                             : limit.rawValue();
            if (most > 0 && width > most) {
                height *= most / width;
                width = most;
            }
        }
        if (bodyHeight > 0 && height > bodyHeight * 0.92) {
            width *= bodyHeight * 0.92 / height;
            height = bodyHeight * 0.92;
        }
        return QSizeF(width, height);
    }

    void drawObject(QPainter *painter, const QRectF &rect, QTextDocument *document, int position,
                    const QTextFormat &format) override
    {
        const QString name = format.toImageFormat().name();
        // A formula the builder set room beneath, so that it stands on the baseline.
        QString base = name;
        qreal room = 0;
        if (const qsizetype below = name.indexOf(QLatin1String("/below-")); below >= 0) {
            room = name.mid(below + 7).toInt();
            base = name.left(below);
        }
        QRectF target(rect.topLeft(), QSizeF(rect.width(), rect.height() - room));
        // A formula in the text stands on the baseline of its line, its depth
        // below, whatever the line's own descent: the line's bottom is where
        // the layout put the picture's bottom.
        if (const auto formula = formulas.constFind(base);
            formula != formulas.cend() && format.toCharFormat().verticalAlignment() == QTextCharFormat::AlignBottom) {
            const QTextBlock block = document->findBlock(position);
            const QTextLine line = block.layout() ? block.layout()->lineForTextPosition(position - block.position()) : QTextLine();
            if (line.isValid()) {
                const qreal baseline = rect.bottom() - line.descent();
                target = QRectF(rect.left(), baseline - formula->baseline, rect.width(), formula->height);
            }
            drawn.insert(position, target);
        }
        if (auto vector = vectors.find(base); vector != vectors.end()) {
            // Only the drawing written by cairo reaches this renderer.
            if (!vector->renderer) vector->renderer = std::make_shared<QSvgRenderer>(vector->plain);
            if (vector->renderer->isValid()) {
                vector->renderer->render(painter, target);
                return;
            }
        }
        const QImage picture = document->resource(QTextDocument::ImageResource, QUrl(name)).value<QImage>();
        if (!picture.isNull()) {
            painter->save();
            painter->setRenderHint(QPainter::SmoothPixmapTransform);
            painter->drawImage(rect, picture);
            painter->restore();
        }
    }

private:
    QSizeF naturalSize(QTextDocument *document, const QString &name)
    {
        if (const auto vector = vectors.constFind(name); vector != vectors.cend()) return vector->size;
        const QImage picture = document->resource(QTextDocument::ImageResource, QUrl(name)).value<QImage>();
        return picture.isNull() ? QSizeF(1, 1) : QSizeF(picture.size());
    }
};

} // namespace paper_detail

using paper_detail::Objects;

namespace {

QImage transparentDot()
{
    QImage dot(1, 1, QImage::Format_ARGB32_Premultiplied);
    dot.fill(Qt::transparent);
    return dot;
}

} // namespace

// What the builder asks for: formulas and diagrams as the export drew them,
// and images read from disk whole.
class PaperDocument::Resources : public preview::Resources {
public:
    Resources(const ExportPictures *pictures, Objects *objects, qint64 pixels)
        : m_pictures(pictures), m_objects(objects), m_pixelsLeft(pixels) {}

    preview::Picture math(const QString &tex, bool display, qreal fontSize) override
    {
        preview::Picture picture;
        const ExportPictures::Picture *drawn = m_pictures ? m_pictures->formula(tex, display) : nullptr;
        // Without the helper the TeX is printed as it is, quietly.
        if (!drawn || (!drawn->drawn && !m_pictures->missingMath().isEmpty())) return picture;
        if (!drawn->drawn) {
            picture.state = preview::Picture::Failed;
            picture.diagnostic = drawn->diagnostic;
            return picture;
        }
        const qreal scale = fontSize / std::max<qreal>(1, m_pictures->options().fontSize);
        picture.state = preview::Picture::Ready;
        picture.key = QStringLiteral("formula/%1/%2").arg(indexOf(QStringLiteral("f") + (display ? u'd' : u'i') + tex)).arg(fontSize);
        if (const auto it = m_images.constFind(picture.key); it != m_images.cend()) return *it;
        const QByteArray plain = SvgPaper::plain(drawn->svg, &picture.diagnostic);
        if (!plain.isEmpty()) readVector(plain, drawn->size, picture);
        if (picture.image.isNull()) {
            picture.state = preview::Picture::Failed;
            m_unread << QStringLiteral("formula (%1)").arg(picture.diagnostic);
        }
        picture.size = drawn->size * scale;
        picture.baseline = drawn->baseline * scale;
        picture.description = drawn->description;
        m_images.insert(picture.key, picture);
        if (picture.state != preview::Picture::Ready) return picture;
        m_objects->formulas.insert(picture.key, {picture.size.height(), picture.baseline});
        if (!display) m_formulas.insert(picture.key, {tex, picture.baseline});
        return picture;
    }

    preview::Picture diagram(const QString &source) override
    {
        preview::Picture picture;
        picture.state = preview::Picture::Failed;
        const ExportPictures::Picture *drawn = m_pictures ? m_pictures->diagram(source) : nullptr;
        if (!drawn || !drawn->drawn) {
            const QString missing = m_pictures ? m_pictures->missingDiagrams() : QString();
            picture.diagnostic = !missing.isEmpty() ? QStringLiteral("Printed as its source: %1.").arg(missing)
                                 : drawn          ? drawn->diagnostic
                                                  : QStringLiteral("Printed as its source: diagrams are not drawn.");
            return picture;
        }
        picture.key = QStringLiteral("diagram/%1").arg(indexOf(QStringLiteral("d") + source));
        const auto known = m_images.constFind(picture.key);
        if (known != m_images.cend()) return *known;
        if (drawn->vector()) {
            readVector(drawn->svg, drawn->size, picture);
        } else {
            readRaster(PictureData::read(drawn->png), picture);
        }
        if (picture.image.isNull()) m_unread << QStringLiteral("diagram (%1)").arg(picture.diagnostic);
        else picture.state = preview::Picture::Ready;
        picture.size = drawn->size;
        picture.description = drawn->description;
        m_images.insert(picture.key, picture);
        return picture;
    }

    preview::Picture image(const md::Destination &destination, const QString &written, const QStringList &folders) override
    {
        preview::Picture picture;
        picture.state = preview::Picture::Failed;
        QByteArray bytes;
        QString identity;
        if (destination.kind == md::Destination::Kind::Remote) {
            picture.diagnostic = QStringLiteral("pictures from the web are not fetched for an export");
            ++m_remote;
            return picture;
        }
        if (destination.kind == md::Destination::Kind::LocalFile && destination.absolute) {
            const QString canonical = QFileInfo(destination.path).canonicalFilePath();
            // Every cached file was opened within this paper document's resource folders.
            if (const auto it = m_images.constFind(canonical); it != m_images.cend()) return *it;
            const auto read = SafeRead::readPicture(destination.path, folders);
            if (read.ok) {
                identity = read.canonicalPath;
                bytes = read.bytes;
            } else {
                picture.diagnostic = read.missing ? QStringLiteral("there is no such file") : read.error;
            }
        } else if (destination.scheme == u"data") {
            bytes = PictureData::urlBytes(written, SafeRead::PictureBytes, &picture.diagnostic);
            if (!bytes.isEmpty())
                identity = QStringLiteral("data:") + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        } else {
            picture.diagnostic = destination.kind == md::Destination::Kind::LocalFile
                ? QStringLiteral("the document has no folder to find it in") : QStringLiteral("not a picture parch can read");
        }

        if (!identity.isEmpty()) {
            if (const auto it = m_images.constFind(identity); it != m_images.cend()) return *it;
            picture.key = QStringLiteral("image/%1").arg(m_images.size() + 1);
            const auto source = PictureData::read(bytes);
            if (source.format == "svg") {
                const QByteArray plain = SvgPaper::plain(source.bytes, &picture.diagnostic);
                if (!plain.isEmpty()) readVector(plain, SvgPaper::size(plain), picture);
            } else {
                readRaster(source, picture);
            }
            if (!picture.image.isNull()) picture.state = preview::Picture::Ready;
            m_images.insert(identity, picture);
        }
        if (picture.state == preview::Picture::Failed)
            m_unread << QStringLiteral("%1 (%2)").arg(destination.scheme == u"data" ? QStringLiteral("embedded picture") : written,
                                                       picture.diagnostic);
        return picture;
    }

    QStringList unread() const { return m_unread; }
    int remote() const { return m_remote; }
    // Formulas in the text by picture name: their TeX and baseline.
    struct Formula {
        QString tex;
        qreal baseline = 0;
    };
    QHash<QString, Formula> formulas() const { return m_formulas; }

private:
    int indexOf(const QString &what)
    {
        const auto it = m_names.constFind(what);
        if (it != m_names.cend()) return *it;
        const int index = int(m_names.size()) + 1;
        m_names.insert(what, index);
        return index;
    }

    void readVector(const QByteArray &plain, QSizeF size, preview::Picture &picture)
    {
        const qint64 pixels = SvgPaper::embeddedPixels(plain);
        if (pixels < 0) {
            readRaster({"svg", plain, {}}, picture);
            return;
        }
        if (pixels > m_pixelsLeft) {
            picture.diagnostic = QStringLiteral("there is no room for this picture within the total pixel limit");
            return;
        }
        m_pixelsLeft -= pixels;
        picture.image = transparentDot();
        picture.size = size;
        m_objects->vectors.insert(picture.key, {plain, size, {}});
    }

    void readRaster(const PictureData::Source &source, preview::Picture &picture)
    {
        const auto decoded = PictureData::decode(source, {.pixelsLeft = m_pixelsLeft});
        picture.image = decoded.image;
        picture.size = decoded.naturalSize;
        picture.diagnostic = decoded.error;
        m_pixelsLeft -= qint64(picture.image.width()) * picture.image.height();
    }

    const ExportPictures *m_pictures;
    Objects *m_objects;
    qint64 m_pixelsLeft;
    QHash<QString, preview::Picture> m_images;
    QHash<QString, int> m_names;
    QHash<QString, Formula> m_formulas;
    QStringList m_unread;
    int m_remote = 0;
};

QStringList PaperOptions::pageSizes()
{
    return {QStringLiteral("A4"), QStringLiteral("Letter"), QStringLiteral("A5"), QStringLiteral("Legal"),
            QStringLiteral("A3"), QStringLiteral("B5")};
}

QPageSize PaperOptions::sizeNamed(const QString &name)
{
    static const QHash<QString, QPageSize::PageSizeId> sizes{
        {QStringLiteral("a4"), QPageSize::A4},   {QStringLiteral("letter"), QPageSize::Letter},
        {QStringLiteral("a5"), QPageSize::A5},   {QStringLiteral("legal"), QPageSize::Legal},
        {QStringLiteral("a3"), QPageSize::A3},   {QStringLiteral("b5"), QPageSize::B5},
    };
    return QPageSize(sizes.value(name.toLower(), QPageSize::A4));
}

QStringList PaperOptions::templates()
{
    return {QStringLiteral("classic"), QStringLiteral("modern"), QStringLiteral("compact")};
}

PaperOptions PaperOptions::from(const Exporter &exporter, const ExportRecipe &recipe, QString *why)
{
    PaperOptions paper;
    const QString size = exporter.option(recipe, QStringLiteral("page-size"));
    if (!size.isEmpty()) {
        if (!pageSizes().contains(size, Qt::CaseInsensitive) && why)
            *why = QStringLiteral("--page-size takes %1, not \"%2\"").arg(pageSizes().join(QStringLiteral(", ")), size);
        paper.pageSize = sizeNamed(size);
    }
    // The paper a print dialog chose, in millimetres.
    const double width = recipe.options.value(QStringLiteral("page-width")).toDouble();
    const double height = recipe.options.value(QStringLiteral("page-height")).toDouble();
    if (width >= 50 && height >= 50 && width <= 2000 && height <= 2000)
        paper.pageSize = QPageSize(QSizeF(width, height), QPageSize::Millimeter, QString(), QPageSize::FuzzyMatch);
    if (recipe.options.contains(QStringLiteral("landscape"))) paper.orientation = QPageLayout::Landscape;
    // The template first; what is chosen as well goes over it.
    const QString look = exporter.option(recipe, QStringLiteral("template"));
    if (look == u"modern") {
        paper.family = QStringLiteral("sans-serif");
        paper.fontSize = 10;
        paper.lineHeight = 1.45;
    } else if (look == u"compact") {
        paper.fontSize = 10;
        paper.lineHeight = 1.3;
        paper.margin = 15;
    } else if (!look.isEmpty() && look != u"classic" && why) {
        *why = QStringLiteral("--template takes %1, not \"%2\"").arg(templates().join(QStringLiteral(", ")), look);
    }
    bool ok = false;
    const QString margin = recipe.options.value(QStringLiteral("margins"));
    if (!margin.isEmpty()) {
        const double value = margin.toDouble(&ok);
        if (ok && value >= 5 && value <= 60) paper.margin = value;
        else if (why) *why = QStringLiteral("--margins takes millimetres from 5 to 60, not \"%1\"").arg(margin);
    }
    const QString size2 = recipe.options.value(QStringLiteral("font-size"));
    if (!size2.isEmpty()) {
        const double value = size2.toDouble(&ok);
        if (ok && value >= 6 && value <= 36) paper.fontSize = value;
        else if (why) *why = QStringLiteral("--font-size takes points from 6 to 36, not \"%1\"").arg(size2);
    }
    if (const QString family = recipe.options.value(QStringLiteral("font")).trimmed(); !family.isEmpty()) paper.family = family;
    paper.pageNumbers = !recipe.options.contains(QStringLiteral("no-page-numbers"));
    return paper;
}

PaperDocument::PaperDocument(const md::Document &document, const DocumentCitations::Formatted *citations,
                             const ExportPictures *pictures, const PaperOptions &options)
    : m_source(document), m_citations(citations), m_options(options)
{
    m_style = PreviewStyle::paper();
    if (!options.family.isEmpty()) m_style.family = options.family;
    m_style.lineHeight = options.lineHeight;
    // Points to logical pixels, at 96 to the inch.
    m_style.size = options.fontSize * 96.0 / 72.0;
    // What the builder draws itself, such as a task's box, at print sharpness.
    m_style.scale = 4;
    auto objects = std::make_unique<Objects>();
    m_resources = std::make_unique<Resources>(pictures, objects.get(), options.picturePixelBudget);
    m_objects = std::move(objects);
}

PaperDocument::~PaperDocument() = default;

bool PaperDocument::stopping(qreal done)
{
    if (!m_stopped && m_going && !m_going(std::clamp<qreal>(done, 0, 1))) m_stopped = true;
    return m_stopped;
}

bool PaperDocument::layOutBlocks(qreal from, qreal to)
{
    QAbstractTextDocumentLayout *layout = m_text.documentLayout();
    const qreal characters = std::max(1, m_text.characterCount());
    int counted = 0;
    for (QTextBlock block = m_text.begin(); block.isValid(); block = block.next()) {
        // Laid out as far as this block, which Qt does block by block.
        (void)layout->blockBoundingRect(block);
        if (++counted % 32 == 0 && stopping(from + (to - from) * block.position() / characters)) return false;
    }
    return !stopping(to);
}

bool PaperDocument::layOut(QPagedPaintDevice *device, const std::function<bool(qreal done)> &going,
                          const QString &outputFolder)
{
    m_going = going;
    m_stopped = false;
    const auto forget = qScopeGuard([this] { m_going = {}; });
    const QPageLayout layout(m_options.pageSize, m_options.orientation,
                             QMarginsF(m_options.margin, m_options.margin, m_options.margin, m_options.margin),
                             QPageLayout::Millimeter);
    m_page = layout.fullRectPixels(96).size();
    m_body = QRectF(layout.paintRectPixels(96));
    auto *objects = static_cast<Objects *>(m_objects.get());
    objects->bodyHeight = m_body.height();

    preview::BuildOptions options;
    options.islands = false;
    options.interactive = true;
    options.localLinks = Exporters::destinationsFrom(m_source, outputFolder);
    options.citations = m_citations;
    m_text.documentLayout()->registerHandler(QTextFormat::ImageObject, objects);
    if (device) m_text.documentLayout()->setPaintDevice(device);
    // Set before building, so that pictures fit the column as they are made.
    m_text.setLayoutEnabled(false);
    m_text.setPageSize(m_body.size());
    QTextOption wrapping = m_text.defaultTextOption();
    // Code and long addresses break where they must rather than run off the page.
    wrapping.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    m_text.setDefaultTextOption(wrapping);
    const std::vector<preview::Section> sections = preview::Builder(m_source, m_style, m_resources.get(), options)
                                                       .buildAll(&m_text, [this](qreal done) { return !stopping(0.4 * done); });
    if (m_stopped) return false;
    m_text.setPageSize(m_body.size());
    // Letters as they are, not joined into ligatures, so that what is printed
    // is found and copied as it was written: "first", never "ﬁrst".
    QFont font = m_text.defaultFont();
    font.setFeature(QFont::Tag("liga"), 0);
    font.setFeature(QFont::Tag("clig"), 0);
    m_text.setDefaultFont(font);

    // The changes below are one edit, laid out once at its end rather than
    // after each of them, which for many notes or links would take minutes.
    QTextCursor edits(&m_text);
    edits.beginEditBlock();
    // Task boxes lead into the editor, which paper has not.
    for (QTextBlock block = m_text.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            const QTextCharFormat format = fragment.charFormat();
            if (!format.anchorHref().startsWith(QLatin1String("parch:"))) continue;
            QTextCursor cursor(&m_text);
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            QTextCharFormat plain = format;
            plain.setAnchor(false);
            plain.clearProperty(QTextFormat::AnchorHref);
            cursor.setCharFormat(plain);
        }
    }
    markNames(sections);
    // Stopped, the document is thrown away as it is, its edit never ended
    // and so never laid out.
    if (m_stopped) return false;
    ruleTables();
    // The way back from a note is an arrow every face has, not the emoji
    // the screen's fallback fonts may find.
    for (QTextCursor arrow = m_text.find(QString(QChar(0x21a9))); !arrow.isNull();
         arrow = m_text.find(QString(QChar(0x21a9)), arrow)) {
        const QTextCharFormat format = arrow.charFormat();
        arrow.insertText(QString(QChar(0x2191)), format);
    }
    edits.endEditBlock();
    m_text.setLayoutEnabled(true);

    // What the screen draws over the text, paper draws itself.
    const auto at = [this](int position) {
        QTextCursor cursor(&m_text);
        cursor.setPosition(std::clamp(position, 0, m_text.characterCount() - 1));
        return cursor;
    };
    int start = 1;
    for (const preview::Section &section : sections) {
        for (const preview::Mark &mark : section.marks) {
            switch (mark.kind) {
            case preview::Mark::QuoteBar:
                m_rules.append({Rule::QuoteBar, at(start + mark.first), at(start + mark.last), mark.x});
                break;
            case preview::Mark::Rule:
                m_rules.append({Rule::Line, at(start + mark.first), at(start + mark.first), mark.x});
                break;
            case preview::Mark::NotesRule:
                m_rules.append({Rule::ShortLine, at(start + mark.first), at(start + mark.first), mark.x});
                break;
            case preview::Mark::Heading:
                m_headingBlocks.append({m_text.findBlock(start + mark.first), mark.level, mark.text});
                break;
            default: break;
            }
        }
        start += section.length;
    }
    // Laid out block by block first, so that a document that takes long to
    // lay out says how far it has come and can be stopped on the way.
    if (!layOutBlocks(0.4, 0.6)) return false;
    breakPages(sections);
    keepHeadingsWithText();
    splitAcrossPages();
    if (m_stopped) return false;
    // Laid out once more from the start, as relayOut() does, block by block.
    m_text.markContentsDirty(0, m_text.characterCount());
    if (!layOutBlocks(0.8, 1)) return false;
    m_text.pageCount();
    findHeadings();
    return true;
}

void PaperDocument::ruleTables()
{
    // On screen a table's rules are drawn over it; on paper it has borders.
    const qreal padding = m_style.padding();
    std::function<void(QTextFrame *)> visit = [&](QTextFrame *frame) {
        for (QTextFrame *child : frame->childFrames()) {
            if (auto *table = qobject_cast<QTextTable *>(child)) {
                QTextTableFormat format = table->format();
                // With collapsed borders, a border of 1 draws the grid between the cells too.
                format.setBorder(1);
                format.setBorderBrush(m_style.rule);
                format.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
                format.setBorderCollapse(true);
                const QTextCharFormat first = table->cellAt(0, 0).firstCursorPosition().charFormat();
                // A header row repeats at the top of each page the table runs onto.
                if (first.fontWeight() > QFont::Normal) format.setHeaderRowCount(1);
                table->setFormat(format);
                for (int row = 0; row < table->rows(); ++row) {
                    QTextTableCell cell = table->cellAt(row, 0);
                    QTextTableCellFormat cellFormat = cell.format().toTableCellFormat();
                    cellFormat.setLeftPadding(padding * 0.7);
                    cell.setFormat(cellFormat);
                }
            }
            visit(child);
        }
    };
    visit(m_text.rootFrame());
}

void PaperDocument::relayOut()
{
    // After edits inside a table, Qt's layout can stop at the table's end
    // and leave what follows it unplaced: laying it all out again places it.
    m_text.markContentsDirty(0, m_text.characterCount());
    m_text.pageCount();
}

void PaperDocument::splitAcrossPages()
{
    QAbstractTextDocumentLayout *layout = m_text.documentLayout();
    // Measure against one layout. Editing one paragraph at a time would
    // repeatedly invalidate every following paragraph and table.
    struct Breaks {
        QTextBlock block;
        QList<int> offsets;
    };
    QList<Breaks> paragraphs;
    const qreal characters = std::max(1, m_text.characterCount());
    int visited = 0;
    for (QTextBlock block = m_text.begin(); block.isValid(); block = block.next()) {
        if (++visited % 32 == 0 && stopping(0.6 + 0.2 * block.position() / characters)) return;
        const QTextLayout *lines = block.layout();
        if (!lines || lines->lineCount() < 2) continue;
        const qreal top = layout->blockBoundingRect(block).top();
        int page = pageOf(top + lines->lineAt(0).y());
        QList<int> offsets;
        for (int i = 1; i < lines->lineCount(); ++i) {
            const QTextLine line = lines->lineAt(i);
            const int next = pageOf(top + line.y());
            if (next != page && line.textStart() > 0) offsets.append(line.textStart());
            page = next;
        }
        if (!offsets.isEmpty()) paragraphs.append({block, std::move(offsets)});
    }
    if (paragraphs.isEmpty()) return;

    // Work backwards so earlier offsets and the original character formats
    // still apply. A split replaces a line separator in code and keeps the
    // paragraph's outer margins only on its first and last pieces.
    QTextCursor cursor(&m_text);
    cursor.beginEditBlock();
    for (auto it = paragraphs.crbegin(); it != paragraphs.crend(); ++it) {
        const int start = it->block.position();
        QTextBlockFormat before = it->block.blockFormat(), after = before;
        before.setBottomMargin(0);
        after.setTopMargin(0);
        after.setTextIndent(0);
        after.setPageBreakPolicy(QTextFormat::PageBreak_Auto);
        QTextBlockFormat between = after;
        between.setBottomMargin(0);
        cursor.setPosition(start);
        cursor.setBlockFormat(before);
        for (qsizetype k = it->offsets.size() - 1; k >= 0; --k) {
            const int cut = start + it->offsets.at(k);
            cursor.setPosition(cut);
            if (m_text.characterAt(cut - 1) == QChar::LineSeparator)
                cursor.setPosition(cut - 1, QTextCursor::KeepAnchor);
            cursor.insertBlock(k == it->offsets.size() - 1 ? after : between, cursor.charFormat());
        }
    }
    cursor.endEditBlock();
}

void PaperDocument::markNames(const std::vector<preview::Section> &sections)
{
    // Where links within the document go: each place gets its names, on its
    // first character, which the PDF makes a destination.
    QMap<int, QStringList> places;
    int start = 1;
    for (const preview::Section &section : sections) {
        for (const preview::NamedAnchor &name : section.names) places[start + name.position].append(name.name);
        start += section.length;
    }
    const int end = m_text.characterCount() - 1;
    int marked = 0;
    for (auto it = places.cbegin(); it != places.cend(); ++it) {
        // Many names take a while; it can be stopped meanwhile.
        if (++marked % 256 == 0 && stopping(0.4 * it.key() / std::max(1, end))) return;
        int position = it.key();
        // A place where a block ends names the next character there is.
        while (position < end && m_text.characterAt(position) == QChar::ParagraphSeparator) ++position;
        if (position >= end) continue;
        QTextCursor cursor(&m_text);
        cursor.setPosition(position);
        cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
        QStringList names = cursor.charFormat().anchorNames();
        for (const QString &name : it.value())
            if (!names.contains(name)) names.append(name);
        QTextCharFormat named;
        named.setAnchor(true);
        named.setAnchorNames(names);
        // Qt interns character formats. Give different destination lists
        // distinct integer properties as well, so their format lookups scale.
        named.setProperty(QTextFormat::UserProperty + 0x512, position);
        cursor.mergeCharFormat(named);
    }
}

void PaperDocument::breakPages(const std::vector<preview::Section> &sections)
{
    int start = 1;
    for (const preview::Section &section : sections) {
        for (const preview::Mark &mark : section.marks) {
            if (mark.kind != preview::Mark::PageBreak) continue;
            QTextBlock block = m_text.findBlock(start + mark.first);
            // Nothing after it, nothing to start a page with.
            QTextBlock next = block.next();
            while (next.isValid() && next.length() <= 1 && !next.textList()) next = next.next();
            if (!block.isValid() || !next.isValid()) continue;
            QTextBlockFormat format = block.blockFormat();
            format.setPageBreakPolicy(QTextFormat::PageBreak_AlwaysAfter);
            format.setTopMargin(0);
            format.setBottomMargin(0);
            format.setLineHeight(1, QTextBlockFormat::FixedHeight);
            QTextCursor(block).setBlockFormat(format);
            m_breaks.append(block);
        }
        start += section.length;
    }
}

int PaperDocument::pageOf(qreal y) const
{
    return m_body.height() > 0 ? int(std::floor(y / m_body.height())) : 0;
}

qreal PaperDocument::blockBottom(const QTextBlock &block) const
{
    const QTextLayout *layout = block.layout();
    if (!layout || layout->lineCount() == 0) return m_text.documentLayout()->blockBoundingRect(block).bottom();
    const QTextLine last = layout->lineAt(layout->lineCount() - 1);
    return layout->position().y() + last.y() + last.height();
}

void PaperDocument::keepHeadingsWithText()
{
    // A heading at the foot of a page, with what it heads on the next, goes
    // to the next page too. A heading above another may follow it there, so
    // this goes round until nothing moves.
    if (m_headingBlocks.isEmpty() && m_breaks.isEmpty()) return;
    QAbstractTextDocumentLayout *layout = m_text.documentLayout();
    // A run of headings at the end has no following text to keep with it.
    // Lay it out normally, even when the index alone takes several pages.
    int withText = int(m_headingBlocks.size());
    QTextBlock following = m_text.lastBlock();
    for (; withText > 0; --withText) {
        while (following.isValid() && following.length() <= 1) following = following.previous();
        if (following != m_headingBlocks[withText - 1].block) break;
        following = following.previous();
    }
    for (int round = 0; round < 12; ++round) {
        QList<std::pair<QTextBlock, QTextBlockFormat>> changes;
        for (int i = 0; i < withText; ++i) {
            const auto &heading = m_headingBlocks.at(i);
            const QTextBlock block = heading.block;
            if (!block.isValid()) continue;
            if (block.blockFormat().pageBreakPolicy() & QTextFormat::PageBreak_AlwaysBefore) continue;
            QTextBlock next = block.next();
            while (next.isValid() && next.length() <= 1) next = next.next();
            if (!next.isValid()) continue;
            const qreal top = layout->blockBoundingRect(block).top();
            const QTextLayout *text = block.layout();
            const qreal firstLine = text && text->lineCount() ? text->position().y() + text->lineAt(0).y() : top;
            const QTextLayout *following = next.layout();
            const qreal nextLine = following && following->lineCount()
                                       ? following->position().y() + following->lineAt(0).y()
                                       : layout->blockBoundingRect(next).top();
            const int page = pageOf(firstLine);
            // Already the first thing on its page, it can go no further.
            if (pageOf(nextLine) <= page || firstLine - page * m_body.height() < 1) continue;
            QTextBlockFormat format = block.blockFormat();
            format.setPageBreakPolicy(format.pageBreakPolicy() | QTextFormat::PageBreak_AlwaysBefore);
            changes.append({block, format});
        }
        // A page break that falls at the top of a page has a new page
        // already: breaking again would leave this one empty.
        for (const QTextBlock &block : std::as_const(m_breaks)) {
            if (!(block.blockFormat().pageBreakPolicy() & QTextFormat::PageBreak_AlwaysAfter)) continue;
            const qreal top = layout->blockBoundingRect(block).top();
            if (top - pageOf(top + 0.5) * m_body.height() >= 1 || top < 1) continue;
            QTextBlockFormat format = block.blockFormat();
            format.setPageBreakPolicy(QTextFormat::PageBreak_Auto);
            changes.append({block, format});
        }
        if (changes.isEmpty()) break;
        QTextCursor edit(&m_text);
        edit.beginEditBlock();
        for (const auto &[block, format] : std::as_const(changes)) QTextCursor(block).setBlockFormat(format);
        edit.endEditBlock();
        relayOut();
    }
}

void PaperDocument::findHeadings()
{
    m_headings.clear();
    for (const HeadingBlock &heading : std::as_const(m_headingBlocks)) {
        const QTextBlock block = heading.block;
        if (!block.isValid()) continue;
        const QTextLayout *text = block.layout();
        const qreal y = text && text->lineCount() ? text->position().y() + text->lineAt(0).y()
                                                  : m_text.documentLayout()->blockBoundingRect(block).top();
        const int page = pageOf(y);
        m_headings.append({heading.level, heading.text, page, m_body.top() + y - page * m_body.height()});
    }
}

int PaperDocument::pageCount() const
{
    return std::max(1, int(std::ceil(m_text.documentLayout()->documentSize().height() / m_body.height() - 0.001)));
}

QSizeF PaperDocument::pageSize() const { return m_page; }

int PaperDocument::pageAt(int position) const
{
    const QTextBlock block = m_text.findBlock(position);
    if (!block.isValid() || !block.layout()) return 0;
    const qreal top = m_text.documentLayout()->blockBoundingRect(block).top();
    const QTextLine line = block.layout()->lineForTextPosition(position - block.position());
    return pageOf(top + (line.isValid() ? line.y() : 0));
}

void PaperDocument::paint(QPainter &painter, int page) const
{
    const qreal top = page * m_body.height();
    painter.save();
    painter.translate(m_body.left(), m_body.top() - top);
    // Half a pixel in at the top and the bottom, so that a block that ends
    // where the page begins, or begins where it ends, is not drawn here.
    const QRectF clip(0, top + 0.5, m_body.width(), m_body.height() - 1);
    // Wide enough for a bar beside a quote in the margin, never into the next page.
    painter.setClipRect(QRectF(-m_body.left(), top, m_page.width(), m_body.height()));
    QAbstractTextDocumentLayout::PaintContext context;
    context.clip = clip;
    context.palette.setColor(QPalette::Text, m_style.text);
    m_text.documentLayout()->draw(&painter, context);

    QAbstractTextDocumentLayout *layout = m_text.documentLayout();
    const qreal bar = std::max<qreal>(2, std::round(m_style.size / 7));
    for (const Rule &rule : m_rules) {
        const QTextBlock first = rule.first.block();
        const QTextBlock last = rule.last.block();
        if (!first.isValid() || !last.isValid()) continue;
        const qreal from = layout->blockBoundingRect(first).top();
        if (rule.kind == Rule::QuoteBar) {
            const qreal to = blockBottom(last);
            const qreal a = std::max(from, top), b = std::min(to, top + m_body.height());
            if (b <= a) continue;
            painter.fillRect(QRectF(rule.x, a, bar, b - a), m_style.quoteBar);
            continue;
        }
        if (from < top || from >= top + m_body.height()) continue;
        const qreal width = rule.kind == Rule::ShortLine ? std::round(m_body.width() * 0.28) : m_body.width() - rule.x;
        painter.fillRect(QRectF(rule.kind == Rule::ShortLine ? 0 : rule.x, from, width, 1), m_style.rule);
    }
    painter.restore();

    if (m_options.pageNumbers && pageCount() > 1) {
        painter.save();
        QFont font(m_style.family);
        font.setPixelSize(std::max(1, int(std::lround(m_style.size * 0.8))));
        painter.setFont(font);
        painter.setPen(m_style.ink(PreviewStyle::Dim));
        const qreal margin = m_page.height() - m_body.bottom();
        painter.drawText(QRectF(0, m_body.bottom() + margin * 0.35, m_page.width(), margin * 0.4),
                         Qt::AlignHCenter | Qt::AlignVCenter, QString::number(page + 1));
        painter.restore();
    }
}

QStringList PaperDocument::limitations() const
{
    QStringList limitations;
    if (const int remote = m_resources->remote())
        limitations.append(QStringLiteral("Images from the web are not fetched, so their descriptions stand in their "
                                          "place (%1)")
                               .arg(Exporters::count(remote, QStringLiteral("image"), QStringLiteral("images"))));
    if (const QStringList unread = m_resources->unread(); !unread.isEmpty())
        limitations.append(QStringLiteral("Images that could not be read are left out: %1").arg(unread.join(QStringLiteral(", "))));
    return limitations;
}

QList<PaperDocument::Placed> PaperDocument::formulaPlaces() const
{
    QList<Placed> places;
    const auto *objects = static_cast<const Objects *>(m_objects.get());
    const auto formulas = m_resources->formulas();
    for (auto it = objects->drawn.cbegin(); it != objects->drawn.cend(); ++it) {
        QTextCursor cursor(const_cast<QTextDocument *>(&m_text));
        cursor.setPosition(it.key());
        cursor.setPosition(it.key() + 1, QTextCursor::KeepAnchor);
        QString name = cursor.charFormat().toImageFormat().name();
        if (const qsizetype below = name.indexOf(QLatin1String("/below-")); below >= 0) name = name.left(below);
        const auto formula = formulas.constFind(name);
        if (formula == formulas.cend()) continue;
        const QTextBlock block = m_text.findBlock(it.key());
        const QTextLine line = block.layout()->lineForTextPosition(it.key() - block.position());
        if (!line.isValid()) continue;
        places.append({formula->tex, it->top() + formula->baseline,
                       block.layout()->position().y() + line.y() + line.ascent()});
    }
    return places;
}

#include "paperdocument.moc"
