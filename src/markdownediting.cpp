#include "markdownediting.h"

#include "mdtable.h"

#include <QHash>
#include <QRegularExpression>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <functional>
#include <map>

namespace MarkdownEditing {

namespace {

// ---- Lines ---------------------------------------------------------------

bool isBreak(QChar c) { return c == u'\n' || c == u'\r'; }

// One line of the text: [start, end) is its text, and next is where the
// line after it starts, past its line break; at the end of the text, end.
struct Line {
    int start = 0;
    int end = 0;
    int next = 0;
    int length() const { return end - start; }
    bool hasBreak() const { return next > end; }
};

Line lineAt(QStringView text, int position)
{
    const int size = int(text.size());
    position = qBound(0, position, size);
    // Between the CR and the LF of a pair is the end of the line before.
    if (position > 0 && position < size && text[position - 1] == u'\r' && text[position] == u'\n') --position;
    int start = position;
    while (start > 0 && !isBreak(text[start - 1])) --start;
    int end = position;
    while (end < size && !isBreak(text[end])) ++end;
    int next = end;
    if (next < size) next += (text[next] == u'\r' && next + 1 < size && text[next + 1] == u'\n') ? 2 : 1;
    return {start, end, next};
}

// The line before, or nothing for the first.
std::optional<Line> lineBefore(QStringView text, const Line &line)
{
    if (line.start == 0) return std::nullopt;
    return lineAt(text, line.start - 1);
}

// The line after, or nothing for the last.
std::optional<Line> lineAfter(QStringView text, const Line &line)
{
    if (!line.hasBreak()) return std::nullopt;
    return lineAt(text, line.next);
}

// The lines a selection touches. A selection that ends where a line starts
// does not touch that line.
QList<Line> linesOf(QStringView text, int start, int end)
{
    QList<Line> lines{lineAt(text, start)};
    while (lines.last().hasBreak() && lines.last().next < end) lines.append(lineAt(text, lines.last().next));
    return lines;
}

QStringView textOf(QStringView text, const Line &line) { return text.mid(line.start, line.length()); }

bool isBlank(QStringView text)
{
    return std::all_of(text.begin(), text.end(), [](QChar c) { return c == u' ' || c == u'\t'; });
}

int leadingSpaces(QStringView text, int from = 0)
{
    int i = from;
    while (i < text.size() && (text[i] == u' ' || text[i] == u'\t')) ++i;
    return i - from;
}

// ---- Positions through edits ---------------------------------------------

// Where a position of the text before the edits is in the text after them.
// With `after`, a position where text is inserted ends up after it.
int mapPosition(int position, const QList<SourceEdit> &edits, bool after = true)
{
    int shift = 0;
    for (const SourceEdit &edit : edits) {
        const int start = edit.range.start, end = start + edit.range.length;
        const int size = int(edit.replacement.size());
        if (end < position || (end == position && (edit.range.length > 0 || after))) {
            shift += size - edit.range.length;
            continue;
        }
        if (start < position) return start + shift + qMin(position - start, size);
        break;
    }
    return position + shift;
}

void sortEdits(QList<SourceEdit> &edits)
{
    std::stable_sort(edits.begin(), edits.end(),
                     [](const SourceEdit &a, const SourceEdit &b) { return a.range.start < b.range.start; });
}

// The change, with the selection mapped through its edits: a caret moves
// past what is inserted where it is; a selection keeps what is inserted at
// its start outside it and at its end inside it.
// Positions `staysBefore` names stay before what is inserted there.
TextChange mapped(QList<SourceEdit> edits, int anchor, int cursor, const std::function<bool(int)> &staysBefore = {})
{
    sortEdits(edits);
    const auto map = [&](int position, bool after) {
        return mapPosition(position, edits, after && !(staysBefore && staysBefore(position)));
    };
    TextChange change;
    if (anchor == cursor) {
        change.anchor = change.cursor = map(cursor, true);
    } else {
        const bool forward = anchor < cursor;
        const int start = map(qMin(anchor, cursor), false);
        const int end = map(qMax(anchor, cursor), true);
        change.anchor = forward ? start : end;
        change.cursor = forward ? end : start;
    }
    change.edits = std::move(edits);
    return change;
}

// The same for edits that wrap a selection: what is inserted at its edges
// stays outside it.
TextChange mappedInside(QList<SourceEdit> edits, int anchor, int cursor)
{
    sortEdits(edits);
    const bool forward = anchor <= cursor;
    const int start = mapPosition(qMin(anchor, cursor), edits, true);
    const int end = qMax(start, mapPosition(qMax(anchor, cursor), edits, false));
    return {std::move(edits), forward ? start : end, forward ? end : start};
}

TextChange selecting(QList<SourceEdit> edits, int start, int end, bool forward = true)
{
    sortEdits(edits);
    return {std::move(edits), forward ? start : end, forward ? end : start};
}

// ---- Block structure of a line -------------------------------------------

// The parts of a line's start that give it its block structure, as offsets
// into the line: block quote markers, indentation, a list marker with the
// spaces after it, and a task box. Headings are read separately.
struct Parts {
    int quoteEnd = 0;
    int indentEnd = 0;
    // -1 for a line that is no list item.
    int markerStart = -1;
    int markerEnd = -1;
    QChar bullet;
    qint64 number = 0;
    QChar delimiter;
    int spaceEnd = 0;
    bool task = false;
    bool checked = false;
    int boxStart = -1;
    int contentStart = 0;

    bool item() const { return markerStart >= 0; }
    bool ordered() const { return item() && bullet.isNull(); }
    // The indentation, counted after the quote markers.
    int indent() const { return indentEnd - quoteEnd; }
};

// Three or more of one of * - _, with nothing but spaces between.
bool isThematicBreak(QStringView text)
{
    QChar mark;
    int count = 0;
    for (const QChar c : text) {
        if (c == u' ' || c == u'\t') continue;
        if (c != u'*' && c != u'-' && c != u'_') return false;
        if (!mark.isNull() && c != mark) return false;
        mark = c;
        ++count;
    }
    return count >= 3;
}

Parts partsOf(QStringView line)
{
    Parts parts;
    const int size = int(line.size());
    // Quote markers, each after at most three spaces and with one space after it.
    for (;;) {
        int i = parts.quoteEnd;
        int spaces = 0;
        while (i < size && line[i] == u' ' && spaces < 3) {
            ++i;
            ++spaces;
        }
        if (i >= size || line[i] != u'>') break;
        ++i;
        if (i < size && line[i] == u' ') ++i;
        parts.quoteEnd = i;
    }
    parts.indentEnd = parts.quoteEnd + leadingSpaces(line, parts.quoteEnd);
    parts.contentStart = parts.indentEnd;
    parts.spaceEnd = parts.indentEnd;
    if (isThematicBreak(line.mid(parts.quoteEnd))) return parts;
    const int at = parts.indentEnd;
    const auto spaceOrEnd = [&](int i) { return i >= size || line[i] == u' ' || line[i] == u'\t'; };
    int markerEnd = -1;
    if (at < size && (line[at] == u'-' || line[at] == u'*' || line[at] == u'+') && spaceOrEnd(at + 1)) {
        parts.bullet = line[at];
        markerEnd = at + 1;
    } else {
        int digits = 0;
        while (at + digits < size && line[at + digits].isDigit() && line[at + digits].unicode() < 128 && digits < 10)
            ++digits;
        if (digits >= 1 && digits <= 9 && at + digits < size && (line[at + digits] == u'.' || line[at + digits] == u')')
            && spaceOrEnd(at + digits + 1)) {
            parts.number = line.mid(at, digits).toLongLong();
            parts.delimiter = line[at + digits];
            markerEnd = at + digits + 1;
        }
    }
    if (markerEnd < 0) return parts;
    parts.markerStart = at;
    parts.markerEnd = markerEnd;
    // Up to four spaces separate the marker from the text; an item with
    // nothing after it takes all of them.
    const int spaces = leadingSpaces(line, markerEnd);
    parts.spaceEnd = markerEnd + (markerEnd + spaces >= size ? spaces : qMin(spaces, 4));
    parts.contentStart = parts.spaceEnd;
    const int box = parts.spaceEnd;
    if (box + 2 < size && line[box] == u'[' && line[box + 2] == u']'
        && (line[box + 1] == u' ' || line[box + 1] == u'x' || line[box + 1] == u'X') && spaceOrEnd(box + 3)) {
        parts.task = true;
        parts.checked = line[box + 1] != u' ';
        parts.boxStart = box;
        parts.contentStart = box + 3 + (box + 3 < size ? 1 : 0);
    }
    return parts;
}

// An ATX heading's level, and where its text starts, after the quote
// markers; level 0 for a line that is no ATX heading.
struct Atx {
    int level = 0;
    int markStart = 0;
    int textStart = 0;
};

Atx atxOf(QStringView line, int from)
{
    Atx atx;
    int i = from;
    int spaces = 0;
    while (i < line.size() && line[i] == u' ' && spaces < 3) {
        ++i;
        ++spaces;
    }
    int hashes = 0;
    while (i + hashes < line.size() && line[i + hashes] == u'#') ++hashes;
    if (hashes < 1 || hashes > 6) return atx;
    const int after = i + hashes;
    if (after < line.size() && line[after] != u' ' && line[after] != u'\t') return atx;
    atx.level = hashes;
    atx.markStart = i;
    atx.textStart = after + leadingSpaces(line, after);
    return atx;
}

// A Setext underline's level: 1 for =, 2 for -, 0 for a line that is none.
int underlineLevel(QStringView line)
{
    const int spaces = leadingSpaces(line);
    if (spaces > 3) return 0;
    const QStringView rest = line.mid(spaces);
    if (rest.isEmpty() || (rest[0] != u'=' && rest[0] != u'-')) return 0;
    const QChar mark = rest[0];
    int i = 0;
    while (i < rest.size() && rest[i] == mark) ++i;
    if (!isBlank(rest.mid(i))) return 0;
    return mark == u'=' ? 1 : 2;
}

// A fence of three or more backticks or tildes, as it opens or closes a code block.
struct Fence {
    QChar mark;
    int length = 0;
    bool hasInfo = false;
};

std::optional<Fence> fenceOf(QStringView line)
{
    const int spaces = leadingSpaces(line);
    if (spaces > 3 || spaces >= line.size()) return std::nullopt;
    const QChar mark = line[spaces];
    if (mark != u'`' && mark != u'~') return std::nullopt;
    int length = 0;
    while (spaces + length < line.size() && line[spaces + length] == mark) ++length;
    if (length < 3) return std::nullopt;
    const QStringView info = line.mid(spaces + length);
    if (mark == u'`' && info.contains(u'`')) return std::nullopt;
    return Fence{mark, length, !isBlank(info)};
}

bool isDisplayMathFence(QStringView line) { return line.trimmed() == u"$$"; }

// The fence lines of the text that open a code block, each with the line
// that closes it, or with -1 when nothing does.
std::map<int, int> fencedBlocks(QStringView text)
{
    std::map<int, int> blocks;
    int open = -1;
    Fence opening;
    for (Line line = lineAt(text, 0);; line = lineAt(text, line.next)) {
        const auto fence = fenceOf(textOf(text, line));
        if (fence) {
            if (open < 0) {
                open = line.start;
                opening = *fence;
                blocks[open] = -1;
            } else if (fence->mark == opening.mark && fence->length >= opening.length && !fence->hasInfo) {
                blocks[open] = line.start;
                open = -1;
            }
        }
        if (!line.hasBreak()) break;
    }
    return blocks;
}

// ---- Inline delimiters ---------------------------------------------------

int runBefore(QStringView text, int position, QChar c)
{
    int i = position;
    while (i > 0 && text[i - 1] == c) --i;
    return position - i;
}

int runAfter(QStringView text, int position, QChar c)
{
    int i = position;
    while (i < text.size() && text[i] == c) ++i;
    return i - position;
}

// An inline style written with a repeated character: the marker it writes,
// the characters that can make it, and how long a run of them must be to be it.
struct Style {
    QString marker;
    QString characters;
    enum Rule { AtLeastTwo, Odd, One } rule;

    bool fits(int run) const
    {
        switch (rule) {
        case AtLeastTwo:
            return run >= 2;
        case Odd:
            return run % 2 == 1;
        case One:
            return run == 1;
        }
        return false;
    }
    int length() const { return int(marker.size()); }
};

std::optional<Style> styleOf(const QString &command)
{
    if (command == u"bold") return Style{QStringLiteral("**"), QStringLiteral("*_"), Style::AtLeastTwo};
    if (command == u"italic") return Style{QStringLiteral("*"), QStringLiteral("*_"), Style::Odd};
    if (command == u"strikethrough") return Style{QStringLiteral("~~"), QStringLiteral("~"), Style::AtLeastTwo};
    if (command == u"highlight") return Style{QStringLiteral("=="), QStringLiteral("="), Style::AtLeastTwo};
    if (command == u"inlineMath") return Style{QStringLiteral("$"), QStringLiteral("$"), Style::One};
    // The delimited forms, ^(a+b)^ and ~y,z~, which hold punctuation too;
    // two tildes are a strikethrough, never a subscript.
    if (command == u"superscript") return Style{QStringLiteral("^"), QStringLiteral("^"), Style::One};
    if (command == u"subscript") return Style{QStringLiteral("~"), QStringLiteral("~"), Style::One};
    return std::nullopt;
}

// Whether a position on a line is inside a code span: between a run of
// backticks and the next run as long.
bool inCodeSpan(QStringView text, int position)
{
    const Line line = lineAt(text, position);
    for (int i = line.start; i < line.end;) {
        if (text[i] == u'\\') {
            i += 2;
            continue;
        }
        if (text[i] != u'`') {
            ++i;
            continue;
        }
        const int open = runAfter(text, i, u'`');
        int close = i + open;
        while (close < line.end) {
            if (text[close] != u'`') {
                ++close;
                continue;
            }
            const int run = runAfter(text, close, u'`');
            if (run == open) break;
            close += run;
        }
        if (close >= line.end) {
            i += open;
            continue;
        }
        if (position >= i + open && position <= close) return true;
        i = close + open;
    }
    return false;
}

// Whether the dialect reads [start, end) as a superscript or subscript once
// it has the marker either side, as the inline parser reads one: after a
// letter, a digit, a mark or what closes a group; holding no white space,
// code, brackets or marker of its own, and not ending in a backslash, which
// would escape the closing marker; with no marker beside it, which would
// make a tilde's a strikethrough or leave a caret after it with nothing to
// raise; not right after a run that is raised or lowered already; and not
// inside code. An empty span is where a pair is inserted.
bool scriptFits(QStringView text, int start, int end, QChar marker)
{
    if (start <= 0) return false;
    char32_t base = text[start - 1].unicode();
    if (QChar::isLowSurrogate(base) && start >= 2 && text[start - 2].isHighSurrogate())
        base = QChar::surrogateToUcs4(text[start - 2], text[start - 1]);
    const QChar::Category category = QChar::category(base);
    if (!QChar::isLetterOrNumber(base) && !QChar::isMark(base) && category != QChar::Punctuation_Close
        && category != QChar::Punctuation_FinalQuote)
        return false;
    for (int i = start; i < end; ++i) {
        const QChar c = text[i];
        if (c == u'\\') {
            if (i + 1 >= end) return false;
            ++i;
            continue;
        }
        if (c.isSpace() || c == u'`' || c == u'[' || c == u']' || c == marker) return false;
    }
    if (end < text.size() && text[end] == marker) return false;
    // A base that is itself raised or lowered, as the 2 of x^2, would have
    // the marker written after it close that one instead.
    int back = start;
    while (back > 0 && (text[back - 1].isLetterOrNumber() || text[back - 1].isMark() || text[back - 1].isSurrogate()))
        --back;
    if (back > 0 && text[back - 1] == marker && !(back > 1 && text[back - 2] == u'\\')) return false;
    return !inCodeSpan(text, start);
}

// How one inline command wraps and unwraps a span of a line.
struct InlineKind {
    // The edits that take the markers around or at the edges of [start, end)
    // away, when it has them.
    std::function<std::optional<QList<SourceEdit>>(QStringView, int, int)> unwrap;
    std::function<QList<SourceEdit>(QStringView, int, int)> wrap;
    // Without a selection: the edits that take away the markers of the span
    // the caret is in, with where the caret goes; or the text inserted, with
    // the caret's place in it.
    std::function<std::optional<TextChange>(QStringView, int)> remove;
    std::function<std::pair<QString, int>()> insert;
    // Wraps a selection over several lines as one span rather than line by line.
    bool spansLines = false;
    // Whether markers wrapped around [start, end), or inserted at a caret
    // when they are the same, make what the command is for; every span
    // when not given.
    std::function<bool(QStringView, int, int)> fits;
};

// Runs of one character on a line, outside code spans, as [start, end).
QList<std::pair<int, int>> runsOn(QStringView text, const Line &line, QChar c)
{
    QList<std::pair<int, int>> runs;
    for (int i = line.start; i < line.end;) {
        if (text[i] == u'\\') {
            i += 2;
            continue;
        }
        if (text[i] != c) {
            ++i;
            continue;
        }
        const int start = i;
        while (i < line.end && text[i] == c) ++i;
        runs.append({start, i});
    }
    return runs;
}

InlineKind styleKind(const Style &style)
{
    const int m = style.length();
    InlineKind kind;
    kind.unwrap = [style, m](QStringView text, int start, int end) -> std::optional<QList<SourceEdit>> {
        for (const QChar c : style.characters) {
            // Markers just outside.
            const int before = runBefore(text, start, c), after = runAfter(text, end, c);
            if (before > 0 && after > 0 && style.fits(before) && style.fits(after))
                return QList<SourceEdit>{{{start - m, m}, {}}, {{end, m}, {}}};
            // Markers at the edges of the selection itself.
            const int first = qMin(runAfter(text, start, c), end - start);
            const int last = qMin(runBefore(text, end, c), end - start);
            if (first > 0 && last > 0 && first + last < end - start && style.fits(first) && style.fits(last))
                return QList<SourceEdit>{{{start, m}, {}}, {{end - m, m}, {}}};
        }
        return std::nullopt;
    };
    kind.wrap = [style](QStringView, int start, int end) {
        return QList<SourceEdit>{{{start, 0}, style.marker}, {{end, 0}, style.marker}};
    };
    kind.remove = [style, m](QStringView text, int caret) -> std::optional<TextChange> {
        for (const QChar c : style.characters) {
            // An empty pair with the caret in it.
            const int before = runBefore(text, caret, c), after = runAfter(text, caret, c);
            if (before > 0 && after > 0 && style.fits(before) && style.fits(after))
                return TextChange{{{{caret - m, 2 * m}, {}}}, caret - m, caret - m};
            // The span the caret is in: the nearest runs on either side that
            // can make the style, the one before opening a span.
            const Line line = lineAt(text, caret);
            QList<std::pair<int, int>> runs;
            for (const auto &run : runsOn(text, line, c))
                if (style.fits(run.second - run.first)) runs.append(run);
            int opener = -1;
            for (int i = 0; i < runs.size(); ++i)
                if (runs[i].second <= caret) opener = i;
            if (opener < 0 || opener % 2 != 0 || opener + 1 >= runs.size()) continue;
            const auto open = runs[opener], close = runs[opener + 1];
            if (close.first < caret) continue;
            if (open.second < line.end && text[open.second].isSpace()) continue;
            if (close.first > line.start && text[close.first - 1].isSpace()) continue;
            const int at = caret - m;
            return TextChange{{{{open.second - m, m}, {}}, {{close.first, m}, {}}}, at, at};
        }
        return std::nullopt;
    };
    kind.insert = [style, m] { return std::pair{style.marker + style.marker, m}; };
    return kind;
}

InlineKind codeKind()
{
    InlineKind kind;
    kind.unwrap = [](QStringView text, int start, int end) -> std::optional<QList<SourceEdit>> {
        const int before = runBefore(text, start, u'`'), after = runAfter(text, end, u'`');
        if (before > 0 && before == after) return QList<SourceEdit>{{{start - before, before}, {}}, {{end, after}, {}}};
        const int first = runAfter(text, start, u'`'), last = runBefore(text, end, u'`');
        if (first > 0 && first == last && first + last < end - start) {
            // A space either side of the content belongs to the delimiters.
            const bool padded = text[start + first] == u' ' && text[end - last - 1] == u' ' && end - start - 2 * first > 2;
            const int pad = padded ? 1 : 0;
            return QList<SourceEdit>{{{start, first + pad}, {}}, {{end - last - pad, last + pad}, {}}};
        }
        return std::nullopt;
    };
    kind.wrap = [](QStringView text, int start, int end) {
        int longest = 0;
        for (int i = start; i < end;) {
            const int run = runAfter(text, i, u'`');
            longest = qMax(longest, run);
            i += qMax(1, run);
        }
        const QString fence(longest + 1, u'`');
        const bool pad = text[start] == u'`' || text[end - 1] == u'`';
        return QList<SourceEdit>{{{start, 0}, pad ? fence + u' ' : fence}, {{end, 0}, pad ? u' ' + fence : fence}};
    };
    kind.remove = [](QStringView text, int caret) -> std::optional<TextChange> {
        const int before = runBefore(text, caret, u'`'), after = runAfter(text, caret, u'`');
        if (before > 0 && before == after)
            return TextChange{{{{caret - before, before + after}, {}}}, caret - before, caret - before};
        // Code spans on the line: each run of backticks closed by the next run as long.
        const Line line = lineAt(text, caret);
        const auto runs = runsOn(text, line, u'`');
        for (int i = 0; i < runs.size(); ++i) {
            const int length = runs[i].second - runs[i].first;
            int j = i + 1;
            while (j < runs.size() && runs[j].second - runs[j].first != length) ++j;
            if (j >= runs.size()) continue;
            if (runs[i].second <= caret && runs[j].first >= caret) {
                const int at = caret - length;
                return TextChange{{{{runs[i].first, length}, {}}, {{runs[j].first, length}, {}}}, at, at};
            }
            i = j;
        }
        return std::nullopt;
    };
    kind.insert = [] { return std::pair{QStringLiteral("``"), 1}; };
    return kind;
}

InlineKind commentKind()
{
    static const QString open = QStringLiteral("<!--"), close = QStringLiteral("-->");
    InlineKind kind;
    kind.unwrap = [](QStringView text, int start, int end) -> std::optional<QList<SourceEdit>> {
        // Outside: "<!--" and an optional space before, an optional space and "-->" after.
        int openAt = start;
        if (openAt > 0 && text[openAt - 1] == u' ') --openAt;
        int closeEnd = end;
        if (closeEnd < text.size() && text[closeEnd] == u' ') ++closeEnd;
        if (openAt >= 4 && text.mid(openAt - 4, 4) == open && text.mid(closeEnd).startsWith(close))
            return QList<SourceEdit>{{{openAt - 4, start - openAt + 4}, {}}, {{end, closeEnd - end + 3}, {}}};
        // Inside: the selection starts with "<!--" and ends with "-->".
        const QStringView selected = text.mid(start, end - start);
        if (selected.size() >= 7 && selected.startsWith(open) && selected.endsWith(close)) {
            const int openLength = 4 + (selected[4] == u' ' ? 1 : 0);
            const int closeLength = 3 + (selected[selected.size() - 4] == u' ' && selected.size() > 8 ? 1 : 0);
            return QList<SourceEdit>{{{start, openLength}, {}}, {{end - closeLength, closeLength}, {}}};
        }
        return std::nullopt;
    };
    kind.wrap = [](QStringView, int start, int end) {
        return QList<SourceEdit>{{{start, 0}, open + u' '}, {{end, 0}, u' ' + close}};
    };
    kind.remove = [](QStringView text, int caret) -> std::optional<TextChange> {
        const Line line = lineAt(text, caret);
        const QStringView before = text.mid(line.start, caret - line.start);
        const int openAt = int(before.lastIndexOf(open));
        if (openAt < 0 || before.mid(openAt).contains(close)) return std::nullopt;
        const int closeAt = int(text.mid(caret, line.end - caret).indexOf(close));
        if (closeAt < 0) return std::nullopt;
        const int openStart = line.start + openAt;
        int openLength = 4;
        if (openStart + 4 < caret && text[openStart + 4] == u' ') ++openLength;
        int closeStart = caret + closeAt, closeLength = 3;
        if (closeStart > caret && text[closeStart - 1] == u' ') {
            --closeStart;
            ++closeLength;
        }
        const int at = caret - openLength;
        return TextChange{{{{openStart, openLength}, {}}, {{closeStart, closeLength}, {}}}, at, at};
    };
    kind.insert = [] { return std::pair{QStringLiteral("<!--  -->"), 5}; };
    kind.spansLines = true;
    return kind;
}

std::optional<InlineKind> inlineKindOf(const QString &command)
{
    if (const auto style = styleOf(command)) {
        InlineKind kind = styleKind(*style);
        // Superscript and subscript are written only where the dialect reads them.
        if (command == u"superscript" || command == u"subscript") {
            const QChar marker = style->marker.front();
            kind.fits = [marker](QStringView text, int start, int end) { return scriptFits(text, start, end, marker); };
        }
        return kind;
    }
    if (command == u"code") return codeKind();
    if (command == u"comment") return commentKind();
    return std::nullopt;
}

// The spans of a selection an inline command acts on: on each line it
// touches, the selected text after the line's block markers, without the
// spaces at its ends.
QList<std::pair<int, int>> segmentsOf(QStringView text, int start, int end)
{
    QList<std::pair<int, int>> segments;
    for (const Line &line : linesOf(text, start, end)) {
        const QStringView own = textOf(text, line);
        const Parts parts = partsOf(own);
        int from = line.start + parts.contentStart;
        const Atx atx = atxOf(own, parts.quoteEnd);
        if (atx.level > 0) from = line.start + atx.textStart;
        int first = qMax(start, from);
        int last = qMin(end, line.end);
        while (first < last && text[first].isSpace()) ++first;
        while (last > first && text[last - 1].isSpace()) --last;
        if (first < last) segments.append({first, last});
    }
    return segments;
}

std::optional<TextChange> toggleInline(const InlineKind &kind, QStringView text, int anchor, int cursor)
{
    const int start = qMin(anchor, cursor), end = qMax(anchor, cursor);
    if (start == end) {
        if (auto removed = kind.remove(text, start)) return removed;
        if (kind.fits && !kind.fits(text, start, start)) return std::nullopt;
        const auto [inserted, caret] = kind.insert();
        return TextChange{{{{start, 0}, inserted}}, start + caret, start + caret};
    }
    auto segments = segmentsOf(text, start, end);
    if (segments.isEmpty()) return std::nullopt;
    if (kind.spansLines) segments = {{segments.first().first, segments.last().second}};
    QList<std::optional<QList<SourceEdit>>> unwraps;
    bool allWrapped = true;
    for (const auto &[first, last] : segments) {
        unwraps.append(kind.unwrap(text, first, last));
        allWrapped = allWrapped && unwraps.last().has_value();
    }
    QList<SourceEdit> edits;
    for (int i = 0; i < segments.size(); ++i) {
        if (allWrapped) {
            edits += *unwraps[i];
        } else if (!unwraps[i]) {
            // A span that cannot take it leaves the whole selection as it is.
            if (kind.fits && !kind.fits(text, segments[i].first, segments[i].second)) return std::nullopt;
            edits += kind.wrap(text, segments[i].first, segments[i].second);
        }
    }
    sortEdits(edits);
    // The text of the segments stays selected, without the markers.
    const int newStart = mapPosition(segments.first().first, edits, !allWrapped);
    const int newEnd = mapPosition(segments.last().second, edits, allWrapped);
    return selecting(edits, newStart, newEnd, anchor <= cursor);
}

// ---- Links and images ----------------------------------------------------

std::optional<TextChange> toggleLink(QStringView text, int anchor, int cursor, bool image)
{
    const int start = qMin(anchor, cursor), end = qMax(anchor, cursor);
    // A link or image the selection is in loses its markup and keeps its text.
    static const QRegularExpression link(QStringLiteral("(!?)\\[([^\\]\\n\\r]*)\\]\\(([^)\\n\\r]*)\\)"));
    const Line line = lineAt(text, start);
    if (end <= line.end) {
        const QString own = textOf(text, line).toString();
        for (auto match = link.globalMatch(own); match.hasNext();) {
            const auto found = match.next();
            if (found.captured(1).isEmpty() == image) continue;
            const int from = line.start + int(found.capturedStart()), to = line.start + int(found.capturedEnd());
            if (start < from || end > to || (start == from && end == from) || (start == to && end == to)) continue;
            const QString label = found.captured(2);
            const int labelStart = line.start + int(found.capturedStart(2));
            const auto place = [&](int position) {
                return from + qBound(0, position - labelStart, int(label.size()));
            };
            return TextChange{{{{from, to - from}, label}}, place(anchor), place(cursor)};
        }
    }
    const QString prefix = image ? QStringLiteral("![") : QStringLiteral("[");
    if (start == end) {
        const QString label = image ? QStringLiteral("alt text") : QStringLiteral("text");
        const int url = start + int(prefix.size() + label.size()) + 2;
        return TextChange{{{{start, 0}, prefix + label + QStringLiteral("](url)")}}, url, url + 3};
    }
    const QString selected = text.mid(start, end - start).toString();
    static const QRegularExpression address(QStringLiteral("^(https?://|mailto:|www\\.)\\S+$"));
    if (!image && address.match(selected).hasMatch()) {
        const int label = start + 1;
        return TextChange{{{{start, end - start}, QStringLiteral("[text](") + selected + u')'}}, label, label + 4};
    }
    const int url = start + int(prefix.size() + selected.size()) + 2;
    return TextChange{{{{start, end - start}, prefix + selected + QStringLiteral("](url)")}}, url, url + 3};
}

// ---- Headings ------------------------------------------------------------

// A line's heading level: by its # marks, or by the underline on the line
// after it. 0 for a line that is no heading.
struct HeadingAt {
    int level = 0;
    Atx atx;
    // The underline of a Setext heading.
    std::optional<Line> underline;
};

HeadingAt headingAt(QStringView text, const Line &line)
{
    HeadingAt heading;
    const QStringView own = textOf(text, line);
    const Parts parts = partsOf(own);
    heading.atx = atxOf(own, parts.quoteEnd);
    if (heading.atx.level > 0) {
        heading.level = heading.atx.level;
        return heading;
    }
    if (isBlank(own) || parts.item() || fenceOf(own) || underlineLevel(own) || isThematicBreak(own)) return heading;
    if (const auto next = lineAfter(text, line)) {
        if (const int level = underlineLevel(textOf(text, *next))) {
            heading.level = level;
            heading.underline = next;
        }
    }
    return heading;
}

std::optional<TextChange> setHeading(QStringView text, int anchor, int cursor, int level, const Options &options)
{
    const int start = qMin(anchor, cursor), end = qMax(anchor, cursor);
    struct Target {
        Line line;
        HeadingAt heading;
    };
    QList<Target> targets;
    int underlineEnd = -1;
    for (const Line &line : linesOf(text, start, end)) {
        // The underline of a heading already taken is part of that heading.
        if (line.start == underlineEnd || isBlank(textOf(text, line))) continue;
        const HeadingAt heading = headingAt(text, line);
        if (heading.underline) underlineEnd = heading.underline->start;
        targets.append({line, heading});
    }
    if (targets.isEmpty()) {
        // A blank line becomes an empty heading.
        if (level == 0) return std::nullopt;
        targets.append({lineAt(text, start), {}});
    }
    // Every line at this level already: they become paragraphs again.
    if (level > 0 && std::all_of(targets.begin(), targets.end(), [&](const Target &t) { return t.heading.level == level; }))
        level = 0;
    QList<SourceEdit> edits;
    // A caret at the end of a heading stays there, before its new underline.
    QList<int> underlined;
    for (const Target &target : targets) {
        const Line &line = target.line;
        const QStringView own = textOf(text, line);
        const Parts parts = partsOf(own);
        const HeadingAt &heading = target.heading;
        // Where the heading's text starts, and what comes before it to keep.
        const int textStart = heading.atx.level > 0 ? heading.atx.textStart : parts.quoteEnd + leadingSpaces(own, parts.quoteEnd);
        const bool underline = level > 0 && level <= 2 && options.setextHeadings;
        const QString marks = level > 0 && !underline ? QString(level, u'#') + u' ' : QString();
        if (own.mid(parts.quoteEnd, textStart - parts.quoteEnd) != marks)
            edits.append(SourceEdit{{line.start + parts.quoteEnd, textStart - parts.quoteEnd}, marks});
        const QString underlineText =
            underline ? QString(qMax(3, displayWidth(own.mid(textStart).trimmed())), level == 1 ? u'=' : u'-') : QString();
        if (heading.underline) {
            if (underline) {
                // Only the underline's character changes, so its width stays the writer's.
                const QStringView old = textOf(text, *heading.underline);
                if (heading.level != level) {
                    QString changed = old.toString();
                    changed.replace(old[leadingSpaces(old)], level == 1 ? u'=' : u'-');
                    edits.append(SourceEdit{{heading.underline->start, heading.underline->length()}, changed});
                }
            } else {
                edits.append(SourceEdit{{line.end, heading.underline->end - line.end}, {}});
            }
        } else if (underline) {
            edits.append(SourceEdit{{line.end, 0}, options.newline + underlineText});
            underlined.append(line.end);
        }
    }
    if (edits.isEmpty()) return std::nullopt;
    return mapped(edits, anchor, cursor, [&](int position) { return underlined.contains(position); });
}

// ---- Quotes and lists ----------------------------------------------------

std::optional<TextChange> toggleQuote(QStringView text, int anchor, int cursor)
{
    const auto lines = linesOf(text, qMin(anchor, cursor), qMax(anchor, cursor));
    const auto quoted = [&](const Line &line) {
        const QStringView own = textOf(text, line);
        const int spaces = leadingSpaces(own);
        return spaces <= 3 && spaces < own.size() && own[spaces] == u'>';
    };
    bool all = false;
    for (const Line &line : lines) {
        if (isBlank(textOf(text, line))) continue;
        all = true;
        if (!quoted(line)) {
            all = false;
            break;
        }
    }
    QList<SourceEdit> edits;
    for (const Line &line : lines) {
        const QStringView own = textOf(text, line);
        if (all) {
            if (!quoted(line)) continue;
            const int at = leadingSpaces(own);
            const int length = 1 + (at + 1 < own.size() && own[at + 1] == u' ' ? 1 : 0);
            edits.append(SourceEdit{{line.start + at, length}, {}});
        } else {
            const bool blankInside = own.isEmpty() && lines.size() > 1;
            edits.append(SourceEdit{{line.start, 0}, blankInside ? QStringLiteral(">") : QStringLiteral("> ")});
        }
    }
    return mapped(edits, anchor, cursor);
}

enum class ListKind { None, Bullet, Ordered, Task };

ListKind kindOf(const Parts &parts)
{
    if (!parts.item()) return ListKind::None;
    if (parts.task) return ListKind::Task;
    return parts.ordered() ? ListKind::Ordered : ListKind::Bullet;
}

std::optional<TextChange> toggleList(QStringView text, int anchor, int cursor, ListKind wanted)
{
    const auto all = linesOf(text, qMin(anchor, cursor), qMax(anchor, cursor));
    QList<Line> lines;
    for (const Line &line : all)
        if (!isBlank(textOf(text, line))) lines.append(line);
    if (lines.isEmpty()) lines.append(all.first());
    const bool off = std::all_of(lines.begin(), lines.end(), [&](const Line &line) {
        return kindOf(partsOf(textOf(text, line))) == wanted;
    });
    // Numbers count on from a numbered item just above, at each indentation.
    QHash<int, qint64> next;
    QHash<int, QChar> delimiters;
    if (const auto above = lineBefore(text, lines.first())) {
        const Parts parts = partsOf(textOf(text, *above));
        if (parts.ordered()) {
            next[parts.indent()] = parts.number + 1;
            delimiters[parts.indent()] = parts.delimiter;
        }
    }
    QList<SourceEdit> edits;
    for (const Line &line : lines) {
        const QStringView own = textOf(text, line);
        const Parts parts = partsOf(own);
        const SourceRange marker{line.start + parts.indentEnd, parts.contentStart - parts.indentEnd};
        if (off) {
            edits.append(SourceEdit{marker, {}});
            continue;
        }
        // A shallower line ends the numbering of deeper ones.
        for (auto it = next.begin(); it != next.end();) {
            if (it.key() > parts.indent()) it = next.erase(it);
            else ++it;
        }
        QString prefix;
        switch (wanted) {
        case ListKind::Bullet:
            prefix = QString(parts.item() && !parts.ordered() ? parts.bullet : u'-') + u' ';
            break;
        case ListKind::Ordered: {
            const qint64 number = next.value(parts.indent(), 1);
            next[parts.indent()] = number + 1;
            prefix = QString::number(number) + delimiters.value(parts.indent(), u'.') + u' ';
            break;
        }
        case ListKind::Task:
            if (parts.task) continue;
            prefix = parts.item() ? own.mid(parts.markerStart, parts.markerEnd - parts.markerStart).toString() + u' '
                                  : QStringLiteral("- ");
            prefix += QStringLiteral("[ ] ");
            break;
        case ListKind::None:
            break;
        }
        if (own.mid(parts.indentEnd, parts.contentStart - parts.indentEnd) != prefix) edits.append(SourceEdit{marker, prefix});
    }
    if (edits.isEmpty()) return std::nullopt;
    return mapped(edits, anchor, cursor);
}

std::optional<TextChange> toggleTask(QStringView text, int anchor, int cursor)
{
    const auto lines = linesOf(text, qMin(anchor, cursor), qMax(anchor, cursor));
    bool anyOpen = false;
    for (const Line &line : lines) {
        const Parts parts = partsOf(textOf(text, line));
        anyOpen = anyOpen || (parts.task && !parts.checked);
    }
    QList<SourceEdit> edits;
    for (const Line &line : lines) {
        const QStringView own = textOf(text, line);
        const Parts parts = partsOf(own);
        if (parts.task) {
            if (parts.checked != anyOpen) edits.append(SourceEdit{{line.start + parts.boxStart + 1, 1}, anyOpen ? QStringLiteral("x") : QStringLiteral(" ")});
        } else if (lines.size() == 1 || !isBlank(own)) {
            if (parts.item()) edits.append(SourceEdit{{line.start + parts.spaceEnd, 0}, QStringLiteral("[ ] ")});
            else edits.append(SourceEdit{{line.start + parts.indentEnd, 0}, QStringLiteral("- [ ] ")});
        }
    }
    if (edits.isEmpty()) return std::nullopt;
    return mapped(edits, anchor, cursor);
}

// ---- Fenced blocks ---------------------------------------------------------

// Removes a whole line together with one of the line breaks beside it: the
// one before it for a line that closes something, where there is one.
SourceEdit removeLine(QStringView text, const Line &line, bool closing = false)
{
    const auto before = lineBefore(text, line);
    if (line.hasBreak() && !(closing && before)) return {{line.start, line.next - line.start}, {}};
    if (before) return {{before->end, line.end - before->end}, {}};
    return {{line.start, line.length()}, {}};
}

// The edits that take away a block's opening and closing fence lines. The
// fences of an empty block share a line break, so they go together, with
// one line break, rather than each with a break of its own.
QList<SourceEdit> removeFences(QStringView text, const Line &open, const Line &close)
{
    if (close.start != open.next) return {removeLine(text, open), removeLine(text, close, true)};
    if (close.hasBreak()) return {{{open.start, close.next - open.start}, {}}};
    if (const auto before = lineBefore(text, open)) return {{{before->end, close.end - before->end}, {}}};
    return {{{open.start, close.end - open.start}, {}}};
}

// Wraps the lines a selection touches between fence lines, or takes the
// fences away from lines that have them; a caret on a blank line gets an
// empty block to write in.
std::optional<TextChange> toggleFencedBlock(QStringView text, int anchor, int cursor, const Options &options, bool math)
{
    const int start = qMin(anchor, cursor), end = qMax(anchor, cursor);
    const auto lines = linesOf(text, start, end);
    const QString &nl = options.newline;
    const auto opensAndCloses = [&](const Line &open, const Line &close) {
        if (math) return isDisplayMathFence(textOf(text, open)) && isDisplayMathFence(textOf(text, close));
        const auto blocks = fencedBlocks(text);
        const auto found = blocks.find(open.start);
        return found != blocks.end() && found->second == close.start;
    };
    // The selection holds the fences, or the lines around it are fences.
    if (lines.size() >= 2 && opensAndCloses(lines.first(), lines.last()))
        return mapped(removeFences(text, lines.first(), lines.last()), anchor, cursor);
    const auto above = lineBefore(text, lines.first());
    const auto below = lineAfter(text, lines.last());
    if (above && below && opensAndCloses(*above, *below))
        return mapped(removeFences(text, *above, *below), anchor, cursor);

    QString fence = math ? QStringLiteral("$$") : QStringLiteral("```");
    if (lines.size() == 1 && isBlank(textOf(text, lines.first()))) {
        const Line &line = lines.first();
        const int caret = line.start + int(fence.size() + nl.size());
        return TextChange{{{{line.start, line.length()}, fence + nl + nl + fence}}, caret, caret};
    }
    if (!math) {
        // Longer than any fence inside, so that none of them closes it.
        int longest = 0;
        for (const Line &line : lines)
            if (const auto inner = fenceOf(textOf(text, line)); inner && inner->mark == u'`')
                longest = qMax(longest, inner->length);
        if (longest >= 3) fence = QString(longest + 1, u'`');
    }
    return mappedInside({{{lines.first().start, 0}, fence + nl}, {{lines.last().end, 0}, nl + fence}}, anchor, cursor);
}

// Display math: $$ around a selection within one line, or a block of lines.
std::optional<TextChange> toggleDisplayMath(QStringView text, int anchor, int cursor, const Options &options)
{
    const int start = qMin(anchor, cursor), end = qMax(anchor, cursor);
    const Line line = lineAt(text, start);
    const QStringView own = textOf(text, line);
    if (start < end && end <= line.end && !isDisplayMathFence(own)) {
        const QStringView selected = text.mid(start, end - start);
        if (runBefore(text, start, u'$') == 2 && runAfter(text, end, u'$') == 2)
            return selecting({{{start - 2, 2}, {}}, {{end, 2}, {}}}, start - 2, end - 2, anchor <= cursor);
        if (selected.size() > 4 && selected.startsWith(u"$$") && selected.endsWith(u"$$"))
            return selecting({{{start, 2}, {}}, {{end - 2, 2}, {}}}, start, end - 4, anchor <= cursor);
        if (selected.trimmed() != own.trimmed())
            return selecting({{{start, 0}, QStringLiteral("$$")}, {{end, 0}, QStringLiteral("$$")}}, start + 2, end + 2,
                             anchor <= cursor);
    }
    return toggleFencedBlock(text, anchor, cursor, options, true);
}

// ---- Insertions -----------------------------------------------------------

// A block of its own after the line the selection ends on, or in place of
// that line when it is blank, with a blank line before it and after it
// where there is text. The caret goes to the line after the block, or the
// block's own text from `select` for `length` is selected.
TextChange insertBlock(QStringView text, int position, const QString &block, int select, int length,
                       const Options &options)
{
    const QString &nl = options.newline;
    const Line line = lineAt(text, position);
    QString before, after;
    int at = line.end, removed = 0;
    if (isBlank(textOf(text, line))) {
        at = line.start;
        removed = line.length();
        if (const auto above = lineBefore(text, line); above && !isBlank(textOf(text, *above))) before = nl;
    } else {
        before = nl + nl;
    }
    if (!line.hasBreak()) {
        after = nl;
    } else if (const auto below = lineAfter(text, line); below && !isBlank(textOf(text, *below))) {
        after = nl;
    }
    const int blockStart = at + int(before.size());
    TextChange change;
    change.edits = {{{at, removed}, before + block + after}};
    if (select >= 0) {
        change.anchor = blockStart + select;
        change.cursor = change.anchor + length;
    } else {
        // Past the line break after the block: the one written, or else the
        // line's own, which may be another than the document's.
        const int lineBreak = after.isEmpty() ? line.next - line.end : int(after.size());
        change.anchor = change.cursor = blockStart + int(block.size()) + lineBreak;
    }
    return change;
}

TextChange insertTable(QStringView text, int position, const Options &options)
{
    const QString &nl = options.newline;
    const QString block = QStringLiteral("| Column 1 | Column 2 |") + nl + QStringLiteral("| -------- | -------- |") + nl
                          + QStringLiteral("|          |          |");
    return insertBlock(text, position, block, 2, 8, options);
}

// A note's definition, `reference: `, at the end of the text, after a
// blank line or straight after other definitions; and where the caret goes
// in it, as an offset into the text after the edit.
std::pair<SourceEdit, int> definitionAtEnd(QStringView text, const QString &reference, const Options &options)
{
    const QString &nl = options.newline;
    const int size = int(text.size());
    QString lead;
    const bool endsWithBreak = size > 0 && isBreak(text[size - 1]);
    if (size > 0) {
        const Line last = lineAt(text, endsWithBreak ? size - 1 : size);
        static const QRegularExpression definition(QStringLiteral("^ {0,3}\\[\\^[^\\]]+\\]:"));
        const QString lastText = textOf(text, last).toString();
        const bool afterDefinition = definition.match(lastText).hasMatch();
        if (!endsWithBreak) lead = afterDefinition ? nl : nl + nl;
        else if (!isBlank(lastText) && !afterDefinition) lead = nl;
    }
    const QString note = lead + reference + QStringLiteral(": ") + (endsWithBreak ? nl : QString());
    return {{{size, 0}, note}, size + int(lead.size() + reference.size()) + 2};
}

TextChange insertFootnote(QStringView text, int anchor, int cursor, const Options &options)
{
    static const QRegularExpression label(QStringLiteral("\\[\\^(\\d{1,9})\\]"));
    qint64 highest = 0;
    for (auto match = label.globalMatch(text); match.hasNext();) highest = qMax(highest, match.next().captured(1).toLongLong());
    const QString reference = QStringLiteral("[^%1]").arg(highest + 1);
    const int end = qMax(anchor, cursor);
    // The definition goes at the end, after the reference put in before it.
    const auto [definition, caret] = definitionAtEnd(text, reference, options);
    return TextChange{{{{end, 0}, reference}, definition}, caret + int(reference.size()),
                      caret + int(reference.size())};
}

// ---- Tables ----------------------------------------------------------------

MdTable::Dialect dialectOf(const Options &options)
{
    MdTable::Dialect dialect;
    dialect.protectCode = options.protectCodePipes;
    dialect.mergedCells = options.mergedCells;
    dialect.lineComments = options.lineComments;
    return dialect;
}

// The cell's text selected, or a caret inside an empty cell.
TextChange selectCell(const MdTable::Cell &cell)
{
    if (cell.start < cell.end) return {{}, cell.start, cell.end};
    const int caret = qMin(cell.rawStart + 1, cell.rawEnd);
    return {{}, caret, caret};
}

// The cells a row has written, without those the header gives it beyond.
QList<int> writtenCells(const MdTable::Table &table, int row)
{
    QList<int> cells;
    if (row == 1) return cells;
    for (int i = 0; i < table.rows[row].cells.size(); ++i)
        if (!table.rows[row].cells[i].missing) cells.append(i);
    return cells;
}

// ---- Notes that nothing defines -------------------------------------------

// Whether a line lies inside a fenced code block, counting the fences
// above it.
bool insideFence(QStringView text, const Line &line)
{
    std::optional<Fence> open;
    for (Line at = lineAt(text, 0); at.start < line.start;) {
        if (const auto fence = fenceOf(textOf(text, at))) {
            if (!open) open = fence;
            else if (fence->mark == open->mark && fence->length >= open->length && !fence->hasInfo) open.reset();
        }
        if (!at.hasBreak()) break;
        at = lineAt(text, at.next);
    }
    return open.has_value();
}

// The undefined note reference at a position, as the range of its
// `[^label]` and its label.
struct NoteReference {
    int start = 0;
    int end = 0;
    QString label;
};

std::optional<NoteReference> noteReferenceAt(QStringView text, int position)
{
    const Line line = lineAt(text, position);
    const QString own = textOf(text, line).toString();
    static const QRegularExpression reference(QStringLiteral("\\[\\^([\\p{L}\\p{N}_:-]+)\\]"));
    for (auto match = reference.globalMatch(own); match.hasNext();) {
        const QRegularExpressionMatch found = match.next();
        const int at = int(found.capturedStart()), start = line.start + at, end = line.start + int(found.capturedEnd());
        if (position < start || position > end) continue;
        // A definition's own marker, an escaped bracket, a code span.
        if (end < line.end && text[end] == u':' && leadingSpaces(own) == at) return std::nullopt;
        if (at > 0 && own[at - 1] == u'\\') return std::nullopt;
        if (own.left(at).count(u'`') % 2 == 1) return std::nullopt;
        if (insideFence(text, line)) return std::nullopt;
        // Defined anywhere, by a label that differs only in case.
        const QString label = found.captured(1);
        const QRegularExpression definition(
            QStringLiteral("^ {0,3}\\[\\^%1\\]:").arg(QRegularExpression::escape(label)),
            QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
        if (definition.match(text).hasMatch()) return std::nullopt;
        return NoteReference{start, end, label};
    }
    return std::nullopt;
}

// [^label] becomes [^label.], a note of its own, with the caret after the label.
std::optional<TextChange> makeNoteInline(QStringView text, int cursor, const Options &options)
{
    if (!options.footnotes || !options.inlineNotes) return std::nullopt;
    const auto reference = noteReferenceAt(text, cursor);
    if (!reference) return std::nullopt;
    const int at = reference->end - 1;
    return TextChange{{{{at, 0}, QStringLiteral(".")}}, at, at};
}

// [^label]: at the end, for the note to be written there.
std::optional<TextChange> defineNote(QStringView text, int cursor, const Options &options)
{
    if (!options.footnotes) return std::nullopt;
    const auto reference = noteReferenceAt(text, cursor);
    if (!reference) return std::nullopt;
    const auto [edit, caret] = definitionAtEnd(text, QStringLiteral("[^%1]").arg(reference->label), options);
    return TextChange{{edit}, caret, caret};
}

// ---- Ordered lists -------------------------------------------------------

// How deep in block quotes a line is.
int quoteDepth(QStringView line, const Parts &parts) { return int(line.left(parts.quoteEnd).count(u'>')); }

struct Item {
    Line line;
    Parts parts;
    // The number's place in the text.
    SourceRange number() const { return {line.start + parts.markerStart, parts.markerEnd - parts.markerStart - 1}; }
};

// The items of the ordered list that holds the item on `line`, in order:
// those at its indentation, in as many quotes and with its delimiter.
// Items nested deeper, the lines that continue an item, and blank lines
// between items are inside the list. An item less indented, one of another
// kind at its indentation, another depth of quotes, or a line less
// indented than the item's text that follows a blank line or starts a
// block of its own, ends it. Empty for a line that is no numbered item.
QList<Item> orderedList(QStringView text, const Line &line)
{
    const QStringView ownText = textOf(text, line);
    const Parts own = partsOf(ownText);
    if (!own.ordered()) return {};
    const int depth = quoteDepth(ownText, own);
    enum class Place { Sibling, Inside, End, Blank };
    // Where a line stands to the list, the text of the items starting
    // `content` columns in, and the line before it blank or not.
    const auto place = [&](const Line &candidate, int content, bool afterBlank) {
        const QStringView view = textOf(text, candidate);
        const Parts parts = partsOf(view);
        if (isBlank(view.mid(parts.quoteEnd))) return Place::Blank;
        if (quoteDepth(view, parts) != depth) return Place::End;
        if (parts.item()) {
            if (parts.indent() > own.indent()) return Place::Inside;
            if (parts.indent() < own.indent()) return Place::End;
            return parts.ordered() && parts.delimiter == own.delimiter ? Place::Sibling : Place::End;
        }
        if (parts.indent() >= content) return Place::Inside;
        const QStringView rest = view.mid(parts.quoteEnd);
        if (afterBlank || isThematicBreak(rest) || atxOf(view, parts.quoteEnd).level > 0 || fenceOf(rest))
            return Place::End;
        return Place::Inside;
    };
    const int content = own.contentStart - own.quoteEnd;
    // The first item, found upwards: text less indented than the items'
    // ends the list above it when a blank line is above that text.
    Line first = line;
    for (auto above = lineBefore(text, line); above; above = lineBefore(text, *above)) {
        const auto higher = lineBefore(text, *above);
        const bool blankAbove = !higher || [&] {
            const QStringView view = textOf(text, *higher);
            return isBlank(view.mid(partsOf(view).quoteEnd));
        }();
        const Place at = place(*above, content, blankAbove);
        if (at == Place::End) break;
        if (at == Place::Sibling) first = *above;
    }
    // Then every item from the first down.
    QList<Item> items{{first, partsOf(textOf(text, first))}};
    bool afterBlank = false;
    for (auto below = lineAfter(text, first); below; below = lineAfter(text, *below)) {
        const Parts &last = items.last().parts;
        const Place at = place(*below, last.contentStart - last.quoteEnd, afterBlank);
        if (at == Place::End) break;
        if (at == Place::Sibling) items.append({*below, partsOf(textOf(text, *below))});
        afterBlank = at == Place::Blank;
    }
    return items;
}

// The edits that number the items from `first` on counting up from `number`,
// for those whose number is not that already.
QList<SourceEdit> numbered(const QList<Item> &items, qsizetype first, qint64 number)
{
    QList<SourceEdit> edits;
    for (qsizetype i = first; i < items.size(); ++i, ++number)
        if (items[i].parts.number != number) edits.append({items[i].number(), QString::number(number)});
    return edits;
}

std::optional<TextChange> renumberList(QStringView text, int anchor, int cursor)
{
    // On a line that continues an item, the numbered item it belongs to:
    // the nearest above that is no deeper than the line.
    Line line = lineAt(text, qMin(anchor, cursor));
    const QStringView view = textOf(text, line);
    if (!partsOf(view).item()) {
        const int indent = partsOf(view).indent();
        std::optional<Line> found;
        for (auto above = lineBefore(text, line); above && !found; above = lineBefore(text, *above)) {
            const Parts parts = partsOf(textOf(text, *above));
            if (!parts.item() || parts.indent() > indent) continue;
            if (!parts.ordered()) return std::nullopt;
            found = above;
        }
        if (!found) return std::nullopt;
        line = *found;
    }
    const QList<Item> items = orderedList(text, line);
    if (items.isEmpty()) return std::nullopt;
    const QList<SourceEdit> edits = numbered(items, 0, items.first().parts.number);
    if (edits.isEmpty()) return std::nullopt;
    return mapped(edits, anchor, cursor);
}

} // namespace

// ---- Public ----------------------------------------------------------------

QStringList commands()
{
    return {QStringLiteral("bold"),        QStringLiteral("italic"),       QStringLiteral("strikethrough"),
            QStringLiteral("highlight"),   QStringLiteral("superscript"),  QStringLiteral("subscript"),
            QStringLiteral("code"),        QStringLiteral("comment"),
            QStringLiteral("heading1"),    QStringLiteral("heading2"),     QStringLiteral("heading3"),
            QStringLiteral("heading4"),    QStringLiteral("heading5"),     QStringLiteral("heading6"),
            QStringLiteral("paragraph"),   QStringLiteral("quote"),        QStringLiteral("bulletList"),
            QStringLiteral("numberedList"), QStringLiteral("renumberList"), QStringLiteral("taskList"),
            QStringLiteral("toggleTask"),
            QStringLiteral("codeBlock"),   QStringLiteral("link"),         QStringLiteral("image"),
            QStringLiteral("footnote"),    QStringLiteral("inlineNote"),   QStringLiteral("defineNote"),
            QStringLiteral("table"),       QStringLiteral("formatTable"),
            QStringLiteral("inlineMath"),  QStringLiteral("displayMath"),  QStringLiteral("thematicBreak"),
            QStringLiteral("pageBreak")};
}

bool isCommand(const QString &id)
{
    static const QStringList all = commands();
    return all.contains(id);
}

std::optional<TextChange> format(const QString &command, QStringView text, int anchor, int cursor, const Options &options)
{
    const int size = int(text.size());
    if (anchor < 0 || cursor < 0 || anchor > size || cursor > size) return std::nullopt;
    if (const auto kind = inlineKindOf(command)) return toggleInline(*kind, text, anchor, cursor);
    if (command.startsWith(u"heading") && command.size() == 8) {
        const int level = command.back().digitValue();
        if (level >= 1 && level <= 6) return setHeading(text, anchor, cursor, level, options);
    }
    const int end = qMax(anchor, cursor);
    if (command == u"paragraph") return setHeading(text, anchor, cursor, 0, options);
    if (command == u"quote") return toggleQuote(text, anchor, cursor);
    if (command == u"bulletList") return toggleList(text, anchor, cursor, ListKind::Bullet);
    if (command == u"numberedList") return toggleList(text, anchor, cursor, ListKind::Ordered);
    if (command == u"taskList") return toggleList(text, anchor, cursor, ListKind::Task);
    if (command == u"toggleTask") return toggleTask(text, anchor, cursor);
    if (command == u"codeBlock") return toggleFencedBlock(text, anchor, cursor, options, false);
    if (command == u"displayMath") return toggleDisplayMath(text, anchor, cursor, options);
    if (command == u"link") return toggleLink(text, anchor, cursor, false);
    if (command == u"image") return toggleLink(text, anchor, cursor, true);
    if (command == u"thematicBreak") return insertBlock(text, end, QStringLiteral("---"), -1, 0, options);
    if (command == u"pageBreak") return insertBlock(text, end, QStringLiteral("+++"), -1, 0, options);
    if (command == u"table") return insertTable(text, end, options);
    if (command == u"footnote") return insertFootnote(text, anchor, cursor, options);
    if (command == u"formatTable") return formatTable(text, anchor, cursor, options);
    if (command == u"renumberList") return renumberList(text, anchor, cursor);
    if (command == u"inlineNote") return makeNoteInline(text, cursor, options);
    if (command == u"defineNote") return defineNote(text, cursor, options);
    return std::nullopt;
}

std::optional<TextChange> newLine(QStringView text, int anchor, int cursor, const Options &options)
{
    if (anchor != cursor) return std::nullopt;
    const Line line = lineAt(text, cursor);
    const QStringView own = textOf(text, line);
    const Parts parts = partsOf(own);
    if (!parts.item() || cursor < line.start + parts.contentStart) return std::nullopt;
    if (isBlank(own.mid(parts.contentStart))) {
        // An empty item ends the list; the line keeps its quote markers.
        const int at = line.start + parts.quoteEnd;
        return TextChange{{{{at, line.end - at}, {}}}, at, at};
    }
    QString prefix = own.left(parts.indentEnd).toString();
    // A numbered list goes on repeating one number when every item has it,
    // and otherwise counts on; when it counts up, the items after the new
    // one count on from it, in the same step.
    QList<SourceEdit> following;
    if (parts.ordered()) {
        const QList<Item> items = orderedList(text, line);
        qsizetype here = 0;
        while (here < items.size() && items[here].line.start != line.start) ++here;
        bool repeated = items.size() > 1, countsUp = true;
        for (qsizetype i = 1; i < items.size(); ++i) {
            repeated = repeated && items[i].parts.number == items[0].parts.number;
            countsUp = countsUp && items[i].parts.number == items[i - 1].parts.number + 1;
        }
        const qint64 number = repeated ? parts.number : parts.number + 1;
        if (countsUp && !repeated) following = numbered(items, here + 1, number + 1);
        prefix += QString::number(number) + parts.delimiter;
    } else {
        prefix += parts.bullet;
    }
    const QStringView spacing = own.mid(parts.markerEnd, parts.spaceEnd - parts.markerEnd);
    prefix += spacing.isEmpty() ? QStringLiteral(" ") : spacing.toString();
    if (parts.task) prefix += QStringLiteral("[ ] ");
    // The text after the caret moves to the new item, without the spaces before it.
    const int moved = leadingSpaces(text.mid(cursor, line.end - cursor));
    const QString inserted = options.newline + prefix;
    const int caret = cursor + int(inserted.size());
    QList<SourceEdit> edits{{{cursor, moved}, inserted}};
    edits += following;
    return TextChange{edits, caret, caret};
}

TextChange hardBreak(QStringView, int anchor, int cursor, const Options &options)
{
    const int start = qMin(anchor, cursor), end = qMax(anchor, cursor);
    const QString inserted = QStringLiteral("  ") + options.newline;
    const int caret = start + int(inserted.size());
    return {{{{start, end - start}, inserted}}, caret, caret};
}

std::optional<TextChange> indent(QStringView text, int anchor, int cursor, bool outdent, const Options &)
{
    const auto lines = linesOf(text, qMin(anchor, cursor), qMax(anchor, cursor));
    if (lines.size() == 1 && !partsOf(textOf(text, lines.first())).item()) return std::nullopt;
    // The list item above at an indentation, the nearest one: the item a
    // deeper item belongs to and what it lines up with.
    const auto itemAbove = [&](const Line &line, const std::function<bool(const Parts &)> &wanted) -> std::optional<Parts> {
        for (auto above = lineBefore(text, line); above; above = lineBefore(text, *above)) {
            const QStringView own = textOf(text, *above);
            if (isBlank(own)) continue;
            const Parts parts = partsOf(own);
            if (parts.item() && wanted(parts)) return parts;
            if (!parts.item() && parts.indent() == 0) break;
        }
        return std::nullopt;
    };
    QList<SourceEdit> edits;
    for (const Line &line : lines) {
        const QStringView own = textOf(text, line);
        if (isBlank(own)) continue;
        const Parts parts = partsOf(own);
        const int at = line.start + parts.quoteEnd;
        const bool tabbed = parts.indent() > 0 && own[parts.quoteEnd] == u'\t';
        if (!outdent) {
            QString unit = QStringLiteral("\t");
            if (parts.item() && !tabbed) {
                // As deep as the text of the item above at this depth, so that this one belongs to it.
                const auto sibling = itemAbove(line, [&](const Parts &p) { return p.indent() == parts.indent(); });
                const Parts &like = sibling ? *sibling : parts;
                unit = QString(like.spaceEnd - like.indentEnd, u' ');
            }
            edits.append(SourceEdit{{at, 0}, unit});
            continue;
        }
        if (parts.indent() == 0) continue;
        int removed = 0;
        if (tabbed) {
            removed = 1;
        } else if (parts.item()) {
            const auto parent = itemAbove(line, [&](const Parts &p) { return p.indent() < parts.indent(); });
            removed = parts.indent() - (parent ? parent->indent() : 0);
        } else {
            removed = qMin(4, leadingSpaces(own.mid(parts.quoteEnd)));
        }
        edits.append(SourceEdit{{at, removed}, {}});
    }
    if (edits.isEmpty()) return lines.size() > 1 ? std::optional<TextChange>(TextChange{{}, anchor, cursor}) : std::nullopt;
    return mapped(edits, anchor, cursor);
}

std::optional<TextChange> moveToCell(QStringView text, int anchor, int cursor, bool backwards, const Options &options)
{
    const auto table = MdTable::locate(text, cursor, dialectOf(options));
    if (!table) return std::nullopt;
    int row = MdTable::rowAt(*table, cursor);
    if (row < 0) return std::nullopt;
    const int rows = int(table->rows.size());
    if (!backwards) {
        if (row != 1) {
            const QList<int> cells = writtenCells(*table, row);
            const int here = MdTable::cellAt(*table, row, cursor);
            for (int i : cells)
                if (i > here) return selectCell(table->rows[row].cells[i]);
        }
        int next = row + 1;
        if (next == 1) ++next;
        if (next < rows) return selectCell(table->rows[next].cells[writtenCells(*table, next).value(0)]);
        // Past the last cell: a new row as wide as the header.
        const MdTable::Line &last = table->rows.last().line;
        const int prefix = last.content - last.start;
        QString added = text.mid(last.start, prefix).toString() + u'|';
        for (int i = 0; i < table->columns; ++i) added += QStringLiteral("  |");
        const int caret = last.end + int(options.newline.size()) + prefix + 2;
        return TextChange{{{{last.end, 0}, options.newline + added}}, caret, caret};
    }
    if (row != 1) {
        const QList<int> cells = writtenCells(*table, row);
        const int here = MdTable::cellAt(*table, row, cursor);
        for (auto i = cells.rbegin(); i != cells.rend(); ++i)
            if (*i < here) return selectCell(table->rows[row].cells[*i]);
        // Before the first cell there is nothing to go back to.
        if (row == 0) return TextChange{{}, anchor, cursor};
    }
    const int previous = row <= 2 ? 0 : row - 1;
    return selectCell(table->rows[previous].cells[writtenCells(*table, previous).last()]);
}

std::optional<TextChange> formatTable(QStringView text, int anchor, int cursor, const Options &options)
{
    const auto table = MdTable::locate(text, cursor, dialectOf(options));
    if (!table) return std::nullopt;
    const MdTable::Layout layout = MdTable::write(MdTable::model(text, *table));
    const QStringView before = text.mid(table->start, table->end - table->start);
    if (before == layout.text) return std::nullopt;
    // The selection keeps its cells, at the same place in their text.
    const int shift = int(layout.text.size() - before.size());
    const auto place = [&](int position) {
        if (position < table->start) return position;
        if (position > table->end) return position + shift;
        const int row = MdTable::rowAt(*table, position);
        const int modelRow = MdTable::modelRowOf(row);
        if (modelRow < 0) {
            const int line = table->start + layout.lineStarts[1];
            const int next = table->start + layout.lineStarts.value(2, int(layout.text.size()) + 1) - 1;
            return qMin(line + position - table->rows[1].line.start, next);
        }
        const int index = MdTable::cellAt(*table, row, position);
        const MdTable::Cell &cell = table->rows[row].cells[index];
        const MdTable::Placed &placed = layout.cells[modelRow][index];
        return table->start + placed.start + qBound(0, position - cell.start, placed.length);
    };
    int same = 0;
    const int shortest = int(qMin(before.size(), layout.text.size()));
    while (same < shortest && before[same] == layout.text[same]) ++same;
    int tail = 0;
    while (tail < shortest - same && before[before.size() - 1 - tail] == layout.text[layout.text.size() - 1 - tail]) ++tail;
    const SourceEdit edit{{table->start + same, int(before.size()) - same - tail},
                          layout.text.mid(same, layout.text.size() - same - tail)};
    return TextChange{{edit}, place(anchor), place(cursor)};
}

QString undefinedNoteAt(QStringView text, int position)
{
    if (position < 0 || position > text.size()) return {};
    const auto reference = noteReferenceAt(text, position);
    return reference ? reference->label : QString();
}

int displayWidth(QStringView text) { return MdTable::displayWidth(text); }

} // namespace MarkdownEditing
