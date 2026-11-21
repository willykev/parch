#include "diagramrender.h"

#include "mathrender.h"
#include "theme.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QXmlStreamReader>

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cmath>

namespace {

QString hex(const QColor &color) { return color.name(QColor::HexRgb); }

QString number(qreal value) { return QString::number(value, 'g', 8); }

// WCAG contrast between two colours.
double contrast(const QColor &a, const QColor &b)
{
    const double one = Theme::luminance(a), two = Theme::luminance(b);
    return (std::max(one, two) + .05) / (std::min(one, two) + .05);
}

// Whichever of the two reads better on the fill.
QColor ink(const QColor &fill, const QColor &one, const QColor &two)
{
    return contrast(fill, one) >= contrast(fill, two) ? one : two;
}

// The colour, darkened as far as white words on it need to be read.
QColor readableUnderWhite(const QColor &color)
{
    QColor darker = color;
    for (int step = 1; step <= 10 && contrast(darker, Qt::white) < 4.5; ++step)
        darker = Theme::mix(color, Qt::black, step / 10.0);
    return darker;
}

// Series colours when the request brings none: the accent turned round the
// colour wheel.
QList<QColor> seriesFor(const RenderColors &colors)
{
    if (!colors.series.isEmpty()) return colors.series;
    QList<QColor> series{colors.accent};
    const QColor accent = colors.accent.toHsl();
    for (int step = 1; step < 8; ++step) {
        const int hue = (std::max(accent.hslHue(), 0) + step * 45) % 360;
        series << QColor::fromHsl(hue, accent.hslSaturation(), accent.lightness());
    }
    return series;
}

QString fcString(FcPattern *pattern, const char *object)
{
    FcChar8 *value = nullptr;
    if (!pattern || FcPatternGetString(pattern, object, 0, &value) != FcResultMatch || !value) return {};
    return QString::fromUtf8(reinterpret_cast<const char *>(value));
}

// Whether the diagram helper can read a font file: TrueType or OpenType as
// it is on disk, not packed for the web (WOFF, WOFF2), which fontconfig
// lists beside them and may well match first.
bool readableFont(const FcPattern *pattern)
{
    FcChar8 *path = nullptr;
    if (FcPatternGetString(pattern, FC_FILE, 0, &path) != FcResultMatch || !path) return false;
    QFile file(QFile::decodeName(reinterpret_cast<const char *>(path)));
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray magic = file.read(4);
    return magic != "wOFF" && magic != "wOF2";
}

// The font fontconfig gives for a family in one style, or the next best that
// the helper can read. The caller destroys it.
FcPattern *matchStyle(const QString &family, int weight, int slant)
{
    FcPattern *pattern = FcPatternCreate();
    const QByteArray name = family.toUtf8();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(name.constData()));
    FcPatternAddInteger(pattern, FC_WEIGHT, weight);
    FcPatternAddInteger(pattern, FC_SLANT, slant);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcPattern *match = FcFontMatch(nullptr, pattern, &result);
    if (match && !readableFont(match)) {
        FcPatternDestroy(match);
        match = nullptr;
        if (FcFontSet *sorted = FcFontSort(nullptr, pattern, FcFalse, nullptr, &result)) {
            for (int i = 0; i < sorted->nfont && !match; ++i)
                if (readableFont(sorted->fonts[i])) match = FcFontRenderPrepare(nullptr, pattern, sorted->fonts[i]);
            FcFontSetDestroy(sorted);
        }
    }
    FcPatternDestroy(pattern);
    return match;
}

// The best regular font that has the character, preferring the family and
// language given. The caller destroys it.
FcPattern *coverFor(char32_t character, const char *language, const char *family)
{
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(family));
    if (language) FcPatternAddString(pattern, FC_LANG, reinterpret_cast<const FcChar8 *>(language));
    FcCharSet *characters = FcCharSetCreate();
    FcCharSetAddChar(characters, character);
    FcPatternAddCharSet(pattern, FC_CHARSET, characters);
    FcCharSetDestroy(characters);
    FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_REGULAR);
    FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result = FcResultNoMatch;
    FcFontSet *sorted = FcFontSort(nullptr, pattern, FcFalse, nullptr, &result);
    FcPatternDestroy(pattern);
    FcPattern *found = nullptr;
    if (sorted) {
        for (int i = 0; i < sorted->nfont && !found; ++i) {
            FcCharSet *has = nullptr;
            if (FcPatternGetCharSet(sorted->fonts[i], FC_CHARSET, 0, &has) == FcResultMatch &&
                FcCharSetHasChar(has, character) && readableFont(sorted->fonts[i])) {
                FcPatternReference(sorted->fonts[i]);
                found = sorted->fonts[i];
            }
        }
        FcFontSetDestroy(sorted);
    }
    return found;
}

} // namespace

namespace DiagramRender {

Fonts chooseFonts(const QString &requested)
{
    Fonts fonts;
    FcInit();
    QList<FcCharSet *> covered;
    QStringList identity;
    auto take = [&](FcPattern *pattern) {
        const QString file = fcString(pattern, FC_FILE);
        const QFileInfo info(file);
        if (file.isEmpty() || fonts.files.contains(file) || !info.isFile()) return;
        if (fonts.bytes + info.size() > kFontBudget) {
            if (!fonts.omitted.contains(file)) fonts.omitted << file;
            return;
        }
        fonts.files << file;
        fonts.bytes += info.size();
        identity << QStringLiteral("%1:%2:%3").arg(file).arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
        FcCharSet *characters = nullptr;
        if (FcPatternGetCharSet(pattern, FC_CHARSET, 0, &characters) == FcResultMatch)
            covered << FcCharSetCopy(characters);
    };

    FcPattern *regular = matchStyle(requested.isEmpty() ? QStringLiteral("sans-serif") : requested,
                                    FC_WEIGHT_REGULAR, FC_SLANT_ROMAN);
    fonts.family = fcString(regular, FC_FAMILY);
    take(regular);
    if (regular) FcPatternDestroy(regular);
    // Markdown labels have bold and italic words.
    for (const auto &[weight, slant] : {std::pair{FC_WEIGHT_BOLD, FC_SLANT_ROMAN}, std::pair{FC_WEIGHT_REGULAR, FC_SLANT_ITALIC},
                                        std::pair{FC_WEIGHT_BOLD, FC_SLANT_ITALIC}}) {
        if (fonts.family.isEmpty()) break;
        FcPattern *styled = matchStyle(fonts.family, weight, slant);
        if (fcString(styled, FC_FAMILY) == fonts.family) take(styled);
        if (styled) FcPatternDestroy(styled);
    }
    // One font for each script the family lacks, those most often written
    // first, so that they are the ones that fit within the budget.
    struct Script {
        char32_t sample;
        const char *language;
        const char *family;
    };
    static const Script scripts[] = {
        {U'日', "ja", "sans-serif"},     {U'한', "ko", "sans-serif"},  {U'\U0001F600', nullptr, "emoji"},
        {U'א', "he", "sans-serif"},     {U'ا', "ar", "sans-serif"},  {U'क', "hi", "sans-serif"},
        {U'ก', "th", "sans-serif"},     {U'α', "el", "sans-serif"},  {U'ж', "ru", "sans-serif"},
    };
    for (const Script &script : scripts) {
        const bool has = std::any_of(covered.cbegin(), covered.cend(),
                                     [&](FcCharSet *characters) { return FcCharSetHasChar(characters, script.sample); });
        if (has) continue;
        if (FcPattern *cover = coverFor(script.sample, script.language, script.family)) {
            take(cover);
            FcPatternDestroy(cover);
        }
    }
    for (FcCharSet *characters : std::as_const(covered)) FcCharSetDestroy(characters);
    fonts.identity = identity.join(u'\n');
    return fonts;
}

QByteArray configuration(const RenderRequest &request, const QString &family)
{
    const RenderColors &c = request.colors;
    const QColor fg = c.foreground, bg = c.background, accent = c.accent, muted = c.muted, surface = c.surface;
    const QList<QColor> series = seriesFor(c);
    auto nth = [&series](int i) { return series.at(i % series.size()); };
    // Lines and borders read clearly without the weight of text.
    const QColor line = Theme::mix(fg, bg, .25);
    const QColor note = Theme::mix(bg, accent, .15);
    const QColor subtle = Theme::mix(bg, fg, .05);

    QJsonObject v;
    v[u"darkMode"] = c.dark;
    v[u"background"] = hex(bg);
    v[u"fontFamily"] = family.isEmpty() ? QStringLiteral("sans-serif") : family;
    v[u"fontSize"] = number(request.fontSize) + QStringLiteral("px");
    // Nodes, edges and text, which every family builds on.
    v[u"primaryColor"] = hex(surface);
    v[u"primaryTextColor"] = hex(fg);
    v[u"primaryBorderColor"] = hex(accent);
    v[u"secondaryColor"] = hex(Theme::mix(bg, accent, .18));
    v[u"secondaryTextColor"] = hex(fg);
    v[u"secondaryBorderColor"] = hex(muted);
    v[u"tertiaryColor"] = hex(subtle);
    v[u"tertiaryTextColor"] = hex(fg);
    v[u"tertiaryBorderColor"] = hex(muted);
    v[u"mainBkg"] = hex(surface);
    v[u"nodeBorder"] = hex(accent);
    v[u"nodeTextColor"] = hex(fg);
    v[u"lineColor"] = hex(line);
    v[u"textColor"] = hex(fg);
    v[u"titleColor"] = hex(fg);
    v[u"edgeLabelBackground"] = hex(bg);
    v[u"clusterBkg"] = hex(subtle);
    v[u"clusterBorder"] = hex(muted);
    v[u"noteBkgColor"] = hex(note);
    v[u"noteTextColor"] = hex(fg);
    v[u"noteBorderColor"] = hex(muted);
    v[u"errorBkgColor"] = hex(Theme::mix(bg, nth(3), .3));
    v[u"errorTextColor"] = hex(fg);
    // Sequence diagrams.
    v[u"actorBkg"] = hex(surface);
    v[u"actorBorder"] = hex(accent);
    v[u"actorTextColor"] = hex(fg);
    v[u"actorLineColor"] = hex(muted);
    v[u"signalColor"] = hex(line);
    v[u"signalTextColor"] = hex(fg);
    v[u"labelBoxBkgColor"] = hex(surface);
    v[u"labelBoxBorderColor"] = hex(muted);
    v[u"labelTextColor"] = hex(fg);
    v[u"loopTextColor"] = hex(fg);
    v[u"activationBkgColor"] = hex(Theme::mix(surface, accent, .2));
    v[u"activationBorderColor"] = hex(accent);
    v[u"sequenceNumberColor"] = hex(ink(accent, bg, fg));
    // State, class, entity relationship and requirement diagrams.
    v[u"labelColor"] = hex(fg);
    v[u"altBackground"] = hex(subtle);
    v[u"classText"] = hex(fg);
    v[u"attributeBackgroundColorOdd"] = hex(surface);
    v[u"attributeBackgroundColorEven"] = hex(Theme::mix(surface, bg, .5));
    v[u"relationColor"] = hex(line);
    v[u"relationLabelBackground"] = hex(bg);
    v[u"relationLabelColor"] = hex(fg);
    v[u"requirementBackground"] = hex(surface);
    v[u"requirementBorderColor"] = hex(accent);
    v[u"requirementTextColor"] = hex(fg);
    // Gantt charts.
    v[u"sectionBkgColor"] = hex(Theme::mix(bg, accent, .1));
    v[u"altSectionBkgColor"] = hex(bg);
    v[u"sectionBkgColor2"] = hex(Theme::mix(bg, nth(1), .1));
    v[u"taskBkgColor"] = hex(Theme::mix(surface, accent, .35));
    v[u"taskBorderColor"] = hex(accent);
    v[u"taskTextColor"] = hex(fg);
    v[u"taskTextLightColor"] = hex(fg);
    v[u"taskTextDarkColor"] = hex(fg);
    v[u"taskTextOutsideColor"] = hex(fg);
    v[u"taskTextClickableColor"] = hex(accent);
    v[u"activeTaskBkgColor"] = hex(Theme::mix(surface, nth(1), .35));
    v[u"activeTaskBorderColor"] = hex(nth(1));
    v[u"doneTaskBkgColor"] = hex(Theme::mix(bg, muted, .5));
    v[u"doneTaskBorderColor"] = hex(muted);
    v[u"critBkgColor"] = hex(Theme::mix(surface, nth(3), .4));
    v[u"critBorderColor"] = hex(nth(3));
    v[u"gridColor"] = hex(muted);
    v[u"todayLineColor"] = hex(nth(3));
    // Pie charts, Git graphs, mind maps, timelines, journeys and quadrants:
    // the families drawn in series colours.
    for (int i = 0; i < 12; ++i) {
        const QColor slice = i < series.size() ? nth(i) : Theme::mix(nth(i), bg, .35);
        v[QStringLiteral("pie%1").arg(i + 1)] = hex(slice);
        const QColor scale = Theme::mix(bg, slice, .45);
        v[QStringLiteral("cScale%1").arg(i)] = hex(scale);
        v[QStringLiteral("cScaleLabel%1").arg(i)] = hex(ink(scale, fg, bg));
        v[QStringLiteral("cScaleInv%1").arg(i)] = hex(slice);
    }
    v[u"pieTitleTextColor"] = hex(fg);
    v[u"pieSectionTextColor"] = hex(ink(nth(0), bg, fg));
    v[u"pieLegendTextColor"] = hex(fg);
    v[u"pieStrokeColor"] = hex(bg);
    v[u"pieOuterStrokeColor"] = hex(muted);
    for (int i = 0; i < 8; ++i) {
        v[QStringLiteral("git%1").arg(i)] = hex(nth(i));
        v[QStringLiteral("gitInv%1").arg(i)] = hex(ink(nth(i), bg, fg));
        v[QStringLiteral("gitBranchLabel%1").arg(i)] = hex(ink(nth(i), bg, fg));
        v[QStringLiteral("fillType%1").arg(i)] = hex(Theme::mix(bg, nth(i), .45));
    }
    v[u"commitLabelColor"] = hex(fg);
    v[u"commitLabelBackground"] = hex(surface);
    v[u"tagLabelColor"] = hex(fg);
    v[u"tagLabelBackground"] = hex(surface);
    v[u"tagLabelBorder"] = hex(muted);
    for (int i = 0; i < 4; ++i) {
        v[QStringLiteral("quadrant%1Fill").arg(i + 1)] = hex(Theme::mix(bg, nth(i), .2));
        v[QStringLiteral("quadrant%1TextFill").arg(i + 1)] = hex(fg);
    }
    v[u"quadrantPointFill"] = hex(accent);
    v[u"quadrantPointTextFill"] = hex(fg);
    v[u"quadrantXAxisTextFill"] = hex(fg);
    v[u"quadrantYAxisTextFill"] = hex(fg);
    v[u"quadrantTitleFill"] = hex(fg);
    v[u"quadrantInternalBorderStrokeFill"] = hex(muted);
    v[u"quadrantExternalBorderStrokeFill"] = hex(muted);
    // Architecture diagrams.
    v[u"archEdgeColor"] = hex(line);
    v[u"archEdgeArrowColor"] = hex(line);
    v[u"archGroupBorderColor"] = hex(muted);
    // Charts with an object of their own.
    QStringList palette;
    for (const QColor &color : series) palette << hex(color);
    auto object = [](std::initializer_list<std::pair<const char *, QString>> entries) {
        QJsonObject result;
        for (const auto &[name, value] : entries) result.insert(QLatin1String(name), value);
        return result;
    };
    v[u"xyChart"] = object({{"backgroundColor", hex(bg)},
                            {"titleColor", hex(fg)},
                            {"xAxisLabelColor", hex(fg)},
                            {"xAxisTitleColor", hex(fg)},
                            {"xAxisTickColor", hex(muted)},
                            {"xAxisLineColor", hex(muted)},
                            {"yAxisLabelColor", hex(fg)},
                            {"yAxisTitleColor", hex(fg)},
                            {"yAxisTickColor", hex(muted)},
                            {"yAxisLineColor", hex(muted)},
                            {"plotColorPalette", palette.join(QStringLiteral(", "))}});
    v[u"radar"] = object({{"axisColor", hex(muted)}, {"graticuleColor", hex(muted)}});
    v[u"packet"] = object({{"startByteColor", hex(fg)},
                           {"endByteColor", hex(fg)},
                           {"labelColor", hex(fg)},
                           {"titleColor", hex(fg)},
                           {"blockStrokeColor", hex(accent)},
                           {"blockFillColor", hex(surface)}});

    QJsonObject config;
    config[u"theme"] = QStringLiteral("base");
    config[u"themeVariables"] = v;
    config[u"fontFamily"] = v[u"fontFamily"];
    // C4 draws white words on its boxes whatever the theme, so the boxes
    // take the accent, darkened until white reads on it.
    const QColor box = readableUnderWhite(accent), external = readableUnderWhite(muted);
    QJsonObject c4;
    for (const char *element : {"person", "system", "system_db", "system_queue", "container", "container_db",
                                "container_queue", "component", "component_db", "component_queue"}) {
        const QString name = QLatin1String(element);
        c4[name + QStringLiteral("_bg_color")] = hex(box);
        c4[name + QStringLiteral("_border_color")] = hex(accent);
        c4[QStringLiteral("external_") + name + QStringLiteral("_bg_color")] = hex(external);
        c4[QStringLiteral("external_") + name + QStringLiteral("_border_color")] = hex(muted);
    }
    config[u"c4"] = c4;
    return QJsonDocument(config).toJson(QJsonDocument::Compact);
}

QList<HelperRun> plan(const RenderRequest &request, const QString &program, const Fonts &fonts)
{
    HelperRun base;
    base.program = program;
    base.input = request.source.toUtf8();
    base.files = {{QStringLiteral("mermaid.json"), configuration(request, fonts.family)}};
    // Merman's own budgets for interactive use, the theme's background, and
    // the chosen fonts only: no system font discovery, which can exceed
    // Merman's font limits and differs from machine to machine.
    QStringList common{QStringLiteral("--quiet"),
                       QStringLiteral("--config-file"),
                       QStringLiteral("mermaid.json"),
                       QStringLiteral("--background"),
                       hex(request.colors.background),
                       QStringLiteral("--resource-profile"),
                       QStringLiteral("interactive"),
                       QStringLiteral("--font-system"),
                       QStringLiteral("off")};
    for (const QString &file : fonts.files) common << QStringLiteral("--font-file") << file;

    HelperRun svg = base;
    svg.arguments = QStringList{QStringLiteral("render"), QStringLiteral("-"), QStringLiteral("--output"),
                                QStringLiteral("-"), QStringLiteral("--format"), QStringLiteral("svg"),
                                QStringLiteral("--svg-pipeline"), QStringLiteral("resvg-safe")} +
                    common;
    const qreal scale = request.purpose == RenderPurpose::Print ? std::max(request.scale, kPrintScale) : request.scale;
    HelperRun png = base;
    // Wide diagrams may be up to 8192 pixels across, within Merman's
    // default budget of 4096 × 4096 pixels in all; past that it draws them
    // smaller, which read() takes into account.
    png.arguments = QStringList{QStringLiteral("render"),           QStringLiteral("-"),
                                QStringLiteral("--output"),         QStringLiteral("-"),
                                QStringLiteral("--format"),         QStringLiteral("png"),
                                QStringLiteral("--scale"),          number(scale),
                                QStringLiteral("--raster-max-width"), QStringLiteral("8192"),
                                QStringLiteral("--raster-max-height"), QStringLiteral("8192")} +
                    common;
    if (needsRestyle(request.source)) {
        png.arguments.insert(2, QStringLiteral("--input-kind"));
        png.arguments.insert(3, QStringLiteral("svg"));
        // The SVG is drawn as it is: Mermaid's configuration has done its part.
        const qsizetype config = png.arguments.indexOf(QStringLiteral("--config-file"));
        png.arguments.remove(config, 2);
        png.files.clear();
        png.input.clear();
        const QString family = fonts.family;
        png.inputFromPrevious = [request, family](const QByteArray &drawn) {
            return restyle(drawn, request, family);
        };
    }
    return {svg, png};
}

QString declaredKind(const QString &source)
{
    const QStringList lines = source.split(u'\n');
    qsizetype i = 0;
    while (i < lines.size() && lines.at(i).trimmed().isEmpty()) ++i;
    // Front matter, as Mermaid allows before the diagram.
    if (i < lines.size() && lines.at(i).trimmed() == QLatin1String("---")) {
        ++i;
        while (i < lines.size() && lines.at(i).trimmed() != QLatin1String("---")) ++i;
        ++i;
    }
    for (; i < lines.size(); ++i) {
        const QString line = lines.at(i).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1String("%%"))) continue;
        static const QRegularExpression word(QStringLiteral(R"(^[A-Za-z0-9_-]+)"));
        return word.match(line).captured();
    }
    return {};
}

bool needsRestyle(const QString &source)
{
    const QString kind = declaredKind(source);
    return kind == QLatin1String("zenuml") || kind.startsWith(QLatin1String("C4"));
}

namespace {

// The SVG with every fill, stroke and colour of the map's colours, written
// on an element or in a style, replaced by the colour it maps to.
QString recolour(const QString &text, const QHash<QString, QString> &colours)
{
    static const QRegularExpression painted(
        QStringLiteral(R"re(\b(fill|stroke|color)(="|\s*:\s*)(#[0-9a-fA-F]{3,8}|black|white)\b)re"));
    QString recoloured;
    recoloured.reserve(text.size());
    qsizetype last = 0;
    for (auto it = painted.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        const QString colour = colours.value(match.captured(3).toLower());
        if (colour.isEmpty()) continue;
        recoloured += QStringView(text).mid(last, match.capturedStart(3) - last);
        recoloured += colour;
        last = match.capturedEnd(3);
    }
    recoloured += QStringView(text).mid(last);
    return recoloured;
}

} // namespace

QByteArray restyle(const QByteArray &svg, const RenderRequest &request, const QString &family)
{
    const RenderColors &c = request.colors;
    const QString fg = hex(c.foreground), bg = hex(c.background), accent = hex(c.accent), muted = hex(c.muted),
                  surface = hex(c.surface), line = hex(Theme::mix(c.foreground, c.background, .25)),
                  subtle = hex(Theme::mix(c.background, c.foreground, .05)),
                  note = hex(Theme::mix(c.background, c.accent, .15));
    QString text = QString::fromUtf8(svg);
    static const QRegularExpression role(QStringLiteral(R"re(<svg[^>]*\saria-roledescription="([^"]+)")re"));
    const QString kind = role.match(text).captured(1);
    if (kind == QLatin1String("c4")) {
        // Relations, their labels and their arrowheads; the boxes and their
        // white text come from the configuration.
        return recolour(text, {{QStringLiteral("#444444"), line},
                               {QStringLiteral("#000000"), line},
                               {QStringLiteral("black"), line}})
            .toUtf8();
    }
    if (kind != QLatin1String("zenuml")) return svg;
    // Colours written on the elements themselves, as ZenUML writes its
    // arrowheads and icons.
    text = recolour(text, {{QStringLiteral("#000000"), line}, {QStringLiteral("#000"), line},
                           {QStringLiteral("black"), line},   {QStringLiteral("#222222"), fg},
                           {QStringLiteral("#222"), fg},      {QStringLiteral("#333333"), fg},
                           {QStringLiteral("#333"), fg},      {QStringLiteral("#666666"), muted},
                           {QStringLiteral("#666"), muted},   {QStringLiteral("#ffffff"), bg},
                           {QStringLiteral("#fff"), bg},      {QStringLiteral("white"), bg},
                           {QStringLiteral("#dedede"), surface}});
    // Rules after ZenUML's own and as specific as theirs, so that they win.
    static const QRegularExpression root(QStringLiteral(R"re(<svg[^>]*\sid="([^"]+)")re"));
    const QString id = root.match(text).captured(1);
    const QString scope = id.isEmpty() ? QStringLiteral("svg ") : u'#' + id + u' ';
    const QString font =
        QStringLiteral("font-family:\"%1\"").arg(family.isEmpty() ? QStringLiteral("sans-serif") : family);
    const QList<std::pair<QString, QString>> rules{
        {QStringLiteral(".frame-border-outer"), QStringLiteral("fill:") + muted},
        {QStringLiteral(".frame-border-inner,.frame-header-bg"), QStringLiteral("fill:") + bg},
        {QStringLiteral(".frame-header-line"), QStringLiteral("stroke:") + muted},
        {QStringLiteral(".frame-title,.participant-label,.stereotype-label,.message-label,.fragment-label,"
                        ".fragment-condition,.fragment-section-label,.return-label,.divider-label,.comment-text,"
                        ".seq-number,.group-title-text"),
         QStringLiteral("fill:") + fg + u';' + font},
        {QStringLiteral(".participant-icon"), QStringLiteral("color:") + fg},
        {QStringLiteral(".participant-box,.occurrence"), QStringLiteral("fill:%1;stroke:%2").arg(surface, accent)},
        {QStringLiteral(".lifeline,.fragment-border,.fragment-separator,.divider-line,.group-outline"),
         QStringLiteral("stroke:") + muted},
        {QStringLiteral(".message-line,.return-line,.return-arrow"), QStringLiteral("stroke:") + line},
        {QStringLiteral(".arrow-head"), QStringLiteral("fill:%1;stroke:%1").arg(line)},
        {QStringLiteral(".fragment-header,.group-title-bg"), QStringLiteral("fill:") + subtle},
        {QStringLiteral(".divider-bg"), QStringLiteral("fill:%1;stroke:%2").arg(note, muted)},
        {QStringLiteral(".return-icon"), QStringLiteral("fill:") + fg},
    };
    QString css;
    for (const auto &[selectors, declarations] : rules) {
        QStringList scoped;
        for (const QString &selector : selectors.split(u',')) scoped << scope + selector;
        css += scoped.join(u',') + u'{' + declarations + u'}';
    }
    const qsizetype style = text.lastIndexOf(QLatin1String("</style>"));
    if (style >= 0) {
        text.insert(style, css);
    } else if (const qsizetype open = text.indexOf(u'>', text.indexOf(QLatin1String("<svg"))); open >= 0) {
        text.insert(open + 1, QStringLiteral("<style>") + css + QStringLiteral("</style>"));
    }
    return text.toUtf8();
}

namespace {

// The helper's own words, without the noise around them.
QString message(const HelperOutcome &outcome)
{
    QString text = QString::fromUtf8(outcome.errors).trimmed();
    if (text.size() > 2000) text = text.left(2000) + QStringLiteral("…");
    return text.isEmpty() ? QStringLiteral("merman-cli stopped with exit code %1").arg(outcome.exitCode) : text;
}

} // namespace

RenderResult read(const HelperOutcome &svg, const HelperOutcome &png, const RenderRequest &request)
{
    RenderResult result;
    result.elapsedMs = svg.elapsedMs + png.elapsedMs;
    result.networkIsolated = svg.networkIsolated && png.networkIsolated;
    if (!svg.succeeded()) {
        result.status = RenderResult::Refused;
        result.diagnostic = message(svg);
        return result;
    }
    result.description = describe(svg.output);
    const QSizeF size = MathRender::viewBoxSize(svg.output);
    if (!png.succeeded()) {
        result.status = RenderResult::Refused;
        result.diagnostic = message(png);
        return result;
    }
    QBuffer buffer;
    buffer.setData(png.output);
    QImageReader reader(&buffer, "png");
    reader.setAutoDetectImageFormat(false);
    reader.setDecideFormatFromContent(false);
    const QSize pixels = reader.size();
    if (!pixels.isValid() || pixels.isEmpty() || !size.isValid() || size.isEmpty()) {
        result.status = RenderResult::Failed;
        result.diagnostic = QStringLiteral("merman-cli drew an image parch cannot read");
        return result;
    }
    result.status = RenderResult::Rendered;
    result.format = QStringLiteral("png");
    result.data = png.output;
    result.size = size;
    result.scale = pixels.width() / size.width();
    result.vector = false;
    if (request.purpose == RenderPurpose::Print) {
        result.note = QStringLiteral("Printed as a %1 × %2 pixel image: Qt cannot draw Merman's SVG correctly.")
                          .arg(pixels.width())
                          .arg(pixels.height());
        // In the colours the picture has, as the PNG run drew it.
        result.svg = needsRestyle(request.source)
                         ? restyle(svg.output, request, chooseFonts(request.fontFamily).family)
                         : svg.output;
    }
    return result;
}

QString kindName(const QString &role)
{
    static const QHash<QString, QString> names{
        {QStringLiteral("flowchart-v2"), QStringLiteral("Flowchart")},
        {QStringLiteral("flowchart"), QStringLiteral("Flowchart")},
        {QStringLiteral("graph"), QStringLiteral("Flowchart")},
        {QStringLiteral("sequence"), QStringLiteral("Sequence diagram")},
        {QStringLiteral("classDiagram"), QStringLiteral("Class diagram")},
        {QStringLiteral("class"), QStringLiteral("Class diagram")},
        {QStringLiteral("stateDiagram"), QStringLiteral("State diagram")},
        {QStringLiteral("state"), QStringLiteral("State diagram")},
        {QStringLiteral("er"), QStringLiteral("Entity relationship diagram")},
        {QStringLiteral("journey"), QStringLiteral("User journey")},
        {QStringLiteral("gantt"), QStringLiteral("Gantt chart")},
        {QStringLiteral("pie"), QStringLiteral("Pie chart")},
        {QStringLiteral("quadrantChart"), QStringLiteral("Quadrant chart")},
        {QStringLiteral("requirement"), QStringLiteral("Requirement diagram")},
        {QStringLiteral("gitGraph"), QStringLiteral("Git graph")},
        {QStringLiteral("c4"), QStringLiteral("C4 diagram")},
        {QStringLiteral("mindmap"), QStringLiteral("Mind map")},
        {QStringLiteral("timeline"), QStringLiteral("Timeline")},
        {QStringLiteral("sankey"), QStringLiteral("Sankey diagram")},
        {QStringLiteral("xychart"), QStringLiteral("XY chart")},
        {QStringLiteral("block"), QStringLiteral("Block diagram")},
        {QStringLiteral("packet"), QStringLiteral("Packet diagram")},
        {QStringLiteral("kanban"), QStringLiteral("Kanban board")},
        {QStringLiteral("architecture"), QStringLiteral("Architecture diagram")},
        {QStringLiteral("radar"), QStringLiteral("Radar chart")},
        {QStringLiteral("treemap"), QStringLiteral("Treemap")},
        {QStringLiteral("zenuml"), QStringLiteral("Sequence diagram")},
    };
    if (const auto it = names.constFind(role); it != names.constEnd()) return *it;
    return QStringLiteral("Diagram");
}

QString describe(const QByteArray &svg)
{
    QXmlStreamReader xml(svg);
    QString role, title, summary;
    QStringList labels;
    int depth = 0;
    // Inside a text element: how deep, and what it says so far.
    int textDepth = 0;
    QString text;
    // Inside the root's own title or description.
    QString *heading = nullptr;
    int headingDepth = 0;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            ++depth;
            const auto name = xml.name();
            if (depth == 1 && name == u"svg") {
                role = xml.attributes().value(u"aria-roledescription").toString();
            } else if (depth == 2 && (name == u"title" || name == u"desc")) {
                heading = name == u"title" ? &title : &summary;
                headingDepth = depth;
            } else if (name == u"style" || name == u"script") {
                xml.skipCurrentElement();
                --depth;
            } else if (textDepth == 0 && name == u"text") {
                textDepth = depth;
                text.clear();
            } else if (textDepth > 0) {
                // Lines and runs of one label are separate words.
                text += u' ';
            }
        } else if (xml.isEndElement()) {
            if (textDepth > 0 && depth == textDepth) {
                const QString label = text.simplified();
                if (!label.isEmpty() && !labels.contains(label)) labels << label;
                textDepth = 0;
            } else if (textDepth > 0) {
                text += u' ';
            }
            if (heading && depth == headingDepth) heading = nullptr;
            --depth;
        } else if (xml.isCharacters()) {
            if (heading)
                *heading += xml.text();
            else if (textDepth > 0)
                text += xml.text();
        }
    }
    title = title.simplified();
    summary = summary.simplified();
    if (!title.isEmpty() || !summary.isEmpty()) {
        QStringList parts;
        for (const QString &part : {title, summary}) {
            if (part.isEmpty()) continue;
            const QChar last = part.back();
            parts << (last == u'.' || last == u'!' || last == u'?' ? part : part + u'.');
        }
        return parts.join(u' ');
    }
    const QString kind = kindName(role);
    if (labels.isEmpty()) return kind + u'.';
    QString words;
    int count = 0;
    for (const QString &label : std::as_const(labels)) {
        if (count == 40 || words.size() + label.size() > 600) {
            words += QStringLiteral(", …");
            break;
        }
        words += (count++ ? QStringLiteral(", ") : QString()) + label;
    }
    return kind + QStringLiteral(": ") + words + u'.';
}

} // namespace DiagramRender
