#pragma once

#include <QByteArray>
#include <QColor>
#include <QList>
#include <QMetaType>
#include <QSizeF>
#include <QString>

class Theme;

// The colours a formula or diagram is drawn in, taken from the live theme so
// that it sits on the page in light and dark themes alike.
struct RenderColors {
    bool dark = true;
    QColor foreground{"#a9b1d6"};
    QColor background{"#1a1b26"};
    QColor accent{"#7aa2f7"};
    QColor muted{"#414868"};
    // Behind code, and inside diagram boxes.
    QColor surface{"#24283b"};
    // Chart series, slices, branches and sections, in order. Empty: made from
    // the accent and the theme's other colours.
    QList<QColor> series;

    static RenderColors fromTheme(const Theme &theme);
    // Every colour, in a fixed order: what the cache keys on.
    QString identity() const;
    bool operator==(const RenderColors &other) const = default;
};

enum class RenderKind {
    InlineMath,  // a formula on a line of text: $...$
    DisplayMath, // a formula on a line of its own: $$...$$
    Diagram,     // a fenced mermaid block
};

enum class RenderPurpose {
    Preview, // on screen, at the display's pixel density
    Print,   // into a PDF or onto paper
};

struct RenderRequest {
    RenderKind kind = RenderKind::DisplayMath;
    RenderPurpose purpose = RenderPurpose::Preview;
    // The formula without its dollar signs, or the diagram without its fence.
    QString source;
    RenderColors colors;
    // The size of the surrounding text, in logical pixels.
    qreal fontSize = 16;
    // Device pixels per logical pixel.
    qreal scale = 1;
    // The family diagram labels are set in; empty takes the desktop's sans-serif.
    QString fontFamily;
};

struct RenderResult {
    enum Status {
        Rendered,
        // The helper read the source and refused it: a TeX or Mermaid error,
        // in the helper's own words in `diagnostic`.
        Refused,
        // The helper is missing, could not run, ran too long or wrote too much.
        Failed,
    };
    Status status = Failed;
    // "svg" or "png".
    QString format;
    QByteArray data;
    // A diagram drawn for print also keeps the SVG it was drawn from, in the
    // request's colours: Qt cannot draw it as it is, but once librsvg has
    // made it plain (SvgPaper::plain) a PDF can hold it as vectors.
    QByteArray svg;
    // On the page, in logical pixels.
    QSizeF size;
    // From the top of a formula down to the baseline it sits on, in logical
    // pixels; the rest of its height hangs below the line.
    qreal baseline = 0;
    // Device pixels per logical pixel of a PNG; 1 for SVG.
    qreal scale = 1;
    // Whether the artifact stays sharp at any size.
    bool vector = false;
    // What the formula or diagram says, for assistive technology and for
    // alt text in exports. Kept when the artifact cannot be drawn.
    QString description;
    QString diagnostic;
    // How the artifact differs from what was asked, when it does.
    QString note;
    bool cached = false;
    // Wall time of the helper runs behind it; 0 when it came from the cache.
    qint64 elapsedMs = 0;
    // Whether every helper run behind it was cut off from the network.
    bool networkIsolated = false;

    bool ok() const { return status == Rendered; }
};

Q_DECLARE_METATYPE(RenderResult)
