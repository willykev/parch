// Include GLib before Qt defines its signals keyword.
#include <gio/gio.h>

#include "picturedata.h"
#include "svgpaper.h"

#include <QBuffer>
#include <QImageReader>
#include <QScopeGuard>
#include <QXmlStreamReader>
#include <QtEndian>

#include <algorithm>
#include <cmath>

namespace PictureData {
namespace {

QString unreadable() { return QStringLiteral("it is not a picture parch can read"); }

bool svgRoot(const QByteArray &bytes)
{
    QXmlStreamReader xml(bytes);
    return xml.readNextStartElement() && xml.name() == u"svg"
           && (xml.namespaceUri().isEmpty() || xml.namespaceUri() == u"http://www.w3.org/2000/svg");
}

// The file-type box names the image coding in its major or compatible brands.
QByteArray heifFormat(QByteArrayView data)
{
    if (data.size() < 16 || data.sliced(4, 4) != "ftyp") return {};
    quint64 length = qFromBigEndian<quint32>(data.data());
    qsizetype header = 8;
    if (length == 1) {
        length = qFromBigEndian<quint64>(data.data() + 8);
        header = 16;
    } else if (!length) {
        length = data.size();
    }
    if (length < quint64(header + 8) || length > quint64(data.size()) || length % 4) return {};
    bool heif = false;
    for (qsizetype at = header; at < qsizetype(length); at += 4) {
        if (at == header + 4) continue; // minor version
        const auto brand = data.sliced(at, 4);
        if (brand == "avif" || brand == "avis") return "avif";
        if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "hevx" || brand == "heim"
            || brand == "heis" || brand == "hevm" || brand == "hevs" || brand == "mif1" || brand == "msf1") heif = true;
    }
    return heif ? QByteArray("heif") : QByteArray();
}

QByteArray rasterFormat(QByteArrayView data)
{
    using namespace Qt::StringLiterals;
    if (data.startsWith("\x89PNG\r\n\x1a\n"_ba)) return "png";
    if (data.startsWith("\xff\xd8\xff"_ba)) return "jpeg";
    if (data.startsWith("GIF87a") || data.startsWith("GIF89a")) return "gif";
    if (data.size() >= 12 && data.startsWith("RIFF") && data.sliced(8, 4) == "WEBP") return "webp";
    if (data.startsWith("BM")) return "bmp";
    if (data.startsWith("II*\0"_ba) || data.startsWith("MM\0*"_ba)
        || data.startsWith("II+\0"_ba) || data.startsWith("MM\0+"_ba)) return "tiff";
    if (data.startsWith("\xff\x0a"_ba) || data.startsWith("\0\0\0\x0cJXL \r\n\x87\n"_ba)) return "jxl";
    return heifFormat(data);
}

QByteArray inflateSvg(const QByteArray &data, qint64 limit, QString *error)
{
    auto *memory = g_memory_input_stream_new_from_data(data.constData(), data.size(), nullptr);
    auto *gzip = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_GZIP);
    auto *stream = g_converter_input_stream_new(memory, G_CONVERTER(gzip));
    const auto close = qScopeGuard([&] { g_object_unref(stream); g_object_unref(gzip); g_object_unref(memory); });
    QByteArray out;
    char chunk[32768];
    for (;;) {
        const gssize count = g_input_stream_read(stream, chunk, sizeof chunk, nullptr, nullptr);
        if (count == 0) return out;
        if (count < 0 || count > limit - out.size()) {
            *error = count < 0 ? unreadable() : SafeRead::tooLargeReason(limit);
            return {};
        }
        out.append(chunk, count);
    }
}

} // namespace

Source read(const QByteArray &bytes, qint64 byteLimit)
{
    if (bytes.size() > byteLimit) return {{}, {}, SafeRead::tooLargeReason(byteLimit)};
    if (const auto format = rasterFormat(bytes); !format.isEmpty()) return {format, bytes, {}};
    QByteArray xml = bytes;
    if (bytes.startsWith("\x1f\x8b")) {
        QString error;
        xml = inflateSvg(bytes, byteLimit, &error);
        if (!error.isEmpty()) return {{}, {}, error};
    }
    if (svgRoot(xml)) return {"svg", xml, {}};
    return {};
}

QString Source::mediaType() const
{
    if (format.isEmpty()) return {};
    if (format == "svg") return QStringLiteral("image/svg+xml");
    return QStringLiteral("image/") + QString::fromLatin1(format);
}

QByteArray urlBytes(QStringView url, qint64 byteLimit, QString *error)
{
    error->clear();
    const qsizetype fragment = url.indexOf(u'#');
    if (fragment >= 0) url = url.first(fragment);
    const qsizetype comma = url.indexOf(u',');
    if (comma < 0 || !url.startsWith(u"data:", Qt::CaseInsensitive)) {
        *error = unreadable();
        return {};
    }
    const bool base64 = url.first(comma).endsWith(u";base64", Qt::CaseInsensitive);
    const QStringView body = url.sliced(comma + 1);
    const qsizetype capacity = base64 ? (byteLimit + 2) / 3 * 4 : byteLimit;
    QByteArray encoded;
    // Decode percent escapes before base64, and keep at most one encoded picture.
    // A refused paste never needs a second copy of the whole clipboard.
    if (body.size() <= capacity && !body.contains(u'%')) {
        encoded = body.toLatin1();
    } else {
        const auto hex = [](QChar c) {
            if (c >= u'0' && c <= u'9') return int(c.unicode() - u'0');
            c = c.toLower();
            return c >= u'a' && c <= u'f' ? int(c.unicode() - u'a' + 10) : -1;
        };
        encoded.reserve(std::min<qsizetype>({body.size(), capacity, 32768}));
        for (qsizetype i = 0; i < body.size(); ++i) {
            char c = body.at(i).toLatin1();
            if (c == '%' && i + 2 < body.size()) {
                const int high = hex(body.at(i + 1)), low = hex(body.at(i + 2));
                if (high >= 0 && low >= 0) {
                    c = char((high << 4) | low);
                    i += 2;
                }
            }
            if (base64 && (c == ' ' || c == '\n' || c == '\r' || c == '\t')) continue;
            if (encoded.size() == capacity) {
                *error = SafeRead::tooLargeReason(byteLimit);
                return {};
            }
            encoded += c;
        }
    }
    QByteArray bytes = base64 ? QByteArray::fromBase64(encoded) : std::move(encoded);
    if (bytes.size() > byteLimit) {
        *error = SafeRead::tooLargeReason(byteLimit);
        return {};
    }
    if (bytes.isEmpty()) *error = unreadable();
    return bytes;
}

bool fitsRaster(QSizeF size, qreal scale, int maxSide)
{
    size *= scale;
    return !size.isEmpty() && size.width() <= maxSide && size.height() <= maxSide
           && size.width() * size.height() <= MaxPixels;
}

QSize rasterSize(QSizeF size, qreal scale, int maxSide)
{
    if (size.isEmpty() || !std::isfinite(size.width()) || !std::isfinite(size.height())
        || !std::isfinite(scale) || scale <= 0 || maxSide <= 0) return {};
    scale = std::min(scale, std::min(maxSide, MaxSide) / std::max(size.width(), size.height()));
    size *= scale;
    if (const double area = size.width() * size.height(); area > MaxPixels)
        size *= std::sqrt(MaxPixels / area);
    QSize pixels = size.toSize().expandedTo(QSize(1, 1));
    if (qint64(pixels.width()) * pixels.height() > MaxPixels) {
        if (pixels.width() >= pixels.height()) pixels.setWidth(int(MaxPixels / pixels.height()));
        else pixels.setHeight(int(MaxPixels / pixels.width()));
    }
    return pixels;
}

Decoded decode(const Source &source, const Options &options)
{
    if (source.format.isEmpty()) return {{}, {}, source.error.isEmpty() ? unreadable() : source.error};
    const bool svg = source.format == "svg";
    QBuffer buffer;
    buffer.setData(source.bytes);
    buffer.open(QIODevice::ReadOnly);
    QByteArray format = source.format;
    if (format == "heif" && !QImageReader::supportedImageFormats().contains("heif")) format = "heic";
    QImageReader reader;
    QSizeF natural;
    if (svg) {
        natural = SvgPaper::size(source.bytes);
    } else {
        reader.setDevice(&buffer);
        reader.setFormat(format);
        reader.setAutoDetectImageFormat(false);
        reader.setDecideFormatFromContent(false);
        reader.setAutoTransform(true);
        natural = reader.size();
    }
    const QSize pixels = rasterSize(natural, svg ? options.svgScale : 1, options.maxSide);
    if (pixels.isEmpty()) return {{}, {}, unreadable()};
    if (options.refuseOversize && !fitsRaster(natural, 1, options.maxSide))
        return {{}, {}, QStringLiteral("the picture exceeds 8192 pixels on a side or 64 million pixels in all")};
    if (qint64(pixels.width()) * pixels.height() > options.pixelsLeft)
        return {{}, {}, QStringLiteral("there is no room for this picture within the total pixel limit")};

    Decoded result;
    if (svg) {
        result.image = SvgPaper::raster(source.bytes, pixels, &result.error);
    } else {
        if (pixels != natural.toSize()) reader.setScaledSize(pixels);
        result.image = reader.read();
        if (result.image.isNull()) result.error = reader.errorString();
        if (reader.transformation() & QImageIOHandler::TransformationRotate90) natural.transpose();
        if (options.refuseOversize && !result.image.isNull() && !fitsRaster(result.image.size(), 1, options.maxSide))
            return {{}, {}, QStringLiteral("the picture exceeds 8192 pixels on a side or 64 million pixels in all")};
        // Some readers resize after loading, or return their own orientation.
        const QSize bounded = rasterSize(result.image.size(), 1, options.maxSide);
        if (!result.image.isNull() && result.image.size() != bounded)
            result.image = result.image.scaled(bounded, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (qint64(result.image.width()) * result.image.height() > options.pixelsLeft)
        return {{}, {}, QStringLiteral("there is no room for this picture within the total pixel limit")};
    result.naturalSize = natural;
    result.image.setDevicePixelRatio(1);
    return result;
}

} // namespace PictureData
