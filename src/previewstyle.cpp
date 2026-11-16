#include "previewstyle.h"

#include "theme.h"

#include <QFontDatabase>
#include <QTextFormat>
#include <algorithm>
#include <cmath>

const int PreviewStyle::InkProperty = QTextFormat::UserProperty + 0x501;
const int PreviewStyle::PaperProperty = QTextFormat::UserProperty + 0x502;

namespace {
const QColor black(0, 0, 0), white(255, 255, 255);

// The end of the scale that stands out most from a background.
QColor farthestFrom(const QColor &background)
{
    return PreviewStyle::contrast(black, background) >= PreviewStyle::contrast(white, background) ? black : white;
}
} // namespace

qreal PreviewStyle::contrast(const QColor &a, const QColor &b)
{
    const double first = Theme::luminance(a), second = Theme::luminance(b);
    return (std::max(first, second) + 0.05) / (std::min(first, second) + 0.05);
}

QColor PreviewStyle::readable(const QColor &color, const QColor &background, const QColor &towards, qreal minimum)
{
    if (contrast(color, background) >= minimum) return color;
    constexpr int steps = 20;
    for (int step = 1; step <= steps; ++step) {
        const QColor mixed = Theme::mix(color, towards, double(step) / steps);
        if (contrast(mixed, background) >= minimum) return mixed;
    }
    // Not even `towards` stands out enough: on towards black or white.
    const QColor end = farthestFrom(background);
    for (int step = 1; step < steps; ++step) {
        const QColor mixed = Theme::mix(towards, end, double(step) / steps);
        if (contrast(mixed, background) >= minimum) return mixed;
    }
    return end;
}

PreviewStyle PreviewStyle::screen(const Theme &theme, const QString &family, qreal size, qreal lineHeight)
{
    PreviewStyle style;
    style.family = family.isEmpty() ? defaultFamily() : family;
    style.codeFamily = theme.fontFamily();
    style.size = size;
    style.lineHeight = lineHeight;
    style.dark = theme.dark();
    const QColor background = theme.background();
    const QColor end = farthestFrom(background);
    style.background = background;
    // The text itself reads on the page whatever a theme makes of its colours.
    style.text = readable(theme.foreground(), background, end);
    style.accent = theme.accent();
    style.rule = Theme::mix(background, style.text, 0.24);
    style.quoteBar = theme.accent();
    style.selection = theme.selection();

    const QColor code = Theme::mix(background, style.text, 0.07);
    const QColor mark = Theme::mix(background, theme.warning(), 0.42);
    style.papers[CodePaper] = code;
    style.papers[MarkPaper] = mark;

    const auto on = [&style](const QColor &color, const QColor &behind) {
        return readable(color, behind, style.text);
    };
    style.inks[Heading] = on(theme.bright(), background);
    style.inks[Dim] = on(theme.dim(), background);
    style.inks[Faint] = readable(theme.faint(), background, style.text, 3);
    style.inks[Link] = on(theme.accent(), background);
    style.inks[Code] = on(style.text, code);
    style.inks[Marked] = readable(style.text, mark, farthestFrom(mark));
    style.inks[Danger] = on(theme.danger(), background);
    // Code is coloured from the theme's own colours, each kept readable on
    // the tint behind code.
    const QColor accent = theme.accent(), good = theme.good(), warning = theme.warning(), danger = theme.danger();
    style.inks[Keyword] = on(accent, code);
    style.inks[Type] = on(Theme::mix(accent, good, 0.5), code);
    style.inks[String] = on(good, code);
    style.inks[Number] = on(warning, code);
    style.inks[Comment] = on(theme.dim(), code);
    style.inks[Function] = on(Theme::mix(accent, style.text, 0.45), code);
    style.inks[Variable] = on(Theme::mix(style.text, danger, 0.4), code);
    style.inks[Operator] = on(style.text, code);
    style.inks[Preprocessor] = on(Theme::mix(warning, danger, 0.5), code);
    style.inks[Constant] = on(Theme::mix(warning, accent, 0.4), code);
    return style;
}

PreviewStyle PreviewStyle::paper()
{
    PreviewStyle style;
    style.family = defaultFamily();
    style.codeFamily = QStringLiteral("monospace");
    style.size = 16;
    style.lineHeight = 1.5;
    style.dark = false;
    style.background = white;
    style.text = QColor(26, 26, 26);
    style.accent = QColor(31, 79, 153);
    style.rule = QColor(190, 190, 190);
    style.quoteBar = QColor(150, 150, 150);
    style.selection = QColor(200, 220, 250);
    style.papers[CodePaper] = QColor(244, 244, 244);
    style.papers[MarkPaper] = QColor(255, 240, 150);
    style.inks[Heading] = black;
    style.inks[Dim] = QColor(90, 90, 90);
    style.inks[Faint] = QColor(140, 140, 140);
    style.inks[Link] = style.accent;
    style.inks[Code] = style.text;
    style.inks[Marked] = style.text;
    style.inks[Danger] = QColor(160, 30, 30);
    style.inks[Keyword] = QColor(31, 79, 153);
    style.inks[Type] = QColor(20, 110, 110);
    style.inks[String] = QColor(30, 110, 40);
    style.inks[Number] = QColor(140, 80, 0);
    style.inks[Comment] = QColor(105, 105, 105);
    style.inks[Function] = QColor(70, 60, 140);
    style.inks[Variable] = QColor(120, 50, 50);
    style.inks[Operator] = style.text;
    style.inks[Preprocessor] = QColor(150, 60, 20);
    style.inks[Constant] = QColor(120, 70, 120);
    return style;
}

PreviewStyle PreviewStyle::withColorsOf(const PreviewStyle &colors) const
{
    PreviewStyle style = colors;
    style.family = family;
    style.codeFamily = codeFamily;
    style.size = size;
    style.lineHeight = lineHeight;
    style.scale = scale;
    return style;
}

bool PreviewStyle::sameTypography(const PreviewStyle &other) const
{
    return family == other.family && codeFamily == other.codeFamily && size == other.size
           && lineHeight == other.lineHeight && scale == other.scale;
}

bool PreviewStyle::sameColors(const PreviewStyle &other) const
{
    return dark == other.dark && text == other.text && background == other.background && accent == other.accent
           && rule == other.rule && quoteBar == other.quoteBar && selection == other.selection
           && std::equal(std::begin(inks), std::end(inks), std::begin(other.inks))
           && std::equal(std::begin(papers), std::end(papers), std::begin(other.papers));
}

QFont PreviewStyle::bodyFont() const
{
    QFont font(family);
    font.setPixelSize(std::max(1, int(std::lround(size))));
    return font;
}

QFont PreviewStyle::codeFont() const
{
    QFont font(codeFamily);
    font.setPixelSize(std::max(1, int(std::lround(codeSize()))));
    return font;
}

qreal PreviewStyle::headingSize(int level) const
{
    static constexpr qreal ratios[] = {2.0, 1.5, 1.25, 1.1, 1.0, 0.92};
    return std::round(size * ratios[std::clamp(level, 1, 6) - 1]);
}

qreal PreviewStyle::lineSpacing(qreal fontSize) const
{
    // Large type is set tighter than text, as headings are.
    const qreal multiple = fontSize > size * 1.2 ? std::min(lineHeight, 1.25) : lineHeight;
    return std::round(fontSize * multiple);
}

QString PreviewStyle::defaultFamily(const QStringList &installed)
{
    static const char *const faces[] = {
        "Literata",         "Source Serif 4",   "Source Serif Pro", "Charter",      "Bitstream Charter",
        "XCharter",         "Libertinus Serif", "Linux Libertine O", "Crimson Pro",  "EB Garamond",
        "Noto Serif",       "C059",             "DejaVu Serif",     "Liberation Serif",
    };
    for (const char *face : faces) {
        const QString family = QString::fromLatin1(face);
        if (installed.contains(family, Qt::CaseInsensitive)) return family;
    }
    return QStringLiteral("serif");
}

QString PreviewStyle::defaultFamily()
{
    static const QString family = defaultFamily(QFontDatabase::families());
    return family;
}
