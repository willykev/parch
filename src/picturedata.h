#pragma once

#include "saferead.h"

#include <QImage>
#include <QStringView>

// The picture readers shared by the preview, paste and exports. Only the
// bytes choose the format. SVG is always opened in memory with no base file.
namespace PictureData {

inline constexpr int MaxSide = 8192;
inline constexpr qint64 MaxPixels = 64'000'000;

struct Source {
    QByteArray format; // the raster reader's name, or "svg"
    QByteArray bytes;  // uncompressed for SVG
    QString error;
    QString mediaType() const;
};

Source read(const QByteArray &bytes, qint64 byteLimit = SafeRead::PictureBytes);
QByteArray urlBytes(QStringView url, qint64 byteLimit, QString *error);

// Keeps the aspect ratio, applying the bounds before rounding to integers.
QSize rasterSize(QSizeF size, qreal scale = 1, int maxSide = MaxSide);
bool fitsRaster(QSizeF size, qreal scale = 1, int maxSide = MaxSide);

struct Options {
    int maxSide = MaxSide;
    qreal svgScale = 1;
    bool refuseOversize = false;
    qint64 pixelsLeft = MaxPixels;
};

struct Decoded {
    QImage image;
    QSizeF naturalSize;
    QString error;
};
Decoded decode(const Source &source, const Options &options = {});

} // namespace PictureData
