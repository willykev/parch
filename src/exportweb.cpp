#include "exportweb.h"

#include "exporter.h"
#include "mdhtml.h"
#include "saferead.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
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

Entry compareEntry(int folder, const QByteArray &name, const QByteArray &bytes)
{
    struct stat existing {};
    if (::fstatat(folder, name.constData(), &existing, AT_SYMLINK_NOFOLLOW) != 0)
        return errno == ENOENT ? Entry::Missing : Entry::Unavailable;
    if (!S_ISREG(existing.st_mode) || existing.st_size != bytes.size()) return Entry::Occupied;
    const int fd = ::openat(folder, name.constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return Entry::Occupied;
    const auto close = qScopeGuard([fd] { ::close(fd); });
    struct stat state {};
    if (::fstat(fd, &state) || !S_ISREG(state.st_mode) || state.st_size != bytes.size()) return Entry::Occupied;
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly)) return Entry::Occupied;
    char buffer[16384];
    for (qsizetype at = 0; at < bytes.size();) {
        const qint64 count = file.read(buffer, sizeof buffer);
        if (count <= 0 || count > bytes.size() - at || QByteArrayView(buffer, count) != QByteArrayView(bytes).sliced(at, count))
            return Entry::Occupied;
        at += count;
    }
    return file.read(buffer, 1) == 0 ? Entry::Same : Entry::Occupied;
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
    const auto occupied = [&] {
        return std::any_of(m_assets.cbegin(), m_assets.cend(), [&](const Asset &a) { return a.name == name; })
               || (managed && compareEntry(folder, QFile::encodeName(name), data) == Entry::Occupied);
    };
    for (int n = 2; occupied(); ++n) name = QStringLiteral("%1-%2.%3").arg(stem).arg(n).arg(suffix);
    m_assets.append({name, mediaType, data});
    const QString url = m_folder + QString::fromUtf8(QUrl::toPercentEncoding(name));
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

static bool publishAssets(const QString &target, const QList<Assets::Asset> &files, QString *error)
{
    if (files.isEmpty()) return true;
    const auto fail = [&](const QString &why) {
        *error = why;
        return false;
    };
    if (const QString why = Exporters::linkRefusal(target); !why.isEmpty()) return fail(why);
    QTemporaryDir stage(QFileInfo(target).absolutePath() + QStringLiteral("/.parch-assets-XXXXXX"));
    const QString part = stage.filePath(QStringLiteral("files"));
    if (!stage.isValid() || !QDir().mkdir(part)) return fail(QStringLiteral("cannot stage pictures for %1").arg(target));
    const auto stageFile = [&](const QString &name, const QByteArray &bytes) {
        QFile file(part + u'/' + name);
        return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(bytes) == bytes.size() && file.flush();
    };
    for (const Assets::Asset &asset : files)
        if (!stageFile(asset.name, asset.bytes)) return fail(QStringLiteral("cannot write picture %1").arg(asset.name));
    if (!stageFile(QString::fromLatin1(kFolderMark), folderSignature)) return fail(QStringLiteral("cannot mark %1").arg(target));

    if (::renameat2(AT_FDCWD, QFile::encodeName(part).constData(), AT_FDCWD, QFile::encodeName(target).constData(),
                    RENAME_NOREPLACE) == 0) return true;
    if (errno != EEXIST && errno != ENOTEMPTY) return fail(QStringLiteral("cannot install pictures in %1").arg(target));

    const int folder = openFolder(target);
    const auto close = qScopeGuard([folder] { if (folder >= 0) ::close(folder); });
    if (folder < 0 || compareEntry(folder, kFolderMark, folderSignature) != Entry::Same)
        return fail(QStringLiteral("%1 is already there and an export did not make it").arg(target));
    for (const Assets::Asset &asset : files) {
        const QByteArray name = QFile::encodeName(asset.name);
        const QByteArray staged = QFile::encodeName(part + u'/' + asset.name);
        if (::renameat2(AT_FDCWD, staged.constData(), folder, name.constData(), RENAME_NOREPLACE) == 0) continue;
        if (errno == EEXIST && compareEntry(folder, name, asset.bytes) == Entry::Same) continue;
        return fail(QStringLiteral("cannot install %1 in %2 without replacing an existing file").arg(asset.name, target));
    }
    return true;
}

bool writePage(const ExportRecipe &recipe, const QByteArray &page, const QString &folder, const Assets &assets, QString *error)
{
    Exporters::PendingFile file(recipe.output, recipe.existing);
    if (!file.write(page, error)) return false;
    if (assets.assets().isEmpty()) return file.commit(error);
    return file.commit(error, [&] { return publishAssets(folder, assets.assets(), error); });
}

} // namespace ExportWeb
