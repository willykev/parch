#include "spellingexclusions.h"

#include <QRegularExpression>
#include <algorithm>
#include <vector>

namespace {

struct Line {
    int start, end; // the end leaves out the line break
};

bool isBreak(QChar c) { return c == u'\n' || c == u'\r' || c == QChar(0x2029); }

std::vector<Line> linesOf(QStringView text)
{
    std::vector<Line> lines;
    int start = 0;
    for (int i = 0; i < int(text.size()); ++i) {
        if (!isBreak(text[i])) continue;
        lines.push_back({start, i});
        if (text[i] == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') ++i;
        start = i + 1;
    }
    lines.push_back({start, int(text.size())});
    return lines;
}

bool blank(QStringView line)
{
    return std::all_of(line.begin(), line.end(), [](QChar c) { return c.isSpace(); });
}

// Columns of indentation, a tab reaching the next multiple of four; `units`
// is where the indentation ends.
int indentOf(QStringView line, int *units = nullptr)
{
    int columns = 0, i = 0;
    for (; i < line.size(); ++i) {
        if (line[i] == u' ') ++columns;
        else if (line[i] == u'\t') columns += 4 - columns % 4;
        else break;
    }
    if (units) *units = i;
    return columns;
}

int runOf(QStringView text, int at, int end, QChar c)
{
    int n = 0;
    while (at + n < end && text[at + n] == c) ++n;
    return n;
}

// A line that opens a fenced code block, with its fence's character and length.
bool opensFence(QStringView line, QChar *mark, int *length)
{
    int at = 0;
    if (indentOf(line, &at) > 3 || at >= line.size() || (line[at] != u'`' && line[at] != u'~')) return false;
    const int n = runOf(line, at, int(line.size()), line[at]);
    if (n < 3 || (line[at] == u'`' && line.sliced(at + n).contains(u'`'))) return false;
    *mark = line[at];
    *length = n;
    return true;
}

bool closesFence(QStringView line, QChar mark, int length)
{
    int at = 0;
    if (indentOf(line, &at) > 3) return false;
    const int n = runOf(line, at, int(line.size()), mark);
    return n >= length && blank(line.sliced(at + n));
}

bool startsListItem(QStringView line)
{
    static const QRegularExpression item(QStringLiteral("^\\s*(?:[*+-]|\\d{1,9}[.)])(?:\\s|$)"));
    return item.matchView(line).hasMatch();
}

int find(QStringView text, QStringView what, int from, int end)
{
    const qsizetype at = text.sliced(0, end).indexOf(what, from);
    return int(at);
}

// A footnote's label is one token; a note in brackets with words is prose.
bool isLabel(QStringView inside)
{
    return !inside.isEmpty() && std::all_of(inside.begin(), inside.end(), [](QChar c) {
        return c.isLetterOrNumber() || c == u'_' || c == u'-' || c == u':';
    });
}

// What is not prose in one line of prose: code spans, math, HTML comments,
// tags and autolinks, link destinations and labels, URLs and email addresses.
// A comment left open goes on to the next lines.
void scanLine(QStringView text, int from, int to, QList<SourceRange> &found, bool *inComment)
{
    const auto add = [&](int start, int end) {
        if (end > start) found.append({start, end - start});
    };
    // URLs with a scheme or www., and email addresses; a URL's closing
    // punctuation belongs to the sentence.
    static const QRegularExpression links(
        QStringLiteral("(?<![\\w/])(?:[A-Za-z][A-Za-z0-9+.-]*://|www\\.|mailto:)[^\\s<>\"'`]*"
                       "|[A-Za-z0-9._%+-]+@[A-Za-z0-9-]+(?:\\.[A-Za-z0-9-]+)+"));
    for (auto match = links.globalMatchView(text.sliced(from, to - from)); match.hasNext();) {
        const QRegularExpressionMatch link = match.next();
        int end = from + int(link.capturedEnd());
        const int start = from + int(link.capturedStart());
        while (end > start && QStringView(u".,:;!?*_~'\")").contains(text[end - 1])) {
            // A closing parenthesis stays when the URL opened one.
            if (text[end - 1] == u')'
                && text.sliced(start, end - start).count(u'(') >= text.sliced(start, end - start).count(u')'))
                break;
            --end;
        }
        add(start, end);
    }

    int i = from;
    while (i < to) {
        const QChar c = text[i];
        if (c == u'\\') {
            i += 2;
        } else if (c == u'`') {
            const int n = runOf(text, i, to, u'`');
            int close = -1;
            for (int j = i + n; j < to;) {
                if (text[j] != u'`') {
                    ++j;
                    continue;
                }
                const int m = runOf(text, j, to, u'`');
                if (m == n) {
                    close = j;
                    break;
                }
                j += m;
            }
            if (close >= 0) {
                add(i, close + n);
                i = close + n;
            } else {
                i += n;
            }
        } else if (c == u'<' && text.sliced(i, to - i).startsWith(u"<!--")) {
            const int end = find(text, u"-->", i + 4, to);
            if (end < 0) {
                add(i, to);
                *inComment = true;
                return;
            }
            add(i, end + 3);
            i = end + 3;
        } else if (c == u'<' && i + 1 < to
                   && (text[i + 1].isLetter() || (text[i + 1] == u'/' && i + 2 < to && text[i + 2].isLetter()))) {
            // A tag or an autolink, closed on this line.
            int close = -1;
            for (int j = i + 1; j < to && text[j] != u'<'; ++j)
                if (text[j] == u'>') {
                    close = j;
                    break;
                }
            if (close >= 0) {
                add(i, close + 1);
                i = close + 1;
            } else {
                ++i;
            }
        } else if (c == u'$' && i + 1 < to && text[i + 1] == u'$') {
            const int close = find(text, u"$$", i + 2, to);
            if (close >= 0) {
                add(i, close + 2);
                i = close + 2;
            } else {
                i += 2;
            }
        } else if (c == u'$') {
            // Inline math opens before something other than a space, after
            // anything but a letter or digit, and closes after something
            // other than a space, before anything but a letter or digit, so
            // that "$5 and $10" stays prose.
            int close = -1;
            if (i + 1 < to && !text[i + 1].isSpace() && (i == from || !text[i - 1].isLetterOrNumber())) {
                for (int j = i + 1; j < to; ++j) {
                    if (text[j] == u'\\') {
                        ++j;
                        continue;
                    }
                    if (text[j] == u'$' && !text[j - 1].isSpace() && (j + 1 >= to || !text[j + 1].isLetterOrNumber())) {
                        close = j;
                        break;
                    }
                }
            }
            if (close > i + 1) {
                add(i, close + 1);
                i = close + 1;
            } else {
                ++i;
            }
        } else if (c == u']' && i + 1 < to && (text[i + 1] == u'(' || text[i + 1] == u'[')) {
            // A link's destination and title, or a reference's label.
            int close = -1;
            if (text[i + 1] == u'(') {
                int depth = 0;
                for (int j = i + 1; j < to; ++j) {
                    if (text[j] == u'\\') {
                        ++j;
                    } else if (text[j] == u'(') {
                        ++depth;
                    } else if (text[j] == u')' && --depth == 0) {
                        close = j;
                        break;
                    }
                }
            } else {
                close = find(text, u"]", i + 2, to);
            }
            if (close >= 0) {
                add(i + 1, close + 1);
                i = close + 1;
            } else {
                ++i;
            }
        } else if (c == u'[' && i + 1 < to && QStringView(u"#%@^").contains(text[i + 1])) {
            // A citation, a variable, or a footnote's label.
            const int close = find(text, u"]", i + 2, to);
            if (close >= 0 && (text[i + 1] != u'^' || isLabel(text.sliced(i + 2, close - i - 2)))) {
                add(i, close + 1);
                i = close + 1;
            } else {
                ++i;
            }
        } else if (c == u'{' && text.sliced(i, to - i).startsWith(u"{{")) {
            const int close = find(text, u"}}", i + 2, to);
            if (close >= 0) {
                add(i, close + 2);
                i = close + 2;
            } else {
                i += 2;
            }
        } else {
            ++i;
        }
    }
}

QList<SourceRange> merged(QList<SourceRange> ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const SourceRange &a, const SourceRange &b) { return a.start < b.start; });
    QList<SourceRange> out;
    for (const SourceRange &range : std::as_const(ranges)) {
        if (!out.isEmpty() && range.start <= out.last().start + out.last().length) {
            SourceRange &last = out.last();
            last.length = qMax(last.start + last.length, range.start + range.length) - last.start;
        } else {
            out.append(range);
        }
    }
    return out;
}

} // namespace

QList<SourceRange> spellingExclusions(QStringView text)
{
    QList<SourceRange> found;
    const auto add = [&](int start, int end) {
        if (end > start) found.append({start, end - start});
    };
    const std::vector<Line> lines = linesOf(text);
    const auto lineText = [&](size_t index) {
        return text.sliced(lines[index].start, lines[index].end - lines[index].start);
    };
    size_t i = 0;
    // Front matter: from a line of three hyphens at the very start, after a
    // byte order mark if there is one, to the next line of three hyphens or dots.
    {
        QStringView first = lineText(0);
        if (first.startsWith(QChar(0xfeff))) first = first.sliced(1);
        if (first.trimmed() == u"---" && first.startsWith(u"---")) {
            for (size_t k = 1; k < lines.size(); ++k) {
                const QStringView closing = lineText(k).trimmed();
                if (closing == u"---" || closing == u"...") {
                    add(0, lines[k].end);
                    i = k + 1;
                    break;
                }
            }
        }
    }
    static const QRegularExpression definition(QStringLiteral("^ {0,3}\\[([^\\]]+)\\]:"));
    bool afterBlank = true;
    bool inList = false;
    bool inComment = false;
    for (; i < lines.size(); ++i) {
        const Line line = lines[i];
        const QStringView view = lineText(i);
        if (inComment) {
            const qsizetype end = view.indexOf(u"-->");
            if (end < 0) {
                add(line.start, line.end);
                continue;
            }
            add(line.start, line.start + int(end) + 3);
            inComment = false;
            scanLine(text, line.start + int(end) + 3, line.end, found, &inComment);
            afterBlank = false;
            continue;
        }
        if (blank(view)) {
            afterBlank = true;
            continue;
        }
        QChar mark;
        int length = 0;
        if (opensFence(view, &mark, &length)) {
            size_t k = i + 1;
            while (k < lines.size() && !closesFence(lineText(k), mark, length)) ++k;
            add(line.start, k < lines.size() ? lines[k].end : int(text.size()));
            i = k;
            afterBlank = false;
            continue;
        }
        const int indent = indentOf(view);
        // A list's items and what continues them; a paragraph at the margin
        // after a blank line ends it.
        if (startsListItem(view)) inList = true;
        else if (indent < 2 && afterBlank) inList = false;
        if (indent >= 4 && afterBlank && !inList) {
            // Indented code, to its last indented line.
            size_t last = i;
            for (size_t k = i + 1; k < lines.size(); ++k) {
                const QStringView next = lineText(k);
                if (blank(next)) continue;
                if (indentOf(next) < 4) break;
                last = k;
            }
            add(line.start, lines[last].end);
            i = last;
            afterBlank = false;
            continue;
        }
        afterBlank = false;
        const QStringView trimmed = view.trimmed();
        if (trimmed.startsWith(u"$$") && !trimmed.sliced(2).contains(u"$$")) {
            size_t k = i + 1;
            while (k < lines.size() && !lineText(k).contains(u"$$")) ++k;
            if (k < lines.size()) {
                add(line.start, lines[k].end);
                i = k;
                continue;
            }
        }
        // A Writer comment line.
        if (view.startsWith(u"//")) {
            add(line.start, line.end);
            continue;
        }
        int from = line.start;
        if (const QRegularExpressionMatch match = definition.matchView(view); match.hasMatch()) {
            // A link's reference definition is all label and destination; a
            // footnote's or a citation's has prose after its label.
            const QStringView label = match.capturedView(1);
            if (!label.startsWith(u'^') && !label.startsWith(u'#')) {
                add(line.start, line.end);
                continue;
            }
            from = line.start + int(match.capturedEnd());
            add(line.start, from);
        }
        scanLine(text, from, line.end, found, &inComment);
    }
    return merged(found);
}
