#include "attachments.h"

#include "diskfile.h"
#include "document.h"
#include "htmlmarkdown.h"
#include "documentlinks.h"
#include "mdtree.h"
#include "pathtext.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include <QVariantMap>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

bool isBreak(QChar c)
{
    return c == u'\n' || c == u'\r';
}

bool blank(QStringView text)
{
    return text.trimmed().isEmpty();
}

QString errorText(int number)
{
    return QString::fromLocal8Bit(std::strerror(number));
}

// The folder's assets folder, made when it is not there; a draft's own
// folder, made first, is for its owner alone, as the store makes it.
QString assetsOf(const QString &folder, QString *error)
{
    QString reason;
    if (!QFileInfo(folder).isDir() && !DiskFile::makeFolders(folder, &reason, 0700)) {
        *error = Attachments::tr("Cannot create the folder %1: %2").arg(folder, reason);
        return {};
    }
    const QString assets = folder + QStringLiteral("/assets");
    if (!DiskFile::makeFolders(assets, &reason)) {
        *error = Attachments::tr("Cannot create the folder %1: %2").arg(assets, reason);
        return {};
    }
    return assets;
}

// Whether the file holds exactly these bytes.
bool holdsBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.size() == bytes.size() && file.open(QIODevice::ReadOnly) && file.readAll() == bytes;
}

// Whether two files hold the same bytes.
bool sameFiles(const QString &first, const QString &second)
{
    QFile a(first), b(second);
    if (a.size() != b.size() || !a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly)) return false;
    for (;;) {
        const QByteArray left = a.read(1 << 16), right = b.read(1 << 16);
        if (left != right) return false;
        if (left.isEmpty()) return a.error() == QFileDevice::NoError && b.error() == QFileDevice::NoError;
    }
}

// Puts a file in the folder's assets at the first of the names `candidate`
// gives that is free, or that holds what `same` finds is the same file
// already; `write` fills a new one. Nothing is ever written over or through
// a link, and a file that cannot be finished is taken away again.
Attachments::Placed place(const QString &folder, const std::function<QString(int)> &candidate,
                          const std::function<bool(const QString &path)> &same,
                          const std::function<bool(QFile &out, QString *error)> &write)
{
    QString error;
    const QString assets = assetsOf(folder, &error);
    if (assets.isEmpty()) return {{}, error};
    for (int n = 1; n <= 10000; ++n) {
        const QString name = candidate(n);
        const QString path = assets + u'/' + name;
        const QByteArray encoded = QFile::encodeName(path);
        struct stat there {};
        if (::lstat(encoded.constData(), &there) == 0) {
            if (S_ISREG(there.st_mode) && same(path)) return {QStringLiteral("assets/") + name, {}};
            continue;
        }
        const int descriptor = ::open(encoded.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            return {{}, Attachments::tr("Cannot write %1: %2").arg(path, errorText(errno))};
        }
        QFile out;
        if (!out.open(descriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
            ::close(descriptor);
            ::unlink(encoded.constData());
            return {{}, Attachments::tr("Cannot write %1").arg(path)};
        }
        QString why;
        // The file reaches the disk before the text refers to it.
        const bool written = write(out, &why) && out.flush() && ::fdatasync(out.handle()) == 0;
        if (!written) {
            const QString reason = why.isEmpty() ? out.errorString() : why;
            out.close();
            ::unlink(encoded.constData());
            return {{}, Attachments::tr("Cannot write %1: %2").arg(path, reason)};
        }
        return {QStringLiteral("assets/") + name, {}};
    }
    return {{}, Attachments::tr("No free name is left in %1").arg(assets)};
}

} // namespace

// ---------------------------------------------------------------------------

Insertion::Insertion(Document *document, SourceRange range)
    : QObject(document), m_document(document), m_start(range.start), m_end(range.start + range.length)
{
    m_following = connect(document->buffer(), &SourceBuffer::sourceChanged, this,
                          [this](Revision, SourceRange edited, const QString &replacement) { follow(edited, replacement); });
}

Document *Insertion::document() const { return m_document; }

SourceRange Insertion::range() const { return {m_start, m_end - m_start}; }

void Insertion::follow(SourceRange edited, const QString &replacement)
{
    const int start = edited.start, end = edited.start + edited.length;
    const int delta = int(replacement.size()) - edited.length;
    // Text put in at the very place stays after it: what was pasted came first.
    if (m_start == m_end && edited.length == 0 && start == m_start) return;
    if (end <= m_start) {
        m_start += delta;
        m_end += delta;
    } else if (start >= m_end) {
        return;
    } else {
        // An edit inside the range, or across it, leaves only a place.
        m_touched = true;
        m_start = m_end = start < m_start ? start + int(replacement.size()) : m_start;
    }
}

void Insertion::cancel()
{
    disconnect(m_following);
    m_used = true;
    deleteLater();
}

bool Insertion::insert(const QString &text, bool block)
{
    if (m_used) return false;
    cancel();
    Document *document = m_document;
    if (!document || text.isEmpty() || document->state() == Document::State::ReadOnly || document->isTextless())
        return false;
    SourceBuffer *buffer = document->buffer();
    SourceRange range;
    QString fittedText;
    {
        // The copy of the text is gone before the edit, or the buffer would copy all of it to change it.
        const QString source = buffer->text();
        const int length = int(source.size());
        const int start = std::clamp(m_start, 0, length);
        const int end = m_touched ? start : std::clamp(m_end, start, length);
        range = {start, end - start};
        // Edits beside the place can have made it fall inside a grapheme.
        if (!sourceGraphemeRange(source, range)) {
            const int at = sourceGraphemeBoundary(source, start) ? start : previousSourceGrapheme(source, start);
            range = {std::max(0, at), 0};
        }
        fittedText = fitted(source, range, text, block);
    }
    const QString newline = document->newline();
    if (newline != u"\n") fittedText.replace(u'\n', newline);
    return buffer->replace(range, fittedText, EditKind::Other) == SourceBuffer::EditOutcome::Applied;
}

QString Insertion::fitted(QStringView source, SourceRange range, const QString &text, bool block)
{
    if (!block) return text;
    const int start = range.start, end = range.start + range.length;
    const int size = int(source.size());
    int lineStart = start;
    while (lineStart > 0 && !isBreak(source[lineStart - 1])) --lineStart;
    int lineEnd = end;
    while (lineEnd < size && !isBreak(source[lineEnd])) ++lineEnd;
    QString before, after;
    if (!blank(source.mid(lineStart, start - lineStart))) {
        before = QStringLiteral("\n\n");
    } else if (lineStart > 0) {
        // The line above, past its line break.
        int previousEnd = lineStart - 1;
        if (previousEnd > 0 && source[previousEnd] == u'\n' && source[previousEnd - 1] == u'\r') --previousEnd;
        int previousStart = previousEnd;
        while (previousStart > 0 && !isBreak(source[previousStart - 1])) --previousStart;
        if (!blank(source.mid(previousStart, previousEnd - previousStart))) before = QStringLiteral("\n");
    }
    if (!blank(source.mid(end, lineEnd - end))) {
        after = QStringLiteral("\n\n");
    } else if (lineEnd < size) {
        int nextStart = lineEnd + 1;
        if (source[lineEnd] == u'\r' && nextStart < size && source[nextStart] == u'\n') ++nextStart;
        int nextEnd = nextStart;
        while (nextEnd < size && !isBreak(source[nextEnd])) ++nextEnd;
        if (!blank(source.mid(nextStart, nextEnd - nextStart))) after = QStringLiteral("\n");
    }
    return before + text + after;
}

// ---------------------------------------------------------------------------

Attachments::Attachments(const AppPaths &paths, QObject *parent)
    : QObject(parent), m_paths(paths), m_today([] { return QDate::currentDate(); })
{
    // Two copies at once: a large one never holds up a small one for long.
    m_pool.setMaxThreadCount(2);
}

Attachments::~Attachments()
{
    m_stopping = true;
    m_pool.waitForDone();
}

void Attachments::setLibraryFolders(std::function<QStringList()> folders)
{
    m_libraries = std::move(folders);
}

QStringList Attachments::libraryFolders() const
{
    return m_libraries ? m_libraries() : QStringList();
}

void Attachments::setToday(std::function<QDate()> today)
{
    m_today = std::move(today);
}

QDate Attachments::today() const
{
    return m_today();
}

void Attachments::setLargeFileBytes(qint64 bytes)
{
    m_large = bytes;
}

qint64 Attachments::largeFileBytes() const
{
    return m_large;
}

QString Attachments::documentFolder(const Document *document) const
{
    if (!document) return {};
    // parch's own text finds what it names among parch's resources, where
    // nothing can be put: it keeps nothing on disk.
    if (document->isScratch()) return document->scratchFolder();
    return document->isDraft() ? m_paths.drafts() + u'/' + document->id() : PathText::folderOf(document->path());
}

QString Attachments::libraryOf(const Document *document) const
{
    if (!document || document->isDraft()) return {};
    QStringList roots;
    for (const QString &folder : libraryFolders()) {
        const QString canonical = QFileInfo(folder).canonicalFilePath();
        roots << (canonical.isEmpty() ? folder : canonical);
    }
    return DocumentLinks::libraryOf(documentFolder(document), roots);
}

void Attachments::report(const QString &title, const QString &detail)
{
    emit problem(title, detail);
}

Attachments::Placed Attachments::savePasted(const QString &folder, const QDate &date, const QString &extension,
                                            const QByteArray &bytes)
{
    const QString stem = QStringLiteral("pasted-") + date.toString(QStringLiteral("yyyy-MM-dd")) + u'-';
    return place(
        folder, [&](int n) { return stem + QString::number(n) + u'.' + extension; },
        [&](const QString &path) { return holdsBytes(path, bytes); },
        [&](QFile &out, QString *) { return out.write(bytes) == bytes.size(); });
}

Attachments::Placed Attachments::copyIn(const QString &folder, const QString &source, const std::atomic<bool> *stop)
{
    const QString name = PathText::fileName(source);
    // The number goes before the extension; a name that starts with its only dot has none.
    const qsizetype dot = name.lastIndexOf(u'.');
    const QString stem = dot > 0 ? name.left(dot) : name;
    const QString extension = dot > 0 ? name.mid(dot) : QString();
    return place(
        folder, [&](int n) { return n == 1 ? name : stem + u'-' + QString::number(n) + extension; },
        [&](const QString &path) { return sameFiles(path, source); },
        [&](QFile &out, QString *error) {
            QFile input(source);
            if (!input.open(QIODevice::ReadOnly)) {
                *error = input.errorString();
                return false;
            }
            for (;;) {
                if (stop && stop->load()) {
                    *error = tr("parch was ending");
                    return false;
                }
                const QByteArray chunk = input.read(1 << 20);
                if (chunk.isEmpty()) break;
                if (out.write(chunk) != chunk.size()) return false;
            }
            if (input.error() != QFileDevice::NoError) {
                *error = input.errorString();
                return false;
            }
            return true;
        });
}

QString Attachments::extensionFor(const QString &mimeType)
{
    static const QHash<QString, QString> kept = {
        {QStringLiteral("image/png"), QStringLiteral("png")},   {QStringLiteral("image/jpeg"), QStringLiteral("jpg")},
        {QStringLiteral("image/jpg"), QStringLiteral("jpg")},   {QStringLiteral("image/gif"), QStringLiteral("gif")},
        {QStringLiteral("image/webp"), QStringLiteral("webp")}, {QStringLiteral("image/svg+xml"), QStringLiteral("svg")},
        {QStringLiteral("image/avif"), QStringLiteral("avif")}};
    return kept.value(mimeType.toLower());
}

bool Attachments::isImageFile(const QString &name)
{
    static const QStringList suffixes = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                         QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("svg"),
                                         QStringLiteral("bmp"), QStringLiteral("avif")};
    return suffixes.contains(QFileInfo(name).suffix().toLower());
}

bool Attachments::isMarkdownFile(const QString &name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == u"md" || suffix == u"markdown";
}

QList<Attachments::Dropped> Attachments::plan(const QStringList &paths, const QString &folder, bool draft,
                                              const QStringList &libraries)
{
    const QString home = QFileInfo(folder).canonicalFilePath();
    QStringList roots;
    for (const QString &library : libraries) {
        const QString root = QFileInfo(library).canonicalFilePath();
        if (!root.isEmpty()) roots << root;
    }
    const auto inside = [](const QString &path, const QString &root) {
        return !root.isEmpty() && path.startsWith(root == u"/" ? root : root + u'/');
    };
    QList<Dropped> dropped;
    for (const QString &path : paths) {
        Dropped file;
        file.source = path;
        const QFileInfo info(path);
        file.name = info.fileName();
        if (info.isDir()) {
            file.folder = true;
        } else if (!info.isFile()) {
            // Gone, or no regular file, as a pipe or a device, which a copy could never finish.
            file.missing = true;
        } else {
            const QString canonical = info.canonicalFilePath();
            file.source = canonical;
            file.size = info.size();
            file.image = isImageFile(file.name);
            file.markdown = isMarkdownFile(file.name);
            const QString relative = home.isEmpty() ? QString() : QDir(home).relativeFilePath(canonical);
            const bool inLibrary = std::any_of(roots.cbegin(), roots.cend(),
                                               [&](const QString &root) { return inside(canonical, root); });
            if (inside(canonical, home) || (!draft && inLibrary)) file.linked = relative;
            if (!draft && !relative.isEmpty())
                file.included = relative.startsWith(QStringLiteral("../")) ? relative : QStringLiteral("./") + relative;
        }
        dropped.append(file);
    }
    return dropped;
}

QString Attachments::imageMarkdown(const QString &alt, const QString &relative)
{
    return QStringLiteral("![") + HtmlMarkdown::escapeText(alt) + QStringLiteral("](") + md::encodeDestination(relative)
           + u')';
}

QString Attachments::linkMarkdown(const QString &text, const QString &relative)
{
    return u'[' + HtmlMarkdown::escapeText(text) + QStringLiteral("](") + md::encodeDestination(relative) + u')';
}

QString Attachments::includeLine(const QString &included)
{
    const bool spaced = std::any_of(included.cbegin(), included.cend(), [](QChar c) { return c.isSpace(); });
    return spaced ? u'"' + included + u'"' : included;
}
