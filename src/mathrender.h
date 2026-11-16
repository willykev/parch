#pragma once

#include "renderhelpers.h"
#include "renderrequest.h"

#include <QByteArray>
#include <QSizeF>
#include <QString>

// TeX math through RaTeX's render-svg, which draws one formula per input
// line as SVG that Qt's SVG renderer draws: glyph outlines for everything in
// the KaTeX fonts, text elements for scripts those fonts lack, and pictures
// for emoji.
//
// The command-line tool reports no baseline, so each formula is drawn twice
// in the same run: as it is, and followed by an invisible rule of zero
// height reaching a known depth far below the line. The second picture is
// the formula's height plus that depth, which gives the height; the first is
// height plus depth, which gives the depth. Both pictures are boxes exactly
// as RaTeX laid them out, with no padding.
namespace MathRender {

// The depth, in ems, of the rule that measures the baseline: deeper than any
// formula reaches.
inline constexpr int kRuleDepthEm = 100;

// The formula as one line, as the tool reads input: TeX comments taken out
// and line breaks made spaces, which is what TeX makes of them.
QString oneLine(const QString &tex);

// The run that draws the request with the helper at `program`.
HelperRun plan(const RenderRequest &request, const QString &program);

// The result of a run that finished, given the request it drew.
RenderResult read(const HelperOutcome &outcome, const RenderRequest &request);

// An SVG's view box size, or an empty size when it has none.
QSizeF viewBoxSize(const QByteArray &svg);

} // namespace MathRender
