#include "mathrender.h"

#include <QHash>
#include <QRegularExpression>
#include <QSvgRenderer>

#include <algorithm>

namespace {

// The tool takes colours as #rrggbbaa.
QString rgba(const QColor &color)
{
    return QStringLiteral("#%1%2%3%4")
        .arg(color.red(), 2, 16, QLatin1Char('0'))
        .arg(color.green(), 2, 16, QLatin1Char('0'))
        .arg(color.blue(), 2, 16, QLatin1Char('0'))
        .arg(color.alpha(), 2, 16, QLatin1Char('0'));
}

QString number(qreal value) { return QString::number(value, 'g', 8); }

bool lineBreak(QChar c)
{
    return c == u'\n' || c == u'\r' || c == QChar(0x2028) || c == QChar(0x2029) || c == QChar(0x85);
}

// The opening tag of the SVG root.
QString rootTag(const QString &svg)
{
    const qsizetype start = svg.indexOf(QLatin1String("<svg"));
    if (start < 0) return {};
    const qsizetype end = svg.indexOf(u'>', start);
    return end < 0 ? QString() : svg.mid(start, end - start + 1);
}

// The tool sizes its pictures in points, which Qt's SVG renderer turns into
// more pixels than the formula's own units. Its units are already pixels at
// the requested font size, so the picture is sized in those.
QByteArray sizedInPixels(const QByteArray &svg, QSizeF size)
{
    QString text = QString::fromUtf8(svg);
    const QString tag = rootTag(text);
    if (tag.isEmpty()) return svg;
    QString sized = tag;
    static const QRegularExpression width(QStringLiteral(R"(\swidth="[^"]*")"));
    static const QRegularExpression height(QStringLiteral(R"(\sheight="[^"]*")"));
    sized.replace(width, QStringLiteral(" width=\"%1\"").arg(number(size.width())));
    sized.replace(height, QStringLiteral(" height=\"%1\"").arg(number(size.height())));
    text.replace(text.indexOf(tag), tag.size(), sized);
    return text.toUtf8();
}

} // namespace

namespace MathRender {

QString oneLine(const QString &tex)
{
    QString line;
    line.reserve(tex.size());
    for (qsizetype i = 0; i < tex.size(); ++i) {
        const QChar c = tex.at(i);
        if (c == u'\\') {
            line += c;
            if (i + 1 < tex.size()) {
                ++i;
                // A backslash at the end of a line is a control space.
                line += lineBreak(tex.at(i)) ? QChar(u' ') : tex.at(i);
                if (tex.at(i) == u'\r' && i + 1 < tex.size() && tex.at(i + 1) == u'\n') ++i;
            }
        } else if (c == u'%') {
            // A comment runs to the end of its line and takes the break with it.
            while (i + 1 < tex.size() && !lineBreak(tex.at(i + 1))) ++i;
            if (i + 1 < tex.size()) {
                ++i;
                if (tex.at(i) == u'\r' && i + 1 < tex.size() && tex.at(i + 1) == u'\n') ++i;
            }
        } else if (lineBreak(c)) {
            line += u' ';
        } else {
            line += c;
        }
    }
    return line.trimmed();
}

HelperRun plan(const RenderRequest &request, const QString &program)
{
    HelperRun run;
    run.program = program;
    run.arguments = {QStringLiteral("--stdout"),       QStringLiteral("--font-size"), number(request.fontSize),
                     QStringLiteral("--padding"),      QStringLiteral("0"),           QStringLiteral("--color"),
                     rgba(request.colors.foreground),
                     // rgb() and an opacity, which Qt's SVG renderer reads, where
                     // it takes the rgba() the tool writes otherwise for black.
                     QStringLiteral("--office-compatible-colors")};
    if (request.kind == RenderKind::InlineMath) run.arguments << QStringLiteral("--inline");
    QString formula = oneLine(request.source);
    // The tool skips a line that starts with # as a comment; an empty group
    // in front lets TeX say what it makes of the #.
    if (formula.startsWith(u'#')) formula.prepend(QStringLiteral("{}"));
    const QString rule = QStringLiteral(" \\rule[-%1em]{0pt}{%1em}").arg(kRuleDepthEm);
    run.input = (formula + u'\n' + formula + rule + u'\n').toUtf8();
    return run;
}

QSizeF viewBoxSize(const QByteArray &svg)
{
    static const QRegularExpression viewBox(QStringLiteral(
        R"(viewBox\s*=\s*"\s*[-+0-9.eE]+[\s,]+[-+0-9.eE]+[\s,]+([-+0-9.eE]+)[\s,]+([-+0-9.eE]+)\s*")"));
    const auto match = viewBox.match(rootTag(QString::fromUtf8(svg.left(4096))));
    if (!match.hasMatch()) return {};
    bool wide = false, tall = false;
    const QSizeF size(match.captured(1).toDouble(&wide), match.captured(2).toDouble(&tall));
    return wide && tall && size.width() >= 0 && size.height() >= 0 ? size : QSizeF();
}

RenderResult read(const HelperOutcome &outcome, const RenderRequest &request)
{
    RenderResult result;
    result.description = request.source.trimmed();
    result.elapsedMs = outcome.elapsedMs;
    result.networkIsolated = outcome.networkIsolated;
    const QString errors = QString::fromUtf8(outcome.errors);
    // One line per formula on the error output: "OK     1 <source>", or
    // "ERR    1 <source> — <what is wrong>".
    static const QRegularExpression report(QStringLiteral(R"(^(OK|ERR)\s+(\d+)\s(.*)$)"),
                                           QRegularExpression::MultilineOption);
    QHash<int, QString> refused;
    QList<int> drawn;
    for (auto it = report.globalMatch(errors); it.hasNext();) {
        const auto match = it.next();
        const int formula = match.captured(2).toInt();
        if (match.captured(1) == QLatin1String("OK")) {
            drawn << formula;
        } else {
            const QString rest = match.captured(3);
            const qsizetype dash = rest.lastIndexOf(QStringLiteral(" — "));
            refused.insert(formula, (dash >= 0 ? rest.mid(dash + 3) : rest).trimmed());
        }
    }
    if (refused.contains(1)) {
        result.status = RenderResult::Refused;
        result.diagnostic = refused.value(1);
        return result;
    }
    QList<QByteArray> pictures;
    for (const QByteArray &line : outcome.output.split('\n'))
        if (line.trimmed().startsWith("<svg")) pictures << line.trimmed();
    if (!drawn.contains(1) || pictures.isEmpty()) {
        result.status = RenderResult::Failed;
        result.diagnostic =
            errors.trimmed().isEmpty() ? QStringLiteral("render-svg drew nothing") : errors.trimmed();
        return result;
    }
    const QSizeF size = viewBoxSize(pictures.first());
    if (!size.isValid()) {
        result.status = RenderResult::Failed;
        result.diagnostic = QStringLiteral("render-svg drew a picture without a size");
        return result;
    }
    const QByteArray svg = sizedInPixels(pictures.first(), size);
    if (!QSvgRenderer(svg).isValid()) {
        result.status = RenderResult::Failed;
        result.diagnostic = QStringLiteral("render-svg drew a picture Qt cannot read");
        return result;
    }
    result.status = RenderResult::Rendered;
    result.format = QStringLiteral("svg");
    result.data = svg;
    result.size = size;
    result.vector = true;
    result.scale = 1;
    if (drawn.contains(2) && pictures.size() > 1) {
        const QSizeF reach = viewBoxSize(pictures.at(1));
        result.baseline = std::clamp(reach.height() - kRuleDepthEm * request.fontSize, 0.0, size.height());
    } else {
        result.baseline = size.height();
        result.note = QStringLiteral("The baseline could not be measured, so the formula sits on its lowest point.");
    }
    return result;
}

} // namespace MathRender
