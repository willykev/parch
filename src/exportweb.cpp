#include "exportweb.h"

#include "documentreading.h"
#include "mdhtml.h"
#include "saferead.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QScopeGuard>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace ExportWeb {

namespace {

QString suffixFor(const QString &mediaType)
{
    if (mediaType == u"image/png") return QStringLiteral("png");
    if (mediaType == u"image/jpeg") return QStringLiteral("jpg");
    if (mediaType == u"image/gif") return QStringLiteral("gif");
    if (mediaType == u"image/webp") return QStringLiteral("webp");
    if (mediaType == u"image/svg+xml") return QStringLiteral("svg");
    return QStringLiteral("bin");
}

// A file name a folder, a URL and a ZIP all take as it is: letters, digits,
// dots, hyphens and underscores.
QString safeName(const QString &name)
{
    QString out;
    for (const QChar c : name) {
        if ((c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z') || (c >= u'0' && c <= u'9') || c == u'.' || c == u'-' ||
            c == u'_')
            out.append(c);
        else if (!out.endsWith(u'-'))
            out.append(u'-');
    }
    while (out.startsWith(u'.') || out.startsWith(u'-')) out.remove(0, 1);
    return out.isEmpty() ? QStringLiteral("file") : out.left(80);
}

const QByteArray folderSignature("parch\n");
enum class Entry { Missing, Same, Occupied, Unavailable };

Entry compareEntry(int folder, const QByteArray &name, const QByteArray &bytes, DiskVersion *matched = nullptr,
                   const DiskVersion *known = nullptr)
{
    struct stat existing {};
    if (::fstatat(folder, name.constData(), &existing, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? Entry::Missing : Entry::Unavailable;
    if (!S_ISREG(existing.st_mode) || existing.st_size != bytes.size()) return Entry::Occupied;
    const int fd = ::openat(folder, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return Entry::Occupied;
    const auto close = qScopeGuard([fd] { ::close(fd); });
    struct stat state {};
    FileStamp stamp;
    if (!DiskFile::stampDescriptor(fd, state, stamp) || !S_ISREG(state.st_mode) || state.st_size != bytes.size())
        return Entry::Occupied;
    if (known && known->exists && known->device == quint64(state.st_dev) && known->inode == quint64(state.st_ino)
        && known->stamp.trusted() && known->stamp == stamp) return Entry::Same;
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly)) return Entry::Occupied;
    char buffer[16384];
    for (qsizetype at = 0; at < bytes.size();) {
        const qint64 count = file.read(buffer, sizeof buffer);
        if (count <= 0 || count > bytes.size() - at || QByteArrayView(buffer, count) != QByteArrayView(bytes).sliced(at, count))
            return Entry::Occupied;
        at += count;
    }
    if (file.read(buffer, 1) != 0) return Entry::Occupied;
    if (matched) *matched = DiskVersion{{}, quint64(state.st_dev), quint64(state.st_ino), {}, true, stamp};
    return Entry::Same;
}

int openFolder(const QString &path)
{
    return ::open(QFile::encodeName(QDir::cleanPath(path)).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

QString number(qreal value)
{
    return QString::number(value, 'f', 4).remove(QRegularExpression(QStringLiteral("\\.?0+$")));
}

} // namespace

Assets::Assets(Mode mode, const QString &folder, const QString &outputFolder)
    : m_mode(mode), m_folder(folder), m_outputFolder(outputFolder)
{
}

QString Assets::mediaType(const QByteArray &data, const QString &name)
{
    if (data.startsWith("\x89PNG\r\n\x1a\n")) return QStringLiteral("image/png");
    if (data.startsWith("\xff\xd8\xff")) return QStringLiteral("image/jpeg");
    if (data.startsWith("GIF87a") || data.startsWith("GIF89a")) return QStringLiteral("image/gif");
    if (data.startsWith("RIFF") && data.mid(8, 4) == "WEBP") return QStringLiteral("image/webp");
    const QString type = QMimeDatabase().mimeTypeForFileNameAndData(name, data).name();
    if (type == u"image/svg+xml") return type;
    return {};
}

QString Assets::file(const QString &absolutePath, const QStringList &folders, QString *error)
{
    if (m_mode == Mode::Link) {
        *error = SafeRead::pictureRefusal(absolutePath, folders);
        if (!error->isEmpty()) return {};
        return QString::fromUtf8(QUrl::toPercentEncoding(Exporters::relativeTo(m_outputFolder, absolutePath), "/"));
    }
    if (const auto known = m_byPath.constFind(absolutePath); known != m_byPath.cend()) return *known;
    const SafeRead::Result read = SafeRead::readPicture(absolutePath, folders);
    if (!read.ok) {
        *error = read.error;
        return {};
    }
    const QByteArray &data = read.bytes;
    const QString type = mediaType(data, absolutePath);
    if (type.isEmpty()) {
        *error = QStringLiteral("not a picture a page can show");
        return {};
    }
    const QString url = place(data, type, QFileInfo(absolutePath).fileName());
    m_byPath.insert(absolutePath, url);
    return url;
}

QString Assets::bytes(const QByteArray &data, const QString &mediaType, const QString &suggested)
{
    return place(data, mediaType, suggested);
}

QString Assets::place(const QByteArray &data, const QString &mediaType, const QString &suggested)
{
    if (m_mode == Mode::Embed || m_mode == Mode::Link)
        return QStringLiteral("data:%1;base64,%2").arg(mediaType, QString::fromLatin1(data.toBase64()));
    const QByteArray hash = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    if (const auto known = m_byHash.constFind(hash); known != m_byHash.cend()) return *known;
    QFileInfo info(safeName(suggested));
    QString stem = info.completeBaseName();
    if (stem.isEmpty()) stem = QStringLiteral("picture");
    const QString suffix = suffixFor(mediaType);
    QString name = stem + u'.' + suffix;
    const int folder = m_mode == Mode::Copy ? openFolder(QDir(m_outputFolder).filePath(m_folder)) : -1;
    const auto close = qScopeGuard([folder] { if (folder >= 0) ::close(folder); });
    const bool managed = folder >= 0 && compareEntry(folder, kFolderMark, folderSignature) == Entry::Same;
    DiskVersion matched;
    const auto occupied = [&] {
        return std::any_of(m_assets.cbegin(), m_assets.cend(), [&](const Asset &a) { return a.name == name; })
               || (managed && compareEntry(folder, QFile::encodeName(name), data, &matched) == Entry::Occupied);
    };
    for (int n = 2; occupied(); ++n) name = QStringLiteral("%1-%2.%3").arg(stem).arg(n).arg(suffix);
    m_assets.append({name, mediaType, data, matched});
    // The folder's name is a path in the URL too: a `#`, `?` or `%` in it
    // (an output named "notes#2.html") must not end the path there.
    const QString url = QString::fromUtf8(QUrl::toPercentEncoding(m_folder, "/") + QUrl::toPercentEncoding(name));
    m_byHash.insert(hash, url);
    return url;
}

QString formula(const ExportPictures::Picture &picture, const QString &tex, bool display, qreal fontSize)
{
    if (!picture.drawn || picture.svg.isEmpty() || fontSize <= 0) return {};
    const QString svg = QString::fromUtf8(picture.svg);
    const qsizetype open = svg.indexOf(QLatin1String("<svg"));
    const qsizetype end = open < 0 ? -1 : svg.indexOf(u'>', open);
    if (end < 0) return {};
    QString tag = svg.mid(open, end - open);
    const bool selfClosed = tag.endsWith(u'/');
    if (selfClosed) tag.chop(1);
    static const QRegularExpression sized(QStringLiteral("\\s(width|height|style|role|aria-label|class)\\s*=\\s*\"[^\"]*\""));
    tag.remove(sized);
    const qreal width = picture.size.width() / fontSize, height = picture.size.height() / fontSize;
    const qreal depth = (picture.size.height() - picture.baseline) / fontSize;
    const QString label = md::escapeHtml(tex.simplified());
    tag += QStringLiteral(" width=\"%1em\" height=\"%2em\" role=\"img\" aria-label=\"%3\" class=\"math-svg\"")
               .arg(number(width), number(height), label);
    if (!display) tag += QStringLiteral(" style=\"vertical-align: -%1em\"").arg(number(depth));
    QString markup = tag + u'>' + QStringLiteral("<title>%1</title>").arg(label);
    markup += selfClosed ? QStringLiteral("</svg>") : svg.mid(end + 1).trimmed();
    if (display) return QStringLiteral("<div class=\"math display\">%1</div>").arg(markup);
    return QStringLiteral("<span class=\"math\">%1</span>").arg(markup);
}

QByteArray diagramSvg(const ExportPictures::Picture &picture)
{
    const auto runs = [](const QByteArray &svg) {
        const QByteArray lower = svg.toLower();
        static const QRegularExpression handler(QStringLiteral("\\son[a-z]+\\s*="));
        return lower.contains("<script") || lower.contains("<foreignobject") || lower.contains("javascript:") ||
               handler.match(QString::fromUtf8(lower)).hasMatch();
    };
    if (!picture.markup.isEmpty() && !runs(picture.markup)) return picture.markup;
    if (!picture.svg.isEmpty() && !runs(picture.svg)) return picture.svg;
    return {};
}

namespace {

// Only a file this export created can be retired. Its attribute binds its bytes to its name,
// inode, folder and owning output. Files copied in, edited or shared are kept.
constexpr char assetOwner[] = "user.parch.export";

QByteArray ownership(const QByteArray &output, const QByteArray &name, const struct stat &folder, const struct stat &file)
{
    QByteArray key = output + '\0' + name + '\0';
    for (quint64 value : {quint64(folder.st_dev), quint64(folder.st_ino), quint64(file.st_ino), quint64(file.st_size),
                          quint64(file.st_mtim.tv_sec), quint64(file.st_mtim.tv_nsec)})
        key += QByteArray::number(value) + '/';
    return QCryptographicHash::hash(key, QCryptographicHash::Sha256);
}

QByteArray ownerOf(int fd)
{
    char tag[64]; // SHA-256 of the identity, then SHA-256 of the content
    return ::fgetxattr(fd, assetOwner, tag, sizeof tag) == sizeof tag ? QByteArray(tag, sizeof tag) : QByteArray();
}

class PictureFolder {
public:
    PictureFolder(const QString &target, const QString &output)
        : path(target), output(QFile::encodeName(QFileInfo(output).fileName())) {}
    ~PictureFolder() { if (fd >= 0) ::close(fd); }

    bool publish(int parent, bool locked, const QList<Assets::Asset> &assets, QString *error)
    {
        if (!assets.isEmpty()) {
            const QString why = Exporters::linkRefusal(path);
            if (!why.isEmpty()) {
                *error = why;
                return false;
            }
        }
        const QByteArray leaf = QFile::encodeName(QFileInfo(path).fileName());
        const auto open = [&] { return ::openat(parent, leaf.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC); };
        fd = open();
        if (fd < 0 && errno == ENOENT && !assets.isEmpty()) {
            const bool created = ::mkdirat(parent, leaf.constData(), 0777) == 0;
            if (!created && errno != EEXIST) {
                const int reason = errno;
                *error = QStringLiteral("cannot create %1: %2").arg(path, QString::fromLocal8Bit(std::strerror(reason)));
                return false;
            }
            fd = open();
            if (created && fd >= 0) {
                const auto mark = DiskFile::createAt(fd, path, QString::fromLatin1(kFolderMark), folderSignature);
                if (mark.status != SaveStatus::Saved) {
                    struct stat opened {}, there {};
                    if (::fstat(fd, &opened) == 0 && ::fstatat(parent, leaf.constData(), &there, AT_SYMLINK_NOFOLLOW) == 0
                        && opened.st_dev == there.st_dev && opened.st_ino == there.st_ino) ::unlinkat(parent, leaf.constData(), AT_REMOVEDIR);
                    *error = QStringLiteral("cannot mark %1: %2").arg(path, mark.error);
                    return false;
                }
            }
        }
        if (fd < 0 || ::fstat(fd, &identity) || compareEntry(fd, kFolderMark, folderSignature) != Entry::Same) {
            if (assets.isEmpty()) return true;
            *error = QStringLiteral("%1 is not accessible or an export did not make it").arg(path);
            return false;
        }
        managed = true;
        for (const auto &asset : assets) {
            const QByteArray name = QFile::encodeName(asset.name);
            keep.insert(name);
            const Entry entry = compareEntry(fd, name, asset.bytes, nullptr, &asset.existing);
            if (entry == Entry::Same) continue;
            if (entry == Entry::Missing) {
                const auto made = DiskFile::createAt(fd, path, asset.name, asset.bytes);
                if (made.status == SaveStatus::Saved) {
                    if (locked) markNew(name, made.version);
                    continue;
                }
                if (made.status == SaveStatus::Conflict && compareEntry(fd, name, asset.bytes) == Entry::Same) continue;
                *error = QStringLiteral("cannot install %1 in %2: %3").arg(asset.name, path, made.error);
                return false;
            }
            *error = QStringLiteral("cannot install %1 in %2 without replacing an existing file").arg(asset.name, path);
            return false;
        }
        struct stat there {};
        if (::fstatat(parent, leaf.constData(), &there, AT_SYMLINK_NOFOLLOW) == 0
            && there.st_dev == identity.st_dev && there.st_ino == identity.st_ino) return true;
        *error = QStringLiteral("the pictures folder %1 changed during export").arg(path);
        return false;
    }

    // An image adopted as a document input or shared with a second output no longer belongs to one export.
    bool protectInputs(const md::Document &doc, const ExportRecipe &recipe, QString *error)
    {
        if (!managed) return true;
        const QString canonical = QFileInfo(path).canonicalFilePath();
        QSet<QByteArray> inputs;
        const QStringList paths = DocumentReading::referencedFiles(doc, recipe.dialect, true);
        for (const QString &input : paths) {
            const QString real = QFileInfo(input).canonicalFilePath();
            const QFileInfo info(real);
            if (!real.isEmpty() && info.absolutePath() == canonical) inputs.insert(QFile::encodeName(info.fileName()));
        }
        keep.unite(inputs);
        for (const auto &name : std::as_const(keep)) {
            const int file = openRegular(name);
            if (file < 0) continue;
            const auto close = qScopeGuard([file] { ::close(file); });
            const QByteArray tag = ownerOf(file);
            struct stat st {};
            if (tag.isEmpty() || ::fstat(file, &st) != 0) continue;
            if (!inputs.contains(name) && tag.startsWith(ownership(output, name, identity, st))) continue;
            if (::fremovexattr(file, assetOwner) == 0 || errno == ENODATA) continue;
            *error = QStringLiteral("cannot preserve the shared picture %1").arg(QFile::decodeName(name));
            return false;
        }
        return true;
    }

    void retireUnused() const
    {
        if (!managed) return;
        const int scan = ::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (scan < 0) return;
        DIR *entries = ::fdopendir(scan);
        if (!entries) { ::close(scan); return; }
        const auto close = qScopeGuard([entries] { ::closedir(entries); });
        while (const dirent *entry = ::readdir(entries)) {
            const QByteArray name(entry->d_name);
            if (name.startsWith('.') || keep.contains(name)) continue;
            const int file = openRegular(name);
            if (file < 0) continue;
            const auto closeFile = qScopeGuard([file] { ::close(file); });
            const QByteArray tag = ownerOf(file);
            struct stat st {}, now {};
            if (tag.isEmpty() || ::fstat(file, &st) || !tag.startsWith(ownership(output, name, identity, st))) continue;
            QFile contents;
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (!contents.open(file, QIODevice::ReadOnly) || !hash.addData(&contents) || contents.error() != QFileDevice::NoError
                || hash.result() != tag.last(32)) continue;
            if (::fstatat(fd, name.constData(), &now, AT_SYMLINK_NOFOLLOW) == 0 && now.st_dev == st.st_dev
                && now.st_ino == st.st_ino && now.st_ctim.tv_sec == st.st_ctim.tv_sec && now.st_ctim.tv_nsec == st.st_ctim.tv_nsec)
                ::unlinkat(fd, name.constData(), 0);
        }
    }

private:
    int openRegular(const QByteArray &name) const
    {
        struct stat st {};
        if (::fstatat(fd, name.constData(), &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) return -1;
        return ::openat(fd, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    }

    void markNew(const QByteArray &name, const DiskVersion &made) const
    {
        const int file = openRegular(name);
        if (file < 0) return;
        const auto close = qScopeGuard([file] { ::close(file); });
        struct stat st {};
        if (::fstat(file, &st) || quint64(st.st_dev) != made.device || quint64(st.st_ino) != made.inode) return;
        const QByteArray tag = ownership(output, name, identity, st) + made.sha256;
        ::fsetxattr(file, assetOwner, tag.constData(), size_t(tag.size()), XATTR_CREATE);
    }

    QString path;
    QByteArray output;
    int fd = -1;
    struct stat identity {};
    bool managed = false;
    QSet<QByteArray> keep;
};
} // namespace

bool writePage(const md::Document &document, const ExportRecipe &recipe, const QByteArray &page, const QString &folder,
               const Assets &assets, QString *error)
{
    Exporters::PendingFile file(recipe.output, recipe.existing);
    if (!file.write(page, error)) return false;
    if (assets.assets().isEmpty() && !QFileInfo::exists(folder)) return file.commit(error);
    PictureFolder pictures(folder, recipe.output);
    const auto publish = [&](int directory, bool locked) {
        return pictures.publish(directory, locked, assets.assets(), error) && pictures.protectInputs(document, recipe, error);
    };
    return file.commit(error, publish, [&] { pictures.retireUnused(); });
}

} // namespace ExportWeb
