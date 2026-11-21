#include "exportweb.h"

#include "documentreading.h"
#include "mdhtml.h"
#include "picturedata.h"
#include "saferead.h"
#include "svgpaper.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <algorithm>
#include <cerrno>
#include <cmath>
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
    if (mediaType == u"image/jpeg") return QStringLiteral("jpg");
    if (mediaType == u"image/svg+xml") return QStringLiteral("svg");
    return mediaType.sliced(6);
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

QString Assets::mediaType(const QByteArray &data, const QString &)
{
    return PictureData::read(data).mediaType();
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
    const QString url = picture(read.bytes, QFileInfo(absolutePath).fileName(), error);
    if (!url.isEmpty()) m_byPath.insert(absolutePath, url);
    return url;
}

namespace {

// Keep named views and the coordinates used by svgView fragments. Only the
// geometry of the views travels from the source into the new drawing.
void keepViews(const QByteArray &svg, QByteArray &drawing)
{
    static const QRegularExpression separator(QStringLiteral("[,\\s]+"));
    const auto box = [&](QStringView value) -> QRectF {
        const auto fields = value.trimmed().toString().split(separator);
        if (fields.size() != 4) return {};
        double coordinates[4];
        for (int i = 0; i < 4; ++i) {
            bool ok = false;
            coordinates[i] = fields[i].toDouble(&ok);
            if (!ok || !std::isfinite(coordinates[i])) return {};
        }
        return QRectF(coordinates[0], coordinates[1], coordinates[2], coordinates[3]);
    };
    QXmlStreamReader xml(svg);
    if (!xml.readNextStartElement()) return;
    const auto canvas = box(xml.attributes().value(u"viewBox"));
    const auto svgNamespace = xml.namespaceUri().toString();
    QByteArray views;
    QXmlStreamWriter writer(&views);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != u"view" || xml.namespaceUri() != svgNamespace) continue;
        const auto attrs = xml.attributes();
        const QString id = attrs.value(u"id").toString();
        const QRectF view = box(attrs.value(u"viewBox"));
        if (id.isEmpty() || view.isEmpty()) continue;
        writer.writeStartElement(QStringLiteral("view"));
        writer.writeAttribute(QStringLiteral("id"), id);
        QStringList fields;
        for (double v : {view.x(), view.y(), view.width(), view.height()}) fields << QString::number(v, 'g', 16);
        writer.writeAttribute(QStringLiteral("viewBox"), fields.join(u' '));
        if (attrs.hasAttribute(u"preserveAspectRatio"))
            writer.writeAttribute(QStringLiteral("preserveAspectRatio"), attrs.value(u"preserveAspectRatio").toString());
        writer.writeEndElement();
    }
    if (canvas.isValid() && !canvas.topLeft().isNull()) {
        // Cairo's viewport starts at zero. Position it within the source's
        // view box so a fragment can still select the original coordinates.
        const QString frame = QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1\" height=\"%2\" "
                                             "viewBox=\"%3 %4 %1 %2\"><g transform=\"translate(%3 %4)\">")
                                  .arg(canvas.width(), 0, 'g', 16).arg(canvas.height(), 0, 'g', 16)
                                  .arg(canvas.x(), 0, 'g', 16).arg(canvas.y(), 0, 'g', 16);
        drawing.insert(drawing.indexOf("<svg"), frame.toUtf8());
        drawing.append("</g>").append(views).append("</svg>");
    } else if (!views.isEmpty()) {
        drawing.insert(drawing.lastIndexOf("</svg>"), views);
    }
}

} // namespace

QString Assets::picture(const QByteArray &data, const QString &suggested, QString *error)
{
    const auto source = PictureData::read(data);
    if (source.format.isEmpty()) {
        *error = source.error.isEmpty() ? QStringLiteral("not a picture a page can show") : source.error;
        return {};
    }
    const QString type = source.mediaType();
    if (m_mode == Mode::Markdown) return place(data, type, suggested);
    if (source.format == "svg") {
        QByteArray plain = SvgPaper::plain(source.bytes, error);
        if (plain.isEmpty()) return {};
        keepViews(source.bytes, plain);
        return place(plain, type, suggested);
    }
    if (source.format == "png" || source.format == "jpeg" || source.format == "gif" || source.format == "webp")
        return place(data, type, suggested);
    *error = QStringLiteral("not a picture a page can show");
    return {};
}

QString Assets::bytes(const QByteArray &data, const QString &mediaType, const QString &suggested)
{
    return place(data, mediaType, suggested);
}

QString Assets::copy(const QByteArray &data, const QString &name)
{
    return place(data, QString(), name);
}

void Assets::listCopies(ExportResult &result) const
{
    if (m_mode != Mode::Copy && m_mode != Mode::Markdown) return;
    QStringList copied;
    for (const Asset &asset : m_assets) {
        const QString relative = m_folder + asset.name;
        copied.append(relative);
        result.written.append(QDir(m_outputFolder).filePath(relative));
    }
    if (!copied.isEmpty())
        result.limitations.append(QStringLiteral("Files copied beside the export: %1").arg(copied.join(QStringLiteral(", "))));
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
    // A file that is no picture keeps its own suffix: refs.json, style.csl.
    QString suffix = mediaType.isEmpty() ? info.suffix().toLower() : suffixFor(mediaType);
    if (suffix.isEmpty()) suffix = QStringLiteral("bin");
    if (mediaType == u"image/svg+xml" && data.startsWith("\x1f\x8b")) suffix = QStringLiteral("svgz");
    QString name = stem + u'.' + suffix;
    const int folder = (m_mode == Mode::Copy || m_mode == Mode::Markdown) ? openFolder(QDir(m_outputFolder).filePath(m_folder)) : -1;
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

QString formula(const ExportPictures::Picture &picture, const QString &tex, bool display, qreal fontSize, Assets *book)
{
    if (!picture.drawn || picture.svg.isEmpty() || fontSize <= 0) return {};
    const QByteArray plain = SvgPaper::plain(picture.svg);
    if (plain.isEmpty()) return {};
    const QString width = number(picture.size.width() / fontSize) + QStringLiteral("em");
    const QString height = number(picture.size.height() / fontSize) + QStringLiteral("em");
    const QString align = display ? QString() : QStringLiteral("vertical-align: -%1em").arg(number((picture.size.height() - picture.baseline) / fontSize));
    const QString label = tex.simplified();

    // Simple cairo shapes can stand inline. Drawings that need their own
    // IDs, glyph definitions, gradients or embedded pictures stay one image.
    QXmlStreamReader xml(plain);
    if (!xml.readNextStartElement()) return {};
    QString markup;
    QXmlStreamWriter writer(&markup);
    writer.writeStartElement(QStringLiteral("svg"));
    writer.writeDefaultNamespace(QStringLiteral("http://www.w3.org/2000/svg"));
    writer.writeAttribute(QStringLiteral("viewBox"), xml.attributes().value(u"viewBox").toString());
    writer.writeAttribute(QStringLiteral("width"), width);
    writer.writeAttribute(QStringLiteral("height"), height);
    writer.writeAttribute(QStringLiteral("role"), QStringLiteral("img"));
    writer.writeAttribute(QStringLiteral("aria-label"), label);
    writer.writeAttribute(QStringLiteral("class"), QStringLiteral("math-svg"));
    if (!align.isEmpty()) writer.writeAttribute(QStringLiteral("style"), align);
    writer.writeTextElement(QStringLiteral("title"), label);
    static const QSet<QString> shapes{"g", "path", "rect"};
    static const QSet<QString> attributes{"d", "x", "y", "width", "height", "rx", "ry", "transform", "fill",
        "fill-rule", "fill-opacity", "opacity", "stroke", "stroke-opacity", "stroke-width", "stroke-linecap",
        "stroke-linejoin", "stroke-miterlimit", "stroke-dasharray", "stroke-dashoffset"};
    bool inlineDrawing = true;
    while (!xml.atEnd() && inlineDrawing) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (!shapes.contains(xml.name().toString()) || xml.namespaceUri() != u"http://www.w3.org/2000/svg") {
                inlineDrawing = false;
                break;
            }
            writer.writeStartElement(xml.name().toString());
            for (const auto &attr : xml.attributes()) {
                if (!attr.namespaceUri().isEmpty() || !attributes.contains(attr.name().toString()) || attr.value().contains(u"url(")) {
                    inlineDrawing = false;
                    break;
                }
                writer.writeAttribute(attr.name().toString(), attr.value().toString());
            }
        } else if (xml.isEndElement()) {
            writer.writeEndElement();
        }
    }
    if (!inlineDrawing || xml.hasError()) {
        const QString url = book ? book->bytes(plain, QStringLiteral("image/svg+xml"), QStringLiteral("formula.svg"))
                                 : QStringLiteral("data:image/svg+xml;base64,") + QString::fromLatin1(plain.toBase64());
        const QString style = QStringLiteral("width: %1; height: %2; ").arg(width, height) + align;
        markup = QStringLiteral("<img src=\"%1\" alt=\"%2\" class=\"math-svg\" style=\"%3\" />")
                     .arg(md::escapeHtml(url), md::escapeHtml(label), style);
    }
    return display ? QStringLiteral("<div class=\"math display\">%1</div>").arg(markup)
                   : QStringLiteral("<span class=\"math\">%1</span>").arg(markup);
}

QByteArray diagramSvg(const ExportPictures::Picture &picture)
{
    return SvgPaper::plain(picture.markup.isEmpty() ? picture.svg : picture.markup);
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
