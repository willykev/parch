// librsvg and cairo come first: GLib's headers name struct members
// `signals`, which Qt defines as a keyword.
#include <cairo-svg.h>
#include <cairo.h>
#include <librsvg/rsvg.h>

#include "svgpaper.h"
#include "picturedata.h"

#include <QPainter>
#include <QSvgRenderer>
#include <QXmlStreamReader>

#include <cmath>

namespace {

struct Handle {
    RsvgHandle *handle = nullptr;
    QString error;

    explicit Handle(const QByteArray &svg)
    {
        GError *failure = nullptr;
        // Read from memory with no base: nothing outside the SVG is loaded.
        handle = rsvg_handle_new_from_data(reinterpret_cast<const guint8 *>(svg.constData()), gsize(svg.size()),
                                           &failure);
        if (!handle) {
            error = failure ? QString::fromUtf8(failure->message) : QStringLiteral("librsvg cannot read the SVG");
            if (failure) g_error_free(failure);
        }
    }
    ~Handle()
    {
        if (handle) g_object_unref(handle);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;

    // Draws the whole SVG into the rectangle; false, with why, when it cannot.
    bool render(cairo_t *cr, double width, double height)
    {
        const RsvgRectangle viewport{0, 0, width, height};
        GError *failure = nullptr;
        if (rsvg_handle_render_document(handle, cr, &viewport, &failure)) return true;
        error = failure ? QString::fromUtf8(failure->message) : QStringLiteral("librsvg cannot draw the SVG");
        if (failure) g_error_free(failure);
        return false;
    }
};

cairo_status_t append(void *closure, const unsigned char *data, unsigned int length)
{
    static_cast<QByteArray *>(closure)->append(reinterpret_cast<const char *>(data), qsizetype(length));
    return CAIRO_STATUS_SUCCESS;
}

QSizeF sizeOf(RsvgHandle *handle)
{
    if (!handle) return {};
    gboolean hasViewBox = false;
    RsvgRectangle box{};
    RsvgLength w{}, h{};
    rsvg_handle_get_intrinsic_dimensions(handle, nullptr, &w, nullptr, &h, &hasViewBox, &box);
    if (hasViewBox) return QSizeF(box.width, box.height);
    if (w.length <= 0 || h.length <= 0) return {};
    gdouble width = 0, height = 0;
    if (rsvg_handle_get_intrinsic_size_in_pixels(handle, &width, &height))
        return QSizeF(width, height);
    // An embedded SVG with no intrinsic size uses a 300 by 150 viewport.
    // With one definite length, let librsvg resolve its units and font.
    if (w.unit == RSVG_UNIT_PERCENT && h.unit == RSVG_UNIT_PERCENT) return QSizeF(300, 150);
    const QByteArray css = w.unit == RSVG_UNIT_PERCENT ? QByteArrayLiteral(":root{width:300px!important}")
                                                      : QByteArrayLiteral(":root{height:150px!important}");
    if (!rsvg_handle_set_stylesheet(handle, reinterpret_cast<const guint8 *>(css.constData()), css.size(), nullptr))
        return {};
    const bool measured = rsvg_handle_get_intrinsic_size_in_pixels(handle, &width, &height);
    rsvg_handle_set_stylesheet(handle, reinterpret_cast<const guint8 *>(""), 0, nullptr);
    return measured ? QSizeF(width, height) : QSizeF{};
}

} // namespace

namespace SvgPaper {

bool valid(const QByteArray &svg)
{
    return Handle(svg).handle != nullptr;
}

QSizeF size(const QByteArray &svg)
{
    Handle handle(svg);
    return sizeOf(handle.handle);
}

QByteArray plain(const QByteArray &svg, QString *error)
{
    Handle handle(svg);
    if (!handle.handle) {
        if (error) *error = handle.error;
        return {};
    }
    const QSizeF size = sizeOf(handle.handle);
    if (!size.isValid() || size.isEmpty()) {
        if (error) *error = QStringLiteral("the SVG gives itself no size");
        return {};
    }
    QByteArray out;
    cairo_surface_t *surface = cairo_svg_surface_create_for_stream(append, &out, size.width(), size.height());
    // User units are CSS pixels, as in the drawing it came from.
    cairo_svg_surface_set_document_unit(surface, CAIRO_SVG_UNIT_PX);
    cairo_t *cr = cairo_create(surface);
    const bool drawn = handle.render(cr, size.width(), size.height());
    cairo_destroy(cr);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (!drawn || status != CAIRO_STATUS_SUCCESS) {
        if (error)
            *error = !drawn ? handle.error : QString::fromUtf8(cairo_status_to_string(status));
        return {};
    }
    return out;
}

QImage raster(const QByteArray &svg, QSizeF requested, QString *error)
{
    Handle handle(svg);
    if (!handle.handle) {
        if (error) *error = handle.error;
        return {};
    }
    const QSize pixels = PictureData::rasterSize(requested);
    if (pixels.isEmpty()) {
        if (error) *error = QStringLiteral("no size to draw the SVG at");
        return {};
    }
    // cairo's ARGB32 is QImage's premultiplied ARGB32, word for word.
    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("the picture is too large");
        return {};
    }
    image.fill(Qt::transparent);
    cairo_surface_t *surface = cairo_image_surface_create_for_data(image.bits(), CAIRO_FORMAT_ARGB32, image.width(),
                                                                   image.height(), int(image.bytesPerLine()));
    cairo_t *cr = cairo_create(surface);
    const bool drawn = handle.render(cr, image.width(), image.height());
    const cairo_status_t status = cairo_status(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    if (!drawn || status != CAIRO_STATUS_SUCCESS) {
        if (error) *error = drawn ? QString::fromUtf8(cairo_status_to_string(status)) : handle.error;
        return {};
    }
    return image;
}

qint64 embeddedPixels(const QByteArray &plain)
{
    if (!plain.contains("<image")) return 0;
    QXmlStreamReader xml(plain);
    qint64 total = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != u"image") continue;
        const auto attrs = xml.attributes();
        const QSize size(attrs.value(u"width").toInt(), attrs.value(u"height").toInt());
        if (!PictureData::fitsRaster(size)) return -1;
        total += qint64(size.width()) * size.height();
    }
    return xml.hasError() ? -1 : total;
}

double difference(const QByteArray &svg, const QImage &reference)
{
    if (reference.isNull() || embeddedPixels(svg) < 0) return 1;
    // Small enough to be quick, large enough for labels to count.
    const QSize size = reference.size().scaled(QSize(480, 480), Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
    const QImage expected = reference.convertToFormat(QImage::Format_ARGB32)
                                .scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QSvgRenderer renderer(svg);
    if (!renderer.isValid()) return 1;
    // On the reference's own ground: its corner.
    QImage drawn(size, QImage::Format_ARGB32);
    drawn.fill(expected.pixelColor(0, 0));
    {
        QPainter painter(&drawn);
        painter.setRenderHint(QPainter::Antialiasing);
        renderer.render(&painter, QRectF(QPointF(0, 0), QSizeF(size)));
    }
    constexpr int cell = 4;
    int cells = 0, unlike = 0;
    for (int y = 0; y + cell <= size.height(); y += cell) {
        for (int x = 0; x + cell <= size.width(); x += cell) {
            long a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
            for (int dy = 0; dy < cell; ++dy) {
                const QRgb *e = reinterpret_cast<const QRgb *>(expected.constScanLine(y + dy)) + x;
                const QRgb *d = reinterpret_cast<const QRgb *>(drawn.constScanLine(y + dy)) + x;
                for (int dx = 0; dx < cell; ++dx) {
                    a[0] += qRed(e[dx]), a[1] += qGreen(e[dx]), a[2] += qBlue(e[dx]);
                    b[0] += qRed(d[dx]), b[1] += qGreen(d[dx]), b[2] += qBlue(d[dx]);
                }
            }
            long delta = 0;
            for (int c = 0; c < 3; ++c) delta += std::labs(a[c] - b[c]);
            ++cells;
            // A clear difference: over 48 levels a channel on average.
            if (delta > 48L * 3 * cell * cell) ++unlike;
        }
    }
    return cells ? double(unlike) / cells : 1;
}

QString version()
{
    return QStringLiteral("librsvg %1.%2.%3, cairo %4")
        .arg(LIBRSVG_MAJOR_VERSION)
        .arg(LIBRSVG_MINOR_VERSION)
        .arg(LIBRSVG_MICRO_VERSION)
        .arg(QString::fromLatin1(cairo_version_string()));
}

} // namespace SvgPaper
