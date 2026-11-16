// librsvg and cairo come first: GLib's headers name struct members
// `signals`, which Qt defines as a keyword.
#include <cairo-svg.h>
#include <cairo.h>
#include <librsvg/rsvg.h>

#include "svgpaper.h"

#include <QPainter>
#include <QRegularExpression>
#include <QSvgRenderer>

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

QSizeF sizeOf(const QByteArray &svg, RsvgHandle *handle)
{
    const QSizeF given = SvgPaper::size(svg);
    if (given.isValid() && !given.isEmpty()) return given;
    if (!handle) return {};
    gdouble width = 0, height = 0;
    if (rsvg_handle_get_intrinsic_size_in_pixels(handle, &width, &height) && width > 0 && height > 0)
        return QSizeF(width, height);
    return {};
}

} // namespace

namespace SvgPaper {

QSizeF size(const QByteArray &svg)
{
    // The root element's attributes, wherever its tag ends.
    const qsizetype open = svg.indexOf("<svg");
    if (open < 0) return {};
    const qsizetype close = svg.indexOf('>', open);
    const QString tag = QString::fromUtf8(svg.mid(open, close < 0 ? -1 : close - open));
    static const QRegularExpression viewBox(QStringLiteral("viewBox\\s*=\\s*[\"']\\s*([-0-9.eE]+)[\\s,]+([-0-9.eE]+)"
                                                           "[\\s,]+([-0-9.eE]+)[\\s,]+([-0-9.eE]+)"));
    if (const QRegularExpressionMatch match = viewBox.match(tag); match.hasMatch()) {
        const QSizeF size(match.captured(3).toDouble(), match.captured(4).toDouble());
        if (size.width() > 0 && size.height() > 0) return size;
    }
    static const QRegularExpression width(QStringLiteral("\\swidth\\s*=\\s*[\"']\\s*([0-9.]+)(px)?\\s*[\"']"));
    static const QRegularExpression height(QStringLiteral("\\sheight\\s*=\\s*[\"']\\s*([0-9.]+)(px)?\\s*[\"']"));
    const QRegularExpressionMatch w = width.match(tag), h = height.match(tag);
    if (w.hasMatch() && h.hasMatch()) return QSizeF(w.captured(1).toDouble(), h.captured(1).toDouble());
    return {};
}

QByteArray plain(const QByteArray &svg, QString *error)
{
    Handle handle(svg);
    if (!handle.handle) {
        if (error) *error = handle.error;
        return {};
    }
    const QSizeF size = sizeOf(svg, handle.handle);
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

QImage raster(const QByteArray &svg, const QSize &pixels, QString *error)
{
    Handle handle(svg);
    if (!handle.handle) {
        if (error) *error = handle.error;
        return {};
    }
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
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    cairo_surface_destroy(surface);
    if (!drawn) {
        if (error) *error = handle.error;
        return {};
    }
    return image;
}

double difference(const QByteArray &svg, const QImage &reference)
{
    if (reference.isNull()) return 1;
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
