#pragma once

#include "renderhelpers.h"
#include "renderrequest.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

// Mermaid diagrams through Merman's merman-cli. Each diagram is drawn twice:
// once as SVG, which gives its size in logical pixels and the words its
// description is made of, and once as PNG at the scale asked for, which is
// what parch shows, since Qt's SVG renderer cannot draw Merman's SVG (it
// needs CSS, nested SVG and baselines Qt does not have).
//
// Two families keep colours of their own whatever the theme: ZenUML a whole
// stylesheet, and C4 its relations and arrowheads (its boxes take colours
// from the configuration). Merman takes no CSS of its own while it measures
// text with real fonts, so their SVG is recoloured from the request's colours
// and the second run turns that SVG into the PNG instead of drawing the
// source again.
//
// For print the PNG is drawn at four times the scale or more. Merman's
// resvg-safe SVG drawn by Qt's SVG renderer into a PDF loses its labels and
// most of its colours (see diagramPrintRouteIsReported in the tests), so the
// result for print also keeps that SVG (RenderResult::svg), which exports
// make plain with librsvg (SvgPaper) before a PDF holds it as vectors.
namespace DiagramRender {

// The fonts Merman measures and draws labels with: the label family in its
// four styles first, then one font for each script that family lacks (Chinese,
// Japanese and Korean in one regular-weight collection, Hebrew, Arabic,
// Devanagari, Thai, Greek, Cyrillic) and an emoji font, as fontconfig finds
// them. Merman refuses to draw when the fonts it is given hold more than
// 64 MiB, so a font that would take the set past kFontBudget is left out.
struct Fonts {
    QString family;
    QStringList files;
    qint64 bytes = 0;
    // Fonts left out to stay inside the budget.
    QStringList omitted;
    // Every file with its size and time: what the cache keys on.
    QString identity;
};
inline constexpr qint64 kFontBudget = 48 * 1024 * 1024;
Fonts chooseFonts(const QString &family);

// Mermaid's configuration for the request: its base theme, with every theme
// variable that colours a diagram family set from the request's colours.
QByteArray configuration(const RenderRequest &request, const QString &family);

// The scale a PNG for print is drawn at: never below this.
inline constexpr qreal kPrintScale = 4;

// The runs that draw the request with the helper at `program`: the SVG run,
// then the PNG run.
QList<HelperRun> plan(const RenderRequest &request, const QString &program, const Fonts &fonts);

// The kind of diagram the source declares: its first word after any front
// matter and comments, such as "flowchart", "zenuml" or "C4Context".
QString declaredKind(const QString &source);

// Whether Merman's SVG for the source needs recolouring before it is drawn:
// ZenUML and C4 diagrams.
bool needsRestyle(const QString &source);

// Merman's SVG of a ZenUML or C4 diagram in the request's colours and label
// family; any other SVG as it is.
QByteArray restyle(const QByteArray &svg, const RenderRequest &request, const QString &family);

// The result of both runs, which finished.
RenderResult read(const HelperOutcome &svg, const HelperOutcome &png, const RenderRequest &request);

// What the diagram says, from Merman's SVG: its accessible title and
// description (accTitle, accDescr) when the source gives them, otherwise
// the kind of diagram and the words on it.
QString describe(const QByteArray &svg);

// The kind of diagram Merman reports, in words: "flowchart-v2" is "Flowchart".
QString kindName(const QString &roleDescription);

} // namespace DiagramRender
