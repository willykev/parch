#include "references.h"

#include "markdownlines.h"
#include "pathtext.h"

#include <QDir>
#include <QRegularExpression>
#include <QUrl>
#include <utility>

namespace {
using namespace MarkdownLines;

// How much of a line a change shows on either side of it.
constexpr int contextUnits = 60;

// The spans of inline code on a line, by runs of backticks, as [start, end).
QList<std::pair<qsizetype, qsizetype>> codeSpans(QStringView line)
{
    QList<std::pair<qsizetype, qsizetype>> spans;
    qsizetype at = 0;
    while (at < line.size()) {
        if (line.at(at) != u'`') {
            ++at;
            continue;
        }
        qsizetype run = at;
        while (run < line.size() && line.at(run) == u'`') ++run;
        const qsizetype length = run - at;
        // The closing run is exactly as long.
        qsizetype close = run;
        bool closed = false;
        while (close < line.size()) {
            if (line.at(close) != u'`') {
                ++close;
                continue;
            }
            qsizetype end = close;
            while (end < line.size() && line.at(end) == u'`') ++end;
            if (end - close == length) {
                spans.append({at, end});
                at = end;
                closed = true;
                break;
            }
            close = end;
        }
        if (!closed) at = run;
    }
    return spans;
}

bool inside(const QList<std::pair<qsizetype, qsizetype>> &spans, qsizetype at)
{
    return std::any_of(spans.begin(), spans.end(), [at](const auto &span) { return at >= span.first && at < span.second; });
}

// A destination's place on a line, after the ( that opens it, up to the )
// that closes the link; none when there is no such ).
struct Destination {
    qsizetype start = -1;
    qsizetype end = -1;
    bool enclosed = false;
};

Destination destinationAt(QStringView line, qsizetype open)
{
    qsizetype at = open;
    while (at < line.size() && (line.at(at) == u' ' || line.at(at) == u'\t')) ++at;
    Destination found;
    if (at < line.size() && line.at(at) == u'<') {
        qsizetype end = at + 1;
        while (end < line.size() && line.at(end) != u'>' && line.at(end) != u'<') end += line.at(end) == u'\\' ? 2 : 1;
        if (end >= line.size() || line.at(end) != u'>') return {};
        found = {at + 1, end, true};
        at = end + 1;
    } else {
        qsizetype end = at;
        int depth = 0;
        while (end < line.size()) {
            const QChar c = line.at(end);
            if (c == u'\\' && end + 1 < line.size()) {
                end += 2;
                continue;
            }
            if (c == u' ' || c == u'\t') break;
            if (c == u'(') ++depth;
            if (c == u')' && depth-- == 0) break;
            ++end;
        }
        found = {at, end, false};
        at = end;
    }
    // An optional title, then the closing parenthesis.
    while (at < line.size() && (line.at(at) == u' ' || line.at(at) == u'\t')) ++at;
    if (at < line.size() && (line.at(at) == u'"' || line.at(at) == u'\'' || line.at(at) == u'(')) {
        const QChar close = line.at(at) == u'(' ? u')' : line.at(at);
        qsizetype end = at + 1;
        while (end < line.size() && line.at(end) != close) end += line.at(end) == u'\\' ? 2 : 1;
        if (end >= line.size()) return {};
        at = end + 1;
        while (at < line.size() && (line.at(at) == u' ' || line.at(at) == u'\t')) ++at;
    }
    if (at >= line.size() || line.at(at) != u')') return {};
    return found;
}

const QRegularExpression &definition()
{
    static const QRegularExpression expression(QStringLiteral("^ {0,3}\\[(?!\\^)(?:[^\\]\\\\]|\\\\.)+\\]:[ \\t]*(?:<([^>]*)>|(\\S+))"));
    return expression;
}

// An include: a document's path, quoted or not, alone on its line, with an
// optional quoted caption after it.
const QRegularExpression &include()
{
    static const QRegularExpression expression(
        QStringLiteral("^ {0,3}(?:\"([^\"]+\\.(?:md|markdown|txt))\"|([^\\s\"\\[\\]()<>!#*>`|]+\\.(?:md|markdown|txt)))"
                       "(?:[ \\t]+\"[^\"]*\")?[ \\t]*$"),
        QRegularExpression::CaseInsensitiveOption);
    return expression;
}

const QRegularExpression &scheme()
{
    static const QRegularExpression expression(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    return expression;
}

// A destination's backslash escapes taken out, as Markdown reads them.
QString unescaped(const QString &text)
{
    QString plain;
    plain.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text.at(i) == u'\\' && i + 1 < text.size() && text.at(i + 1).unicode() < 128 && text.at(i + 1).isPunct()) ++i;
        plain.append(text.at(i));
    }
    return plain;
}

// A path percent-encoded where a URL needs it: spaces, % and the marks
// that would end or confuse a destination, and other characters beyond
// ASCII only when asked.
QString encoded(const QString &path, bool nonAscii)
{
    QString result;
    for (const QChar c : path) {
        const ushort u = c.unicode();
        const bool plain = (u > 0x20 && u < 0x7f && !QStringView(u"%()<>[]#?\"\\^`{|} ").contains(c))
                           || (u >= 0x80 && !nonAscii);
        if (plain) {
            result.append(c);
            continue;
        }
        for (const char byte : QString(c).toUtf8())
            result.append(u'%' + QString::number(uchar(byte), 16).toUpper().rightJustified(2, u'0'));
    }
    return result;
}

// A bare destination kept one destination: a space, or a < or >, would end
// it, and so would a parenthesis that is not matched.
QString bare(const QString &path)
{
    int depth = 0;
    bool balanced = true;
    for (const QChar c : path) {
        if (c == u'(') ++depth;
        else if (c == u')' && --depth < 0) balanced = false;
    }
    balanced = balanced && depth == 0;
    QString result;
    for (const QChar c : path) {
        if (c == u' ') result.append(QStringLiteral("%20"));
        else if (c == u'<') result.append(QStringLiteral("%3C"));
        else if (c == u'>') result.append(QStringLiteral("%3E"));
        else if (!balanced && c == u'(') result.append(QStringLiteral("%28"));
        else if (!balanced && c == u')') result.append(QStringLiteral("%29"));
        else result.append(c);
    }
    return result;
}

QString clean(const QString &path)
{
    return QDir::cleanPath(path);
}

QString shownAround(QStringView line, qsizetype start, qsizetype end, const QString &with)
{
    const qsizetype from = std::max<qsizetype>(0, start - contextUnits);
    const qsizetype until = std::min(line.size(), end + contextUnits);
    return (from > 0 ? QStringLiteral("…") : QString()) + line.sliced(from, start - from).toString() + with
           + line.sliced(end, until - end).toString() + (until < line.size() ? QStringLiteral("…") : QString());
}
} // namespace

QList<References::Reference> References::find(const QString &text)
{
    QList<Reference> found;
    const QList<Line> lines = split(text);
    const QList<bool> code = codeLines(text, lines);
    const int skipped = frontMatterLines(text, lines);
    const auto lineAt = [&](qsizetype i) {
        return QStringView(text).sliced(lines.at(i).start, lines.at(i).end - lines.at(i).start);
    };
    const auto add = [&](Kind kind, qsizetype line, qsizetype start, qsizetype end, bool enclosed) {
        if (end <= start) return;
        const int at = lines.at(line).start + int(start);
        found.append({kind, text.mid(at, end - start), {at, int(end - start)}, enclosed, int(line)});
    };
    for (qsizetype i = skipped; i < lines.size(); ++i) {
        if (code.at(i)) continue;
        const QStringView line = lineAt(i);
        if (blank(line)) continue;
        if (const QRegularExpressionMatch match = definition().matchView(line); match.hasMatch()) {
            const int group = match.capturedStart(1) >= 0 ? 1 : 2;
            add(Kind::Definition, i, match.capturedStart(group), match.capturedEnd(group), group == 1);
            continue;
        }
        // An include stands alone, as a paragraph of its own.
        const bool alone = (i == skipped || blank(lineAt(i - 1)) || code.at(i - 1))
                           && (i + 1 == lines.size() || blank(lineAt(i + 1)) || code.at(i + 1));
        if (alone) {
            if (const QRegularExpressionMatch match = include().matchView(line);
                match.hasMatch() && !match.captured(0).contains(QStringLiteral("://"))) {
                const int group = match.capturedStart(1) >= 0 ? 1 : 2;
                add(Kind::Include, i, match.capturedStart(group), match.capturedEnd(group), group == 1);
                continue;
            }
        }
        const auto spans = codeSpans(line);
        for (qsizetype at = line.indexOf(u"]("); at >= 0; at = line.indexOf(u"](", at + 1)) {
            if (inside(spans, at) || (at > 0 && line.at(at - 1) == u'\\')) continue;
            const Destination destination = destinationAt(line, at + 2);
            if (destination.start < 0) continue;
            // An image when its opening bracket follows a !.
            const qsizetype open = line.lastIndexOf(u'[', at);
            const Kind kind = open > 0 && line.at(open - 1) == u'!' ? Kind::Image : Kind::Link;
            add(kind, i, destination.start, destination.end, destination.enclosed);
        }
    }
    return found;
}

QString References::moved(const QString &path, const QList<Move> &moves)
{
    for (const Move &move : moves) {
        if (path == move.from) return move.to;
        if (path.startsWith(move.from + u'/')) return move.to + path.mid(move.from.size());
    }
    return path;
}

QList<References::Change> References::changes(const QString &text, const QString &path, const QString &library,
                                               const QList<Move> &moves)
{
    QList<Change> found;
    const QString newPath = moved(path, moves);
    const QString folder = PathText::folderOf(path);
    const QString newFolder = PathText::folderOf(newPath);
    const QList<Line> lines = split(text);
    for (const Reference &reference : find(text)) {
        QString written = reference.destination;
        QString suffix;
        const bool include = reference.kind == Kind::Include;
        if (!include) {
            const qsizetype mark = written.indexOf(QRegularExpression(QStringLiteral("[#?]")));
            if (mark >= 0) {
                suffix = written.mid(mark);
                written.truncate(mark);
            }
        }
        if (written.isEmpty() || written.startsWith(QStringLiteral("//")) || scheme().match(written).hasMatch()) continue;
        static const QRegularExpression escape(QStringLiteral("%[0-9A-Fa-f]{2}"));
        static const QRegularExpression escapeBeyondAscii(QStringLiteral("%[89A-Fa-f][0-9A-Fa-f]"));
        const bool percent = !include && escape.match(written).hasMatch();
        const QString plain = include ? written : QUrl::fromPercentEncoding(unescaped(written).toUtf8());
        const bool absolute = plain.startsWith(u'/');
        QString target;
        if (absolute && include) {
            if (library.isEmpty()) continue;
            target = clean(library + plain);
        } else {
            target = clean(absolute ? plain : folder + u'/' + plain);
        }
        const QString newTarget = moved(target, moves);
        if (newTarget == target && newPath == path) continue;
        QString relative;
        if (absolute && include && (newTarget == library || newTarget.startsWith(library + u'/'))) {
            relative = u'/' + QDir(library).relativeFilePath(newTarget);
        } else if (absolute && !include) {
            relative = newTarget;
        } else {
            relative = QDir(newFolder).relativeFilePath(newTarget);
            if (relative.isEmpty()) relative = QStringLiteral(".");
            if (plain.startsWith(QStringLiteral("./")) && !relative.startsWith(QStringLiteral(".."))) relative.prepend(QStringLiteral("./"));
        }
        if (plain.endsWith(u'/') && !relative.endsWith(u'/')) relative.append(u'/');
        if (relative == plain) continue;
        QString after;
        if (include) {
            // The range holds the path alone, inside its quotes when it has them.
            after = !reference.enclosed && relative.contains(u' ') ? u'"' + relative + u'"' : relative;
        } else if (reference.enclosed) {
            after = QString(relative).replace(u'<', QStringLiteral("\\<")).replace(u'>', QStringLiteral("\\>")) + suffix;
        } else if (percent) {
            after = encoded(relative, escapeBeyondAscii.match(written).hasMatch()) + suffix;
        } else {
            after = bare(relative) + suffix;
        }
        if (after == reference.destination) continue;
        const Line &line = lines.at(reference.line);
        const QStringView lineText = QStringView(text).sliced(line.start, line.end - line.start);
        const qsizetype start = reference.range.start - line.start;
        const qsizetype end = start + reference.range.length;
        found.append({reference.range, reference.destination, after, reference.line,
                      shownAround(lineText, start, end, reference.destination), shownAround(lineText, start, end, after)});
    }
    return found;
}
