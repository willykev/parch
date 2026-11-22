#include "exporter.h"

#include "diskfile.h"
#include "documentreading.h"
#include "docxexport.h"
#include "epubexport.h"
#include "exportpictures.h"
#include "htmlexport.h"
#include "markdownexport.h"
#include "pdfexport.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>

QJsonObject ExportRecipe::toJson() const
{
    QJsonObject given;
    for (auto it = options.cbegin(); it != options.cend(); ++it)
        given.insert(it.key(), it.value().isEmpty() ? QJsonValue(true) : QJsonValue(it.value()));
    return QJsonObject{
        {QStringLiteral("source"), source},
        {QStringLiteral("format"), format},
        {QStringLiteral("output"), output},
        {QStringLiteral("dialect"), dialect.toJson()},
        {QStringLiteral("options"), given},
    };
}

QString Exporter::option(const ExportRecipe &recipe, const QString &name) const
{
    if (const auto given = recipe.options.constFind(name); given != recipe.options.cend()) return *given;
    for (const ExportOption &option : options())
        if (option.name == name) return option.defaultValue;
    return {};
}

namespace Exporters {

QList<const Exporter *> all()
{
    static const MarkdownExporter markdown;
    static const HtmlExporter html;
    static const PdfExporter pdf;
    static const DocxExporter docx;
    static const EpubExporter epub;
    return {&markdown, &html, &pdf, &docx, &epub};
}

QStringList formats()
{
    QStringList names;
    for (const Exporter *exporter : all()) names.append(exporter->format());
    return names;
}

const Exporter *find(const QString &format)
{
    for (const Exporter *exporter : all())
        if (exporter->format().compare(format, Qt::CaseInsensitive) == 0) return exporter;
    return nullptr;
}

const Exporter *forFile(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix.isEmpty()) return nullptr;
    for (const Exporter *exporter : all())
        if (exporter->suffixes().contains(suffix)) return exporter;
    return nullptr;
}

namespace {

std::optional<gid_t> testGroup;

const struct stat *ownedAccess(const struct stat &state)
{
    return S_ISREG(state.st_mode) && DiskFile::ownedBySavingUser(state.st_uid) ? &state : nullptr;
}

bool sameAccess(const std::optional<struct stat> &previous, const struct stat *current)
{
    if (!previous || !current) return !previous && !current;
    // A change to permissions, group or an extended attribute changes ctime.
    return previous->st_dev == current->st_dev && previous->st_ino == current->st_ino &&
           previous->st_ctim.tv_sec == current->st_ctim.tv_sec && previous->st_ctim.tv_nsec == current->st_ctim.tv_nsec;
}

// Carry the owner's access bits and, where permitted, the group and extended
// attributes. Read attributes from the same file whose ownership was checked.
int keepAccess(int folder, const QByteArray &name, const struct stat &there, int replacement)
{
    const int from = ::openat(folder, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    const auto close = qScopeGuard([&] { if (from >= 0) ::close(from); });
    struct stat opened{};
    if (from >= 0) {
        if (::fstat(from, &opened) != 0) return errno;
        if (opened.st_dev != there.st_dev || opened.st_ino != there.st_ino || opened.st_uid != there.st_uid) return ESTALE;
    }
    struct stat made{};
    if (::fstat(replacement, &made) != 0) return errno;
    const gid_t group = testGroup.value_or(there.st_gid);
    if (made.st_gid != group && ::fchown(replacement, uid_t(-1), group) != 0 &&
        errno != EPERM && errno != EACCES && errno != EINVAL && errno != EOPNOTSUPP && errno != ENOSYS) return errno;
    if (::fchmod(replacement, there.st_mode & 0777) != 0) return errno;
    if (from < 0) return 0;
    ssize_t size = ::flistxattr(from, nullptr, 0);
    if (size <= 0) return 0;
    QByteArray names(size, '\0');
    size = ::flistxattr(from, names.data(), size_t(names.size()));
    if (size <= 0) return 0;
    for (const QByteArray &attribute : names.left(size).split('\0')) {
        if (attribute.isEmpty()) continue;
        const ssize_t length = ::fgetxattr(from, attribute.constData(), nullptr, 0);
        if (length < 0) continue;
        QByteArray value(length, '\0');
        const ssize_t read = ::fgetxattr(from, attribute.constData(), value.data(), size_t(value.size()));
        if (read >= 0 && read <= length)
            (void)::fsetxattr(replacement, attribute.constData(), value.constData(), size_t(read), 0);
    }
    return 0;
}

} // namespace

void setReplacementGroupForTests(std::optional<gid_t> group)
{
    testGroup = group;
}

bool isSource(const md::Document &document, const Dialect::Choice &dialect, const QString &path)
{
    // The file it would replace, through any link, or the path itself when
    // nothing is there yet; and each file the document is made from the same.
    const auto resolved = [](const QString &file) {
        const QFileInfo info(file);
        return info.exists() ? info.canonicalFilePath() : QDir::cleanPath(info.absoluteFilePath());
    };
    const QString target = resolved(path);
    for (const md::SourceFile &file : document.files)
        if (!file.path.isEmpty() && file.path == target) return true;
    for (const QString &file : DocumentReading::referencedFiles(document, dialect))
        if (!file.isEmpty() && QDir::isAbsolutePath(file) && resolved(file) == target) return true;
    return false;
}

QString linkRefusal(const QString &path)
{
    struct stat status;
    if (::lstat(QFile::encodeName(path).constData(), &status) != 0 || !S_ISLNK(status.st_mode)) return {};
    const QString target = QFile::symLinkTarget(path);
    return QStringLiteral("%1 is a link to %2; parch never writes through a link")
        .arg(path, target.isEmpty() ? QStringLiteral("another file") : target);
}

PendingFile::PendingFile(const QString &path, ExistingOutput existing) : m_path(path), m_existing(existing) {}

PendingFile::~PendingFile()
{
    if (!m_temporary.isEmpty()) ::unlinkat(m_folder, m_temporary.constData(), 0);
    if (m_folder >= 0) ::close(m_folder);
}

bool PendingFile::fail(const QString &why, QString *error)
{
    *error = QStringLiteral("cannot write %1: %2").arg(m_path, why);
    return false;
}

bool PendingFile::write(const QByteArray &bytes, QString *error)
{
    if (m_existing != ExistingOutput::ReplaceLink) {
        if (const QString why = linkRefusal(m_path); !why.isEmpty()) {
            *error = why;
            return false;
        }
    }
    const QFileInfo info(m_path);
    m_name = QFile::encodeName(info.fileName());
    if (m_name.isEmpty()) return fail(QStringLiteral("it names no file"), error);
    m_folder = ::open(QFile::encodeName(info.absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (m_folder < 0) return fail(QString::fromLocal8Bit(std::strerror(errno)), error);
    struct stat there{};
    const bool exists = ::fstatat(m_folder, m_name.constData(), &there, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT) return fail(QString::fromLocal8Bit(std::strerror(errno)), error);
    if (exists && m_existing == ExistingOutput::Refuse) return fail(QStringLiteral("already exists"), error);
    if (exists && S_ISDIR(there.st_mode)) return fail(QStringLiteral("it is a folder"), error);
    if (exists && !S_ISREG(there.st_mode) && !(m_existing == ExistingOutput::ReplaceLink && S_ISLNK(there.st_mode)))
        return fail(QStringLiteral("it is not a regular file"), error);
    if (m_existing != ExistingOutput::Refuse && !stage(bytes, exists ? ownedAccess(there) : nullptr, error)) return false;
    m_contents = bytes;
    return true;
}

bool PendingFile::stage(const QByteArray &bytes, const struct stat *access, QString *error)
{
    // The temporary file is made anew, never through anything already there.
    QByteArray candidate;
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
        const QByteArray name = ".parch-" + QByteArray::number(QRandomGenerator::global()->generate64(), 16) + ".part";
        fd = ::openat(m_folder, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, access ? 0600 : 0666);
        if (fd >= 0) candidate = name;
        else if (errno != EEXIST) break;
    }
    if (fd < 0) return fail(QString::fromLocal8Bit(std::strerror(errno)), error);
    const auto cleanup = qScopeGuard([&] {
        ::close(fd);
        if (!candidate.isEmpty()) ::unlinkat(m_folder, candidate.constData(), 0);
    });
    int why = 0;
    for (qsizetype done = 0; done < bytes.size();) {
        const ssize_t wrote = ::write(fd, bytes.constData() + done, size_t(bytes.size() - done));
        if (wrote < 0 && errno == EINTR) continue;
        if (wrote <= 0) {
            why = wrote < 0 ? errno : EIO;
            break;
        }
        done += wrote;
    }
    if (why == 0 && access) why = keepAccess(m_folder, m_name, *access, fd);
    if (why == 0 && ::fsync(fd) != 0) why = errno;
    if (why != 0) return fail(QString::fromLocal8Bit(std::strerror(why)), error);
    if (!m_temporary.isEmpty() && ::unlinkat(m_folder, m_temporary.constData(), 0) != 0)
        return fail(QString::fromLocal8Bit(std::strerror(errno)), error);
    m_temporary = std::exchange(candidate, {});
    if (access) m_access = *access;
    else m_access.reset();
    return true;
}

bool PendingFile::commit(QString *error, const std::function<bool(int, bool)> &publishPictures,
                         const std::function<void()> &afterCommit)
{
    if (!m_contents && m_temporary.isEmpty()) return fail(QStringLiteral("nothing was written"), error);
    // An export with pictures publishes them and its page together with
    // respect to other exports. Plain files can commit in parallel.
    bool locked = false;
    while (!(locked = ::flock(m_folder, publishPictures ? LOCK_EX : LOCK_SH) == 0)) {
        const int reason = errno;
        if (reason == EINTR) continue;
        // Some mounts cannot lock a directory. Exclusive creation still protects each new name.
        if (reason == EOPNOTSUPP || reason == ENOSYS || reason == EINVAL || reason == EBADF || reason == ENOLCK) break;
        return fail(QString::fromLocal8Bit(std::strerror(reason)), error);
    }
    const auto unlock = qScopeGuard([&] { if (locked) ::flock(m_folder, LOCK_UN); });
    // A link that took the file's place meanwhile is refused as before.
    if (m_existing != ExistingOutput::ReplaceLink) {
        if (const QString why = linkRefusal(m_path); !why.isEmpty()) {
            *error = why;
            return false;
        }
    }
    struct stat current{};
    const bool exists = ::fstatat(m_folder, m_name.constData(), &current, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT) return fail(QString::fromLocal8Bit(std::strerror(errno)), error);
    if (exists && m_existing == ExistingOutput::Refuse) return fail(QStringLiteral("already exists"), error);
    if (exists && S_ISDIR(current.st_mode)) return fail(QStringLiteral("it is a folder"), error);
    if (exists && !S_ISREG(current.st_mode) && !(m_existing == ExistingOutput::ReplaceLink && S_ISLNK(current.st_mode)))
        return fail(QStringLiteral("it is not a regular file"), error);
    const struct stat *access = exists ? ownedAccess(current) : nullptr;
    // A different file or access list may have taken its place while the
    // export was written. Restage with the current permissions before publication.
    if (m_existing != ExistingOutput::Refuse && !sameAccess(m_access, access) && !stage(*m_contents, access, error)) return false;
    if (publishPictures && !publishPictures(m_folder, locked)) return false;
    if (m_existing == ExistingOutput::Refuse) {
        const SaveResult made = DiskFile::createAt(m_folder, QFileInfo(m_path).absolutePath(), QFile::decodeName(m_name), *m_contents);
        if (made.status != SaveStatus::Saved)
            return fail(made.status == SaveStatus::Conflict ? QStringLiteral("already exists") : made.error, error);
    } else {
        const auto published = DiskFile::publishAt(m_folder, m_temporary, m_name, exists ? &current : nullptr);
        if (published.status != SaveStatus::Saved) return fail(published.error, error);
    }
    m_temporary.clear();
    m_contents.reset();
    if (locked && afterCommit) afterCommit();
    return true;
}

bool writeFile(const QString &path, const QByteArray &bytes, QString *error, ExistingOutput existing)
{
    PendingFile file(path, existing);
    return file.write(bytes, error) && file.commit(error);
}

QString relativeTo(const QString &outputFolder, const QString &absolutePath)
{
    return QDir(outputFolder).relativeFilePath(absolutePath);
}

QString rebasedDestination(const md::Destination &destination, const QString &folder, md::DestinationSyntax syntax)
{
    const QString path = relativeTo(folder, destination.path);
    return (syntax == md::DestinationSyntax::Markdown ? md::encodeDestination(path) : md::encodeUrlPath(path))
        + md::destinationSuffix(destination, syntax);
}

std::function<QString(const md::Node &, const QString &)> destinationsFrom(const md::Document &document,
                                                                          const QString &folder)
{
    return [&document, folder = folder.isEmpty() ? QString() : QDir::cleanPath(folder)](const md::Node &node,
                                                                                      const QString &written) -> QString {
        if (written.isEmpty() || written.startsWith(u'#')) return written;
        const QString directory = md::destinationDirectory(document, node);
        const md::Destination target = md::classifyDestination(written, directory);
        if (target.kind != md::Destination::Kind::LocalFile || !target.absolute) return written;
        const bool link = node.type == md::NodeType::Link || node.type == md::NodeType::Autolink
                          || (node.type == md::NodeType::HtmlElement && node.label == u"a");
        if (link)
            if (const QString anchor = md::includedAnchor(document, target); !anchor.isEmpty()) return u'#' + anchor;
        if (folder.isEmpty() || directory.isEmpty() || QDir::cleanPath(directory) == folder || written.startsWith(u'/'))
            return written;
        return rebasedDestination(target, folder);
    };
}

QString count(int n, const QString &one, const QString &many)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

namespace {

bool holdsPictures(const md::Node &node)
{
    if (node.type == md::NodeType::MathInline || node.type == md::NodeType::MathBlock ||
        node.type == md::NodeType::MermaidBlock)
        return true;
    return std::any_of(node.children.cbegin(), node.children.cend(), holdsPictures);
}

} // namespace

std::unique_ptr<ExportPictures> drawPictures(const md::Document &document, const ExportRun &run, qreal fontSize,
                                             qreal share, bool *cancelled)
{
    *cancelled = run.stopped();
    if (*cancelled || !run.pictures || !holdsPictures(document.root)) return nullptr;
    ExportPictures::Options options;
    options.service = run.render;
    options.fontSize = fontSize;
    auto pictures = std::make_unique<ExportPictures>(options);
    run.report(0, QStringLiteral("Drawing formulas and diagrams"));
    const bool done = pictures->draw(document, run.cancelled, [&](int drawn, int total) {
        run.report(share * drawn / std::max(total, 1), QStringLiteral("Drawing formulas and diagrams"));
    });
    if (!done) {
        *cancelled = true;
        return nullptr;
    }
    return pictures;
}

ExportResult cancelled()
{
    ExportResult result;
    result.cancelled = true;
    result.error = QStringLiteral("the export was cancelled");
    return result;
}

QString frontMatterText(const md::Document &document, const QString &key)
{
    const md::YamlValue *value = document.frontMatter().data.find(key);
    if (!value) return {};
    if (value->isScalar()) return value->scalar.simplified();
    // A list of names, as `author:` often is.
    if (value->isList()) {
        QStringList items;
        for (const md::YamlValue &item : value->list)
            if (item.isScalar() && !item.scalar.trimmed().isEmpty()) items.append(item.scalar.simplified());
        return items.join(QStringLiteral(", "));
    }
    return {};
}

QString title(const md::Document &document, const ExportRecipe &recipe)
{
    QString title = recipe.options.value(QStringLiteral("title")).simplified();
    if (title.isEmpty()) title = frontMatterText(document, QStringLiteral("title"));
    if (title.isEmpty() && !document.headings.empty()) title = document.headings.front().text.simplified();
    if (title.isEmpty()) title = QFileInfo(recipe.source).completeBaseName();
    if (title.isEmpty()) title = QStringLiteral("Untitled");
    return title;
}

QString language(const md::Document &document)
{
    QString language = frontMatterText(document, QStringLiteral("lang"));
    if (language.isEmpty()) language = frontMatterText(document, QStringLiteral("language"));
    return language;
}

} // namespace Exporters
