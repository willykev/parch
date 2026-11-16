#pragma once

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QString>

// SVG for paper and packages, through librsvg and cairo, in process.
//
// Merman's diagrams are SVG that needs CSS, nested SVG and text baselines,
// which Qt's SVG renderer does not have: drawn by Qt into a PDF they lose
// their labels and most of their colours. librsvg draws them as a browser
// would, and cairo writes what it drew as plain SVG again (paths, glyph
// outlines and pictures, with no CSS), which Qt draws correctly into a PDF
// as vectors. The same pair draws any SVG as pixels where a format needs a
// picture, such as a formula in a Word document.
//
// Everything here is a pure function of its arguments and may run on any
// thread.
namespace SvgPaper {

// The SVG as plain SVG, the same size, in pixels; empty, with why, when
// librsvg cannot read it.
QByteArray plain(const QByteArray &svg, QString *error = nullptr);

// The SVG drawn in `pixels`, on a transparent ground; a null image, with
// why, when librsvg cannot read it.
QImage raster(const QByteArray &svg, const QSize &pixels, QString *error = nullptr);

// The size the SVG gives itself, in CSS pixels: its view box, else its
// width and height; empty when it gives none.
QSizeF size(const QByteArray &svg);

// How unlike `reference` Qt's own SVG renderer draws `svg`, from 0 (alike)
// to 1: the share of small cells whose colour differs clearly, both drawn
// at the reference's size on its background. A diagram whose plain SVG Qt
// draws unlike Merman's own picture is printed as that picture instead.
double difference(const QByteArray &svg, const QImage &reference);
// Above this, a drawing is unlike its reference.
inline constexpr double kUnlike = 0.08;

// The libraries' versions, as exports report them: "librsvg 2.63.3, cairo 1.18.6".
QString version();

} // namespace SvgPaper
