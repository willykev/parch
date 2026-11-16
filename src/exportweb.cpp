#include "exportweb.h"

#include "mdhtml.h"
#include "saferead.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>

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

QString Assets::file(const QString &absolutePath, QString *error)
{
    if (m_mode == Mode::Link)
        return QString::fromUtf8(QUrl::toPercentEncoding(Exporters::relativeTo(m_outputFolder, absolutePath), "/"));
    if (const auto known = m_byPath.constFind(absolutePath); known != m_byPath.cend()) return *known;
    const SafeRead::Result read = SafeRead::read(absolutePath, SafeRead::PictureBytes);
    if (!read.ok) {
        if (error) *error = read.error;
        return {};
    }
    const QByteArray &data = read.bytes;
    const QString type = mediaType(data, absolutePath);
    if (type.isEmpty()) {
        if (error) *error = QStringLiteral("not a picture a page can show");
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
    for (int n = 2; std::any_of(m_assets.cbegin(), m_assets.cend(), [&](const Asset &a) { return a.name == name; }); ++n)
        name = QStringLiteral("%1-%2.%3").arg(stem).arg(n).arg(suffix);
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

bool replaceFolder(const QString &target, const QList<Assets::Asset> &files, QString *error)
{
    const QFileInfo info(target);
    const QString mark = QString::fromLatin1(kFolderMark);
    if (info.exists() && (!info.isDir() || !QFileInfo::exists(target + u'/' + mark))) {
        *error = QStringLiteral("%1 is already there and an export did not make it; parch leaves it alone").arg(target);
        return false;
    }
    if (files.isEmpty()) return true;
    const QString tag = QString::number(QRandomGenerator::global()->generate(), 16);
    const QString part = target + QStringLiteral(".part-") + tag;
    QDir dir;
    if (!dir.mkpath(part)) {
        *error = QStringLiteral("cannot make the folder %1").arg(part);
        return false;
    }
    const auto fail = [&](const QString &why) {
        QDir(part).removeRecursively();
        *error = why;
        return false;
    };
    for (const Assets::Asset &asset : files) {
        QFile file(part + u'/' + asset.name);
        if (!file.open(QIODevice::WriteOnly) || file.write(asset.bytes) != asset.bytes.size() || !file.flush())
            return fail(QStringLiteral("cannot write %1: %2").arg(file.fileName(), file.errorString()));
    }
    QFile marker(part + u'/' + mark);
    if (!marker.open(QIODevice::WriteOnly) || marker.write("parch\n") != 6 || !marker.flush())
        return fail(QStringLiteral("cannot write %1").arg(marker.fileName()));
    marker.close();
    const QString old = target + QStringLiteral(".old-") + tag;
    const bool had = info.exists();
    if (had && !dir.rename(target, old)) return fail(QStringLiteral("cannot replace %1").arg(target));
    if (!dir.rename(part, target)) {
        if (had) dir.rename(old, target);
        return fail(QStringLiteral("cannot replace %1").arg(target));
    }
    if (had) QDir(old).removeRecursively();
    return true;
}

} // namespace ExportWeb
