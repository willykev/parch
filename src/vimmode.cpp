#include "vimmode.h"

#include "document.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QHash>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMetaObject>
#include <QSet>
#include <QStringView>
#include <algorithm>
#include <climits>
#include <utility>

namespace {

// Counts above this are taken as this, so that a slip on the keys cannot
// keep the view busy for long.
constexpr int maxCount = 9999;
// A stay in Insert mode with more keys than this is not repeated.
constexpr qsizetype maxInsertKeys = 100000;
// A put that would add more UTF-16 units than this is refused.
constexpr qsizetype maxPutUnits = 10'000'000;

bool isBreak(QChar c) { return c == u'\n' || c == u'\r' || c == QChar(0x2029); }
bool isBlank(QChar c) { return c == u' ' || c == u'\t'; }

// Vim's classes of characters: 0 for blanks and line ends, 1 for
// punctuation, 2 for the characters of words, and a class each for Han,
// Hiragana, Katakana and Hangul, so that a word ends where its script does.
int classOf(char32_t c)
{
    if (c == u' ' || c == u'\t' || c == 0xa0 || c == 0x3000 || QChar::isSpace(c)) return 0;
    switch (QChar::script(c)) {
    case QChar::Script_Han:
        return 3;
    case QChar::Script_Hiragana:
        return 4;
    case QChar::Script_Katakana:
        return 5;
    case QChar::Script_Hangul:
        return 6;
    default:
        break;
    }
    if (c == u'_' || QChar::isLetterOrNumber(c) || QChar::isMark(c)) return 2;
    return 1;
}

bool isKeyword(char32_t c) { return classOf(c) >= 2; }

// The text as Vim sees it. Each line has positions from its start to its
// end, the place of its line break or of the end of the text, which Vim
// calls NUL; a line break is LF, CRLF, a lone CR or U+2029. Positions move
// by whole graphemes.
struct Text {
    QStringView s;

    int size() const { return int(s.size()); }
    // On a line end.
    bool atEnd(int p) const { return p >= size() || isBreak(s[p]); }
    char32_t at(int p) const
    {
        if (p < 0 || p >= size()) return 0;
        const QChar c = s[p];
        if (c.isHighSurrogate() && p + 1 < size() && s[p + 1].isLowSurrogate())
            return QChar::surrogateToUcs4(c, s[p + 1]);
        return c.unicode();
    }
    char32_t before(int p) const
    {
        if (p <= 0) return 0;
        const QChar c = s[p - 1];
        if (c.isLowSurrogate() && p >= 2 && s[p - 2].isHighSurrogate()) return QChar::surrogateToUcs4(s[p - 2], c);
        return c.unicode();
    }
    int lineStart(int p) const
    {
        while (p > 0 && !isBreak(s[p - 1])) --p;
        return p;
    }
    int lineEnd(int p) const
    {
        while (p < size() && !isBreak(s[p])) ++p;
        return p;
    }
    // How long the line break at a line's end is: 0 at the end of the text.
    int breakAt(int end) const
    {
        if (end >= size()) return 0;
        return s[end] == u'\r' && end + 1 < size() && s[end + 1] == u'\n' ? 2 : 1;
    }
    bool lastLine(int p) const { return lineEnd(p) == size(); }
    // The start of the next line, or -1 on the last.
    int nextLine(int p) const
    {
        const int end = lineEnd(p);
        return end < size() ? end + breakAt(end) : -1;
    }
    // The end of the line before the one starting at `start`, which is not the first.
    int previousLineEnd(int start) const
    {
        return start - (start >= 2 && s[start - 2] == u'\r' && s[start - 1] == u'\n' ? 2 : 1);
    }
    // The start of the line before, or -1 on the first.
    int previousLine(int p) const
    {
        const int start = lineStart(p);
        return start > 0 ? lineStart(previousLineEnd(start)) : -1;
    }
    bool lineEmpty(int p) const
    {
        const int start = lineStart(p);
        return start == lineEnd(start);
    }
    // Empty, or only blanks.
    bool lineWhite(int p) const
    {
        for (int i = lineStart(p), end = lineEnd(p); i < end; ++i)
            if (!isBlank(s[i])) return false;
        return true;
    }
    int firstNonBlank(int p) const
    {
        int i = lineStart(p);
        const int end = lineEnd(i);
        while (i < end && isBlank(s[i])) ++i;
        return i;
    }
    // The line's last character, or its start when it is empty.
    int lastChar(int p) const
    {
        const int start = lineStart(p), end = lineEnd(p);
        return end > start ? previous(end) : start;
    }
    int next(int p) const
    {
        const int n = nextSourceGrapheme(s, p);
        return n < 0 ? size() : n;
    }
    int previous(int p) const
    {
        const int n = previousSourceGrapheme(s, p);
        return n < 0 ? 0 : n;
    }
    // Only blanks before p on its line.
    bool inIndent(int p) const
    {
        for (int i = lineStart(p); i < p; ++i)
            if (!isBlank(s[i])) return false;
        return true;
    }
    // On a blank with only blanks before it: Vim's inindent(1).
    bool onIndent(int p) const { return !atEnd(p) && isBlank(s[p]) && inIndent(p); }
    int cls(int p, bool big) const
    {
        if (atEnd(p)) return 0;
        const int c = classOf(at(p));
        return big && c ? 1 : c;
    }
    // Vim's inc(): 0 moved on the line, 2 moved onto its end, 1 moved to the
    // next line, -1 at the end of the text.
    int inc(int &p) const
    {
        if (!atEnd(p)) {
            p = next(p);
            return atEnd(p) ? 2 : 0;
        }
        if (p < size()) {
            p += breakAt(p);
            return 1;
        }
        return -1;
    }
    // Vim's dec(): 0 moved on the line, 1 moved to the end of the line
    // before, -1 at the start of the text.
    int dec(int &p) const
    {
        const int start = lineStart(p);
        if (p > start) {
            p = previous(p);
            return 0;
        }
        if (start > 0) {
            p = previousLineEnd(start);
            return 1;
        }
        return -1;
    }
    // inc() and dec() past line ends that have characters before them.
    int incl(int &p) const
    {
        int r = inc(p);
        if (r >= 1 && p != lineStart(p)) r = inc(p);
        return r;
    }
    int decl(int &p) const
    {
        int r = dec(p);
        if (r == 1 && p != lineStart(p)) r = dec(p);
        return r;
    }
    bool oneLeft(int &p) const
    {
        if (p == lineStart(p)) return false;
        p = previous(p);
        return true;
    }
    int lineCount() const
    {
        int lines = 1;
        for (int p = 0; (p = nextLine(p)) >= 0;) ++lines;
        return lines;
    }
    // The start of a line by its number from 0, or of the last line.
    int lineAtNumber(int number) const
    {
        int p = 0;
        for (int n = 0; n < number; ++n) {
            const int after = nextLine(p);
            if (after < 0) break;
            p = after;
        }
        return p;
    }
    int lastLineStart() const { return lineStart(size()); }
    int column(int p) const
    {
        const int start = lineStart(p);
        return MarkdownEditing::displayWidth(s.mid(start, p - start));
    }
    // The character at a display column of the line, or its last when the
    // line is shorter; its end instead when allowEnd. -1 is the end.
    int atColumn(int line, int column, bool allowEnd) const
    {
        const int start = lineStart(line), end = lineEnd(line);
        if (column >= 0) {
            int width = 0;
            for (int p = start; p < end;) {
                const int after = next(p);
                const int w = MarkdownEditing::displayWidth(s.mid(p, after - p));
                if (width + w > column) return p;
                width += w;
                p = after;
            }
        }
        return allowEnd ? end : lastChar(start);
    }
};

// Vim's word motions, as Vim writes them (textobject.c).
bool skipForward(const Text &t, int &p, int cls, bool big)
{
    while (t.cls(p, big) == cls)
        if (t.inc(p) == -1) return true;
    return false;
}

bool skipBackward(const Text &t, int &p, int cls, bool big)
{
    while (t.cls(p, big) == cls)
        if (t.dec(p) == -1) return true;
    return false;
}

bool fwdWord(const Text &t, int &p, int count, bool big, bool eol)
{
    while (--count >= 0) {
        const int sclass = t.cls(p, big);
        const bool lastLine = t.lastLine(p);
        int i = t.inc(p);
        if (i == -1 || (i >= 1 && lastLine)) return false;
        if (i >= 1 && eol && count == 0) return true;
        if (sclass != 0)
            while (t.cls(p, big) == sclass) {
                i = t.inc(p);
                if (i == -1 || (i >= 1 && eol && count == 0)) return true;
            }
        while (t.cls(p, big) == 0) {
            if (p == t.lineStart(p) && t.lineEmpty(p)) break;
            i = t.inc(p);
            if (i == -1 || (i >= 1 && eol && count == 0)) return true;
        }
    }
    return true;
}

bool bckWord(const Text &t, int &p, int count, bool big, bool stop)
{
    while (--count >= 0) {
        const int sclass = t.cls(p, big);
        if (t.dec(p) == -1) return false;
        bool finished = false;
        if (!stop || sclass == t.cls(p, big) || sclass == 0) {
            while (t.cls(p, big) == 0) {
                if (p == t.lineStart(p) && t.lineEmpty(p)) {
                    finished = true;
                    break;
                }
                if (t.dec(p) == -1) return true;
            }
            if (!finished && skipBackward(t, p, t.cls(p, big), big)) return true;
        }
        if (!finished) t.inc(p);
        stop = false;
    }
    return true;
}

bool endWord(const Text &t, int &p, int count, bool big, bool stop, bool empty)
{
    while (--count >= 0) {
        const int sclass = t.cls(p, big);
        if (t.inc(p) == -1) return false;
        bool finished = false;
        if (t.cls(p, big) == sclass && sclass != 0) {
            if (skipForward(t, p, sclass, big)) return false;
        } else if (!stop || sclass == 0) {
            while (t.cls(p, big) == 0) {
                if (p == t.lineStart(p) && t.lineEmpty(p) && empty) {
                    finished = true;
                    break;
                }
                if (t.inc(p) == -1) return false;
            }
            if (!finished && skipForward(t, p, t.cls(p, big), big)) return false;
        }
        if (!finished) t.dec(p);
        stop = false;
    }
    return true;
}

bool bckendWord(const Text &t, int &p, int count, bool big, bool eol)
{
    while (--count >= 0) {
        const int sclass = t.cls(p, big);
        int i = t.dec(p);
        if (i == -1) return false;
        if (eol && i == 1) return true;
        if (sclass != 0)
            while (t.cls(p, big) == sclass)
                if ((i = t.dec(p)) == -1 || (eol && i == 1)) return true;
        while (t.cls(p, big) == 0) {
            if (p == t.lineStart(p) && t.lineEmpty(p)) break;
            if ((i = t.dec(p)) == -1 || (eol && i == 1)) return true;
        }
    }
    return true;
}

void backInLine(const Text &t, int &p, bool big)
{
    const int sclass = t.cls(p, big);
    while (p != t.lineStart(p)) {
        int q = p;
        t.dec(q);
        if (t.cls(q, big) != sclass) break;
        p = q;
    }
}

// iw, aw, iW and aW: Vim's current_word(). `visual` is the other end of a
// Visual area, or -1; on return it is the area's new other end.
bool currentWord(const Text &t, int &p, int &visual, int count, bool include, bool big, int *start, bool *inclusive)
{
    int startPos = p;
    bool includeWhite = false;
    *inclusive = true;
    if (visual < 0 || visual == p) {
        backInLine(t, p, big);
        startPos = p;
        if ((t.cls(p, big) == 0) == include) {
            if (!endWord(t, p, 1, big, true, true)) return false;
        } else {
            fwdWord(t, p, 1, big, true);
            if (p == t.lineStart(p)) t.decl(p);
            else t.oneLeft(p);
            if (include) includeWhite = true;
        }
        if (visual >= 0) visual = startPos;
        --count;
    }
    while (count > 0) {
        *inclusive = true;
        if (visual >= 0 && p < visual) {
            if (t.decl(p) == -1) return false;
            if (include != (t.cls(p, big) != 0)) {
                if (!bckWord(t, p, 1, big, true)) return false;
            } else {
                if (!bckendWord(t, p, 1, big, true)) return false;
                t.incl(p);
            }
        } else {
            if (t.incl(p) == -1) return false;
            if (include != (t.cls(p, big) == 0)) {
                if (!fwdWord(t, p, 1, big, true) && count > 1) return false;
                if (!t.oneLeft(p)) *inclusive = false;
            } else if (!endWord(t, p, 1, big, true, true)) {
                return false;
            }
        }
        --count;
    }
    if (includeWhite && (t.cls(p, big) != 0 || (p == t.lineStart(p) && !*inclusive))) {
        // No blanks after the word: the blanks before it go instead, unless
        // they are the line's indent.
        int q = startPos;
        if (t.oneLeft(q)) {
            backInLine(t, q, big);
            if (t.cls(q, big) == 0 && q > t.lineStart(q)) {
                if (visual >= 0) visual = q;
                else startPos = q;
            }
        }
    }
    *start = visual >= 0 ? visual : startPos;
    return true;
}

int findUnmatchedBackward(const Text &t, int from, QChar open, QChar close)
{
    int depth = 0;
    for (int i = from - 1; i >= 0; --i) {
        if (t.s[i] == close) ++depth;
        else if (t.s[i] == open && depth-- == 0) return i;
    }
    return -1;
}

int findUnmatchedForward(const Text &t, int from, QChar open, QChar close)
{
    int depth = 0;
    for (int i = from + 1; i < t.size(); ++i) {
        if (t.s[i] == open) ++depth;
        else if (t.s[i] == close && depth-- == 0) return i;
    }
    return -1;
}

// i( a( i[ a[ i{ a{: Vim's current_block(). The block holding p, the
// count-th outward, from *start to *end, exclusive; *exclusive says the end
// is exclusive as Vim counts it, so that a block of whole lines becomes
// linewise. With a Visual area from low to high, a block no larger than it
// gives way to the next one out.
bool currentBlock(const Text &t, int p, int low, int high, int count, bool include, QChar open, QChar close, int *start,
                  int *end, bool *exclusive)
{
    const bool visual = low >= 0 && low != high;
    for (int extra = 0; extra < 100; ++extra) {
        int at = visual ? low : p;
        if (!visual) {
            if (open == u'{')
                while (t.onIndent(at))
                    if (t.inc(at) != 0) break;
            if (!t.atEnd(at) && t.s[at] == open) ++at;
        }
        int startPos = -1;
        bool found = findUnmatchedBackward(t, at, open, close) >= 0;
        for (int i = 0; found && i < count + extra; ++i) {
            const int q = findUnmatchedBackward(t, at, open, close);
            if (q < 0) found = false;
            else at = startPos = q;
        }
        if (!found || startPos < 0) return false;
        const int endPos = findUnmatchedForward(t, startPos, open, close);
        if (endPos < 0) return false;
        if (include) {
            *start = startPos;
            *end = endPos + 1;
            *exclusive = false;
        } else {
            int s = startPos;
            t.incl(s);
            int e = endPos;
            bool sol = e == t.lineStart(e);
            t.decl(e);
            while (t.onIndent(e)) {
                sol = true;
                if (t.decl(e) != 0) break;
            }
            if (sol) {
                t.incl(e);
                *start = s;
                *end = e;
                *exclusive = true;
            } else if (s <= e) {
                *start = s;
                *end = t.atEnd(e) ? e : t.next(e);
                *exclusive = false;
            } else {
                *start = *end = s;
                *exclusive = true;
            }
        }
        if (!visual || *start < low || *end > t.next(high) || *start == *end) return true;
    }
    return false;
}

// i" a" i' a' i` a`: Vim's current_quote(), on the line of p.
bool currentQuote(const Text &t, int p, int count, bool include, QChar quote, int *start, int *end)
{
    const int lineStart = t.lineStart(p);
    const QStringView line = t.s.mid(lineStart, t.lineEnd(p) - lineStart);
    const int length = int(line.size());
    const auto findNext = [&](int col, bool escapes) {
        for (; col < length; ++col) {
            if (escapes && line[col] == u'\\') {
                if (++col >= length) return -1;
                continue;
            }
            if (line[col] == quote) return col;
        }
        return -1;
    };
    const auto findPrevious = [&](int col) {
        while (col > 0) {
            --col;
            int escapes = 0;
            while (col - escapes > 0 && line[col - escapes - 1] == u'\\') ++escapes;
            if (escapes & 1) col -= escapes;
            else if (line[col] == quote) break;
        }
        return col;
    };
    const int col = p - lineStart;
    int colStart = 0, colEnd = 0;
    if (col < length && line[col] == quote) {
        // On a quote: which pair it belongs to is counted from the line's start.
        for (;;) {
            colStart = findNext(colStart, false);
            if (colStart < 0 || colStart > col) return false;
            colEnd = findNext(colStart + 1, true);
            if (colEnd < 0) return false;
            if (colStart <= col && col <= colEnd) break;
            colStart = colEnd + 1;
        }
    } else {
        colStart = findPrevious(col);
        if (colStart >= length || line[colStart] != quote) {
            colStart = findNext(colStart, false);
            if (colStart < 0) return false;
        }
        colEnd = findNext(colStart + 1, true);
        if (colEnd < 0) return false;
    }
    if (include) {
        if (colEnd + 1 < length && isBlank(line[colEnd + 1])) {
            while (colEnd + 1 < length && isBlank(line[colEnd + 1])) ++colEnd;
        } else {
            while (colStart > 0 && isBlank(line[colStart - 1])) --colStart;
        }
    }
    const bool quotes = include || count > 1;
    *start = lineStart + (quotes ? colStart : colStart + 1);
    *end = lineStart + (quotes ? colEnd + 1 : colEnd);
    return true;
}

// The start of every line, for the commands that count lines.
struct Lines {
    QList<int> starts;
    explicit Lines(const Text &t)
    {
        starts.append(0);
        for (int p = 0; (p = t.nextLine(p)) >= 0;) starts.append(p);
    }
    int last() const { return int(starts.size()) - 1; }
    int lineOf(int position) const
    {
        return int(std::upper_bound(starts.cbegin(), starts.cend(), position) - starts.cbegin()) - 1;
    }
};

// ip and ap: Vim's current_par(), by line numbers. In Visual mode an area
// over several lines is extended instead, by moving its cursor's line.
bool currentParagraph(const Text &t, int cursor, int visual, bool visualLines, int count, bool include, int *first,
                      int *last, bool *extended)
{
    const Lines lines(t);
    const int final = lines.last();
    const auto white = [&](int line) { return t.lineWhite(lines.starts[line]); };
    int start = lines.lineOf(cursor);
    const auto extend = [&] {
        const int dir = start < lines.lineOf(visual) ? -1 : 1;
        bool ok = true;
        for (int i = count; --i >= 0;) {
            if (start == (dir < 0 ? 0 : final)) {
                ok = false;
                break;
            }
            int previousWhite = -1;
            for (int k = 0; k < 2; ++k) {
                start += dir;
                const int isWhite = white(start);
                if (previousWhite == isWhite) {
                    start -= dir;
                    break;
                }
                while (start != (dir < 0 ? 0 : final) && isWhite == white(start + dir)) start += dir;
                if (!include || start == (dir < 0 ? 0 : final)) break;
                previousWhite = isWhite;
            }
        }
        *first = start;
        *extended = true;
        return ok;
    };
    *extended = false;
    if (visual >= 0 && lines.lineOf(visual) != start) return extend();
    const bool whiteInFront = white(start);
    while (start > 0) {
        if (whiteInFront ? !white(start - 1) : white(start - 1)) break;
        --start;
    }
    int end = start;
    while (end <= final && white(end)) ++end;
    --end;
    int i = count;
    if (!include && whiteInFront) --i;
    while (i--) {
        if (end == final) return false;
        const bool doWhite = !include && white(end + 1);
        if (include || !doWhite) {
            ++end;
            while (end < final && !white(end + 1)) ++end;
        }
        if (i == 0 && whiteInFront && include) break;
        if (include || doWhite)
            while (end < final && white(end + 1)) ++end;
    }
    if (!whiteInFront && !white(end) && include)
        while (start > 0 && white(start - 1)) --start;
    if (visual >= 0 && visualLines && start == lines.lineOf(cursor)) return extend();
    *first = lines.starts[start];
    *last = lines.starts[end];
    return true;
}

// { and }: Vim's findpar(), where an empty line ends a paragraph.
bool paragraphMotion(const Text &t, int from, int count, bool forward, int *target, bool *inclusive)
{
    int current = t.lineStart(from);
    *inclusive = false;
    while (count--) {
        bool skipped = false;
        for (bool first = true;; first = false) {
            if (!t.lineEmpty(current)) skipped = true;
            if (!first && skipped && t.lineEmpty(current)) break;
            const int next = forward ? t.nextLine(current) : t.previousLine(current);
            if (next < 0) {
                if (count) return false;
                break;
            }
            current = next;
        }
    }
    *target = current;
    if (forward && t.lastLine(current) && !t.lineEmpty(current)) {
        *target = t.lastChar(current);
        *inclusive = true;
    }
    return true;
}

// %: the bracket at or after p on its line, to the one that matches it.
int matchBracket(const Text &t, int p)
{
    static const QString brackets = QStringLiteral("()[]{}");
    const int end = t.lineEnd(p);
    while (p < end && !brackets.contains(t.s[p])) ++p;
    if (p >= end) return -1;
    const qsizetype kind = brackets.indexOf(t.s[p]);
    const QChar open = brackets[kind & ~1], close = brackets[kind | 1];
    return kind & 1 ? findUnmatchedBackward(t, p, open, close) : findUnmatchedForward(t, p, open, close);
}

struct Register {
    QString text;
    bool linewise = false;
};

struct CharSearch {
    QString target;
    QChar command;
};

struct Search {
    QString pattern;
    bool forward = true;
    // Whole words only, as * and # look for them.
    bool whole = false;
    bool caseSensitive = true;
};

// What every view's Vim keys share, as one Vim would.
struct Shared {
    QHash<QChar, Register> registers;
    CharSearch charSearch;
    Search search;
};

Shared &shared()
{
    static Shared state;
    return state;
}

// Registers hold their line breaks as LF; puts write the document's own.
QString withLf(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', u'\n');
    text.replace(QChar(0x2029), u'\n');
    return text;
}

QString withNewline(QString text, const QString &newline)
{
    if (newline != u"\n") text.replace(u'\n', newline);
    return text;
}

bool isRegisterName(QChar name)
{
    return (name >= u'a' && name <= u'z') || (name >= u'A' && name <= u'Z') || name == u'0' || name == u'"' || name == u'_'
        || name == u'+' || name == u'*';
}

Register readRegister(QChar name)
{
    if (name == u'+' || name == u'*') {
        const QString text = withLf(QGuiApplication::clipboard()->text());
        return {text, text.endsWith(u'\n')};
    }
    if (name == u'_') return {};
    return shared().registers.value(name.isNull() ? QChar(u'"') : name.toLower());
}

// Yanks go to "0 as well; a capital name appends; "_ keeps nothing, and
// "+ and "* are the clipboard. The unnamed register "" has the last of them.
void writeRegister(QChar name, Register value, bool yank)
{
    Shared &state = shared();
    if (name == u'_') return;
    value.text = withLf(value.text);
    if (name == u'+' || name == u'*') {
        QGuiApplication::clipboard()->setText(value.text);
    } else if (name >= u'A' && name <= u'Z') {
        Register &kept = state.registers[name.toLower()];
        if (kept.linewise || value.linewise) {
            QString base = kept.text;
            if (!base.isEmpty() && !base.endsWith(u'\n')) base += u'\n';
            QString added = value.text;
            if (!added.endsWith(u'\n')) added += u'\n';
            kept = {base + added, true};
        } else {
            kept.text += value.text;
        }
        value = kept;
    } else if (name >= u'a' && name <= u'z') {
        state.registers[name] = value;
    } else if (yank) {
        state.registers[u'0'] = value;
    }
    state.registers[u'"'] = value;
}

// The display of a key in what is pending.
QString shown(const QString &key)
{
    if (key == u"<CR>") return QStringLiteral("⏎");
    return key;
}

bool isNamedKey(const QString &key) { return key.size() > 2 && key.startsWith(u'<') && key.endsWith(u'>'); }

// Moves the view's cursor without that counting as a move from outside.
struct Moving {
    int &depth;
    explicit Moving(int &depth) : depth(depth) { ++depth; }
    ~Moving() { --depth; }
};

} // namespace

struct VimMode::Motion {
    enum Kind { Exclusive, Inclusive, Linewise };
    bool ok = true;
    int target = 0;
    Kind kind = Exclusive;
    // j and k keep the column they started for.
    bool keepWant = false;
    // $ sticks to the ends of lines.
    bool toEnd = false;
    // A motion back over a line break that an operator takes as it is.
    bool noAdjust = false;
};

// What an operator acts on: from start to end, exclusive; linewise, from
// the start of the first line to the end of the last, its break left out.
struct VimMode::Region {
    int start = 0;
    int end = 0;
    bool linewise = false;
};

VimMode::VimMode(VimHost *host, QObject *parent) : QObject(parent), m_host(host) {}

// The host may be gone by now, so nothing here reaches it.
VimMode::~VimMode() = default;

void VimMode::setActive(bool active)
{
    if (active == m_active) return;
    if (!active && m_mode == Insert && m_insertBuffer) m_insertBuffer->joinSince(m_insertMark);
    m_insertBuffer = nullptr;
    m_active = active;
    cancel();
    m_commandLine.clear();
    m_message.clear();
    const Mode before = m_mode;
    m_mode = Normal;
    m_host->setInputMethodEnabled(!active);
    if (active && m_host->document()) {
        const int c = m_host->cursor();
        if (c != m_host->anchor()) startVisual(Visual);
        else setCursor(clamped(c));
    }
    emit activeChanged();
    if (before != m_mode || active) emit modeChanged();
    emit stateChanged();
}

QString VimMode::modeName() const
{
    if (!m_active) return {};
    switch (m_mode) {
    case Normal:
        return tr("NORMAL");
    case Insert:
        return tr("INSERT");
    case Visual:
        return tr("VISUAL");
    case VisualLine:
        return tr("VISUAL LINE");
    }
    return {};
}

int VimMode::cursorEnd() const
{
    const QString source = text();
    const Text t{source};
    const int c = std::min(m_cursor, t.size());
    return t.atEnd(c) ? c : t.next(c);
}

QString VimMode::text() const
{
    Document *document = m_host->document();
    return document ? document->buffer()->text() : QString();
}

SourceBuffer *VimMode::buffer() const
{
    Document *document = m_host->document();
    return document ? document->buffer() : nullptr;
}

QString VimMode::newline() const
{
    Document *document = m_host->document();
    return document ? document->newline() : QStringLiteral("\n");
}

int VimMode::count() const { return std::min(maxCount, std::max(1, m_count1) * std::max(1, m_count2)); }

void VimMode::showTyped(const QString &k)
{
    m_typed += shown(k);
    emit stateChanged();
}

void VimMode::setMessage(const QString &message)
{
    m_message = message;
    emit stateChanged();
}

void VimMode::setMode(Mode mode)
{
    if (mode == m_mode) return;
    m_mode = mode;
    m_host->setInputMethodEnabled(mode == Insert);
    emit modeChanged();
}

int VimMode::clamped(int position) const
{
    const QString source = text();
    const Text t{source};
    const int p = std::clamp(position, 0, t.size());
    return t.atEnd(p) && p != t.lineStart(p) ? t.previous(p) : p;
}

void VimMode::setCursor(int position, bool updateWant)
{
    const QString source = text();
    const Text t{source};
    m_cursor = std::clamp(position, 0, t.size());
    if (updateWant) m_want = t.column(m_cursor);
    if (m_mode == Visual || m_mode == VisualLine) {
        showVisual();
    } else {
        const Moving moving(m_moving);
        m_host->select(m_cursor, m_cursor);
    }
    emit cursorChanged();
}

void VimMode::startVisual(Mode mode)
{
    const int c = m_host->cursor(), a = m_host->anchor();
    setMode(mode);
    if (c != a) {
        // A selection made otherwise: its cursor is on the last character it holds.
        const QString source = text();
        const Text t{source};
        m_visualAnchor = a;
        m_cursor = c > a ? t.previous(c) : c;
    } else {
        m_visualAnchor = m_cursor;
    }
    showVisual();
    emit cursorChanged();
}

VimMode::Region VimMode::visualRegion(bool lines) const
{
    const QString source = text();
    const Text t{source};
    const int a = std::min(m_visualAnchor, m_cursor), b = std::max(m_visualAnchor, m_cursor);
    if (lines) return {t.lineStart(a), t.lineEnd(b), true};
    // The area holds the character at its end, or the line break there.
    return {a, t.atEnd(b) ? b + t.breakAt(b) : t.next(b), false};
}

void VimMode::showVisual()
{
    const QString source = text();
    const Text t{source};
    m_visualAnchor = std::clamp(m_visualAnchor, 0, t.size());
    m_cursor = std::clamp(m_cursor, 0, t.size());
    const Moving moving(m_moving);
    if (m_mode == VisualLine) {
        if (m_cursor >= m_visualAnchor) m_host->select(t.lineStart(m_visualAnchor), t.lineEnd(m_cursor));
        else m_host->select(t.lineEnd(m_visualAnchor), t.lineStart(m_cursor));
        return;
    }
    const auto after = [&](int p) { return t.atEnd(p) ? p + t.breakAt(p) : t.next(p); };
    if (m_cursor >= m_visualAnchor) m_host->select(m_visualAnchor, after(m_cursor));
    else m_host->select(after(m_visualAnchor), m_cursor);
}

void VimMode::leaveVisual()
{
    setMode(Normal);
    setCursor(clamped(m_cursor));
}

void VimMode::reset()
{
    if (m_mode == Insert && m_insertBuffer) m_insertBuffer->joinSince(m_insertMark);
    m_insertBuffer = nullptr;
    m_recordingInsert = false;
    cancel();
    m_commandLine.clear();
    m_message.clear();
    m_cursor = 0;
    m_want = 0;
    if (m_mode != Normal) {
        m_mode = Normal;
        emit modeChanged();
    }
    if (m_active) m_host->setInputMethodEnabled(false);
    emit stateChanged();
    emit cursorChanged();
}

void VimMode::setBlocked(bool blocked)
{
    if (m_mode != Insert || !m_insertBuffer) return;
    if (blocked) m_insertBuffer->joinSince(m_insertMark);
    else m_insertMark = m_insertBuffer->undoDepth();
}

void VimMode::selectionMoved()
{
    if (!m_active || m_moving > 0 || !m_host->document()) return;
    const int c = m_host->cursor(), a = m_host->anchor();
    if (m_mode == Insert || !m_commandLine.isEmpty()) {
        m_cursor = c;
        emit cursorChanged();
        return;
    }
    // The mouse, or keys Vim keys let through, moved the cursor: a command
    // half typed is dropped, and a selection made so is Visual mode's.
    cancel();
    if (a != c) {
        startVisual(Visual);
        return;
    }
    if (m_mode != Normal) setMode(Normal);
    m_cursor = c;
    const QString source = text();
    m_want = Text{source}.column(c);
    emit cursorChanged();
}

QString VimMode::tokenOf(const QKeyEvent *event) const
{
    const Qt::KeyboardModifiers modifiers =
        event->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier);
    const int key = event->key();
    const QString text = event->text();
    if (key == Qt::Key_Escape || text == QStringLiteral("\x1b")
        || (key == Qt::Key_BracketLeft && modifiers == Qt::ControlModifier))
        return QStringLiteral("<Esc>");
    if ((modifiers & (Qt::ControlModifier | Qt::MetaModifier)) && !(modifiers & Qt::AltModifier)) {
        if (key == Qt::Key_R && modifiers == Qt::ControlModifier) return QStringLiteral("<C-r>");
        if (!m_commandLine.isEmpty()) {
            if (key == Qt::Key_C && modifiers == Qt::ControlModifier) return QStringLiteral("<C-c>");
            if (key == Qt::Key_V && (modifiers & ~Qt::ShiftModifier) == Qt::ControlModifier) return QStringLiteral("<C-v>");
        }
        return {};
    }
    switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        return QStringLiteral("<CR>");
    case Qt::Key_Backspace:
        return QStringLiteral("<BS>");
    case Qt::Key_Delete:
        return QStringLiteral("<Del>");
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        return QStringLiteral("<Tab>");
    case Qt::Key_Insert:
        return QStringLiteral("<Insert>");
    case Qt::Key_Left:
    case Qt::Key_Right:
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_Home:
    case Qt::Key_End: {
        // With Shift the view selects, and the selection is then Visual mode's.
        if (modifiers & Qt::ShiftModifier) return {};
        static const QHash<int, QString> names{
            {Qt::Key_Left, QStringLiteral("<Left>")}, {Qt::Key_Right, QStringLiteral("<Right>")},
            {Qt::Key_Up, QStringLiteral("<Up>")},     {Qt::Key_Down, QStringLiteral("<Down>")},
            {Qt::Key_Home, QStringLiteral("<Home>")}, {Qt::Key_End, QStringLiteral("<End>")}};
        return names.value(key);
    }
    default:
        break;
    }
    if (text.isEmpty()) return {};
    const QChar first = text.at(0);
    if (!(first.isPrint() || first.isSurrogate() || first.category() == QChar::Other_PrivateUse)) return {};
    // Alt with a letter types nothing outside Insert mode.
    if (modifiers & Qt::AltModifier) return QStringLiteral("<ignore>");
    return text;
}

bool VimMode::handleKey(QKeyEvent *event)
{
    if (!m_active || !m_host->document()) return false;
    const QString token = tokenOf(event);
    if (m_mode == Insert) {
        if (token == u"<Esc>") {
            // Escape first ends what an input method is composing.
            if (m_host->composing()) return false;
            m_skipJoin = false;
            leaveInsert();
            return true;
        }
        if (!m_replaying) recordInsert({event->key(), event->modifiers(), event->text(), false});
        return false;
    }
    if (!m_commandLine.isEmpty()) {
        // The command line takes every key the window leaves to the text.
        if (!token.isEmpty() && token != u"<ignore>") run(token);
        return true;
    }
    if (token.isEmpty()) {
        // Keys Vim keys leave to the view, such as Ctrl+C or Page Down,
        // act as without them; a command half typed is dropped.
        if (!m_typed.isEmpty() || !m_operator.isNull() || hasCount() || m_await != Await::None) cancel();
        return false;
    }
    if (token == u"<ignore>") return true;
    return run(token);
}

bool VimMode::handleInputMethod(QInputMethodEvent *event)
{
    if (!m_active || !m_host->document()) return false;
    if (m_mode == Insert) {
        if (!m_replaying && !event->commitString().isEmpty()) recordInsert({0, {}, event->commitString(), true});
        return false;
    }
    // Outside Insert mode nothing is composed; committed text is keys.
    event->accept();
    const QString commit = event->commitString();
    if (!m_commandLine.isEmpty()) {
        if (!commit.isEmpty()) run(commit);
        return true;
    }
    for (qsizetype i = 0; i < commit.size(); ++i) {
        const qsizetype length = commit[i].isHighSurrogate() && i + 1 < commit.size() ? 2 : 1;
        run(commit.mid(i, length));
        i += length - 1;
        // A command that began Insert mode leaves the rest to be typed.
        if (m_mode == Insert) {
            const QString rest = commit.mid(i + 1);
            if (!rest.isEmpty()) m_host->commit(rest);
            break;
        }
    }
    return true;
}

void VimMode::recordInsert(const Key &key)
{
    if (m_insertOverflow) return;
    if (m_insertKeys.size() >= maxInsertKeys) {
        m_insertOverflow = true;
        m_insertKeys.clear();
        return;
    }
    m_insertKeys.append(key);
}

bool VimMode::run(const QString &token)
{
    SourceBuffer *b = buffer();
    const QPointer<SourceBuffer> kept(b);
    m_keyMark = b ? b->undoDepth() : 0;
    m_skipJoin = false;
    if (!m_message.isEmpty()) {
        m_message.clear();
        emit stateChanged();
    }
    // The mouse may have left the cursor at the end of a line.
    if (m_mode == Normal && m_commandLine.isEmpty()) {
        const int c = clamped(m_host->cursor());
        if (c != m_host->cursor() || c != m_cursor) setCursor(c, false);
    }
    const bool taken = press(token);
    // A command is one undo step, a stay in Insert mode with it too.
    if (kept && m_mode != Insert && !m_skipJoin) kept->joinSince(m_keyMark);
    return taken;
}

bool VimMode::press(const QString &k)
{
    if (!m_commandLine.isEmpty()) {
        commandLineKey(k);
        return true;
    }
    if (k == u"<Esc>") return escape();
    if (m_await != Await::None) {
        awaited(k);
        return true;
    }
    const bool digit = k.size() == 1 && k[0] >= u'0' && k[0] <= u'9';
    int &counted = m_operator.isNull() ? m_count1 : m_count2;
    if (digit && (k[0] != u'0' || counted > 0)) {
        counted = std::min(maxCount, counted * 10 + int(k[0].unicode() - u'0'));
        showTyped(k);
        return true;
    }
    if (k == u"\"" && m_operator.isNull()) {
        m_await = Await::Register;
        showTyped(k);
        return true;
    }
    m_keys.append(k);
    if (m_mode == Visual || m_mode == VisualLine) visualKey(k);
    else normalKey(k);
    return true;
}

bool VimMode::escape()
{
    const bool pending = !m_typed.isEmpty() || !m_operator.isNull() || hasCount() || m_await != Await::None
        || !m_register.isNull();
    cancel();
    if (m_mode == Visual || m_mode == VisualLine) {
        leaveVisual();
        return true;
    }
    return pending;
}

void VimMode::cancel()
{
    m_count1 = m_count2 = 0;
    m_register = QChar();
    m_operator = QChar();
    m_await = Await::None;
    m_awaitFor.clear();
    m_keys.clear();
    m_typed.clear();
    m_visualSize = {};
    emit stateChanged();
}

void VimMode::done(bool change)
{
    if (change && !m_replaying) {
        Change last = m_visualSize;
        last.keys = m_keys;
        last.count = hasCount() ? count() : 0;
        last.reg = m_register;
        m_last = last;
        m_recordingInsert = m_mode == Insert;
    }
    cancel();
}

void VimMode::request(const QString &command)
{
    // After the key that asked: the command may close this view.
    QMetaObject::invokeMethod(this, [this, command] { emit commandRequested(command); }, Qt::QueuedConnection);
}

bool VimMode::motion(const QString &k, const QString &argument, Use use, Motion *out)
{
    static const QSet<QString> motions{
        QStringLiteral("h"),   QStringLiteral("<Left>"),  QStringLiteral("<BS>"),  QStringLiteral("l"),
        QStringLiteral("<Right>"), QStringLiteral(" "),   QStringLiteral("j"),     QStringLiteral("<Down>"),
        QStringLiteral("k"),   QStringLiteral("<Up>"),    QStringLiteral("<CR>"),  QStringLiteral("0"),
        QStringLiteral("<Home>"), QStringLiteral("^"),    QStringLiteral("$"),     QStringLiteral("<End>"),
        QStringLiteral("w"),   QStringLiteral("W"),       QStringLiteral("b"),     QStringLiteral("B"),
        QStringLiteral("e"),   QStringLiteral("E"),       QStringLiteral("gg"),    QStringLiteral("G"),
        QStringLiteral("{"),   QStringLiteral("}"),       QStringLiteral("%"),     QStringLiteral("f"),
        QStringLiteral("t"),   QStringLiteral("F"),       QStringLiteral("T"),     QStringLiteral(";"),
        QStringLiteral(","),   QStringLiteral("n"),       QStringLiteral("N"),     QStringLiteral("*"),
        QStringLiteral("#")};
    if (!motions.contains(k)) return false;
    // f, t, F and T wait for their character first.
    if ((k == u"f" || k == u"t" || k == u"F" || k == u"T") && argument.isEmpty()) return false;
    const QString source = text();
    const Text t{source};
    const int from = std::clamp(m_cursor, 0, t.size());
    const int n = count();
    const bool op = use == Use::Operator, visual = use == Use::Visual;
    Motion m;
    m.target = from;
    // Vim's adjust_cursor(): a word motion that ends past a line's last
    // character ends on it, and an operator then takes that character.
    const auto adjust = [&](int &p) {
        if (p > from && t.atEnd(p) && p != t.lineStart(p) && !visual) {
            p = t.previous(p);
            if (op) m.kind = Motion::Inclusive;
        }
    };
    if (k == u"h" || k == u"<Left>" || k == u"<BS>") {
        int p = from, moved = 0;
        for (int i = 0; i < n; ++i) {
            if (p > t.lineStart(p)) {
                p = t.previous(p);
            } else if (k == u"<BS>" && p > 0) {
                // Backspace goes on to the line before, as with whichwrap=b.
                p = t.previousLineEnd(t.lineStart(p));
                if (op) m.noAdjust = true;
                else p = t.lastChar(p);
            } else {
                break;
            }
            ++moved;
        }
        m.target = p;
        m.ok = moved > 0;
    } else if (k == u"l" || k == u"<Right>" || k == u" ") {
        int p = from;
        bool moved = false;
        for (int i = 0; i < n; ++i) {
            const bool onEnd = t.atEnd(p);
            if (onEnd || (!visual && t.atEnd(t.next(p)))) {
                if (k == u" " && !t.lastLine(p)) {
                    // Space goes on to the next line, as with whichwrap=s.
                    if (op && m.kind == Motion::Exclusive && !t.lineEmpty(p)) {
                        m.kind = Motion::Inclusive;
                    } else {
                        p = t.nextLine(p);
                        m.kind = Motion::Exclusive;
                    }
                    moved = true;
                    continue;
                }
                if (op && !t.lineEmpty(p)) {
                    m.kind = Motion::Inclusive;
                    moved = true;
                }
                break;
            }
            p = t.next(p);
            moved = true;
        }
        m.target = p;
        m.ok = moved;
    } else if (k == u"j" || k == u"<Down>" || k == u"k" || k == u"<Up>" || k == u"<CR>") {
        const bool down = k != u"k" && k != u"<Up>";
        if (down ? t.lastLine(from) : t.lineStart(from) == 0) {
            m.ok = false;
        } else {
            int line = t.lineStart(from);
            for (int i = 0; i < n; ++i) {
                const int next = down ? t.nextLine(line) : t.previousLine(line);
                if (next < 0) break;
                line = next;
            }
            if (k == u"<CR>") {
                m.target = t.firstNonBlank(line);
            } else {
                m.target = t.atColumn(line, m_want, visual);
                m.keepWant = true;
            }
            m.kind = Motion::Linewise;
        }
    } else if (k == u"0" || k == u"<Home>") {
        m.target = t.lineStart(from);
    } else if (k == u"^") {
        m.target = t.firstNonBlank(from);
    } else if (k == u"$" || k == u"<End>") {
        int line = t.lineStart(from);
        for (int i = 1; i < n && m.ok; ++i) {
            const int next = t.nextLine(line);
            if (next < 0) m.ok = false;
            else line = next;
        }
        m.target = visual ? t.lineEnd(line) : t.lastChar(line);
        m.kind = Motion::Inclusive;
        m.toEnd = true;
    } else if (k == u"w" || k == u"W") {
        int p = from;
        fwdWord(t, p, n, k == u"W", op);
        adjust(p);
        m.target = p;
        // An operator acts even where the motion stops at the end of the text.
        m.ok = p != from || op;
    } else if (k == u"b" || k == u"B") {
        int p = from;
        bckWord(t, p, n, k == u"B", false);
        m.target = p;
        m.ok = p != from || op;
    } else if (k == u"e" || k == u"E") {
        int p = from;
        endWord(t, p, n, k == u"E", false, false);
        m.kind = Motion::Inclusive;
        adjust(p);
        m.target = p;
        m.ok = p != from || op;
    } else if (k == u"gg" || k == u"G") {
        const int line = hasCount() ? t.lineAtNumber(n - 1) : k == u"gg" ? 0 : t.lastLineStart();
        m.target = t.firstNonBlank(line);
        m.kind = Motion::Linewise;
    } else if (k == u"{" || k == u"}") {
        bool inclusive = false;
        m.ok = paragraphMotion(t, from, n, k == u"}", &m.target, &inclusive);
        if (inclusive) m.kind = Motion::Inclusive;
    } else if (k == u"%") {
        if (hasCount()) {
            m.ok = n <= 100;
            m.target = t.firstNonBlank(t.lineAtNumber((n * t.lineCount() + 99) / 100 - 1));
            m.kind = Motion::Linewise;
        } else {
            m.target = matchBracket(t, from);
            m.ok = m.target >= 0;
            m.kind = Motion::Inclusive;
        }
    } else if (k == u"f" || k == u"t" || k == u"F" || k == u"T" || k == u";" || k == u",") {
        CharSearch search;
        const bool again = k == u";" || k == u",";
        if (again) {
            search = shared().charSearch;
            if (k == u"," && !search.command.isNull()) {
                static const QHash<QChar, QChar> reversed{{u'f', u'F'}, {u'F', u'f'}, {u't', u'T'}, {u'T', u't'}};
                search.command = reversed.value(search.command);
            }
        } else {
            search = {argument, k[0]};
            shared().charSearch = search;
        }
        const QChar command = search.command;
        const bool forward = command == u'f' || command == u't';
        const bool till = command == u't' || command == u'T';
        int p = from;
        // Repeated, a t or T that would not move looks one further.
        bool stop = !(again && till && n == 1);
        m.ok = !search.target.isEmpty();
        for (int i = 0; i < n && m.ok; ++i) {
            for (;;) {
                if (forward) {
                    if (t.atEnd(p) || t.atEnd(t.next(p))) {
                        m.ok = false;
                        break;
                    }
                    p = t.next(p);
                } else {
                    if (p <= t.lineStart(from)) {
                        m.ok = false;
                        break;
                    }
                    p = t.previous(p);
                }
                if (stop && QStringView(source).mid(p, t.next(p) - p) == search.target) break;
                stop = true;
            }
        }
        if (m.ok && till) p = forward ? t.previous(p) : t.next(p);
        m.target = m.ok ? p : from;
        m.kind = forward ? Motion::Inclusive : Motion::Exclusive;
    } else if (k == u"n" || k == u"N" || k == u"*" || k == u"#") {
        int start = from;
        bool forward = true;
        if (k == u"*" || k == u"#") {
            // The word under the cursor, or the next on its line; the search
            // starts at its start, so that it finds another.
            const int end = t.lineEnd(from);
            int p = from;
            while (p < end && !isKeyword(t.at(p))) p = t.next(p);
            if (p >= end) {
                setMessage(tr("No word under the cursor"));
                m.ok = false;
            } else {
                start = p;
                while (start > t.lineStart(start) && isKeyword(t.before(start))) start = t.previous(start);
                int after = p;
                while (after < end && isKeyword(t.at(after))) after = t.next(after);
                forward = k == u"*";
                shared().search = {source.mid(start, after - start), forward, true, true};
            }
        } else {
            const Search &last = shared().search;
            if (last.pattern.isEmpty()) {
                setMessage(tr("No previous search"));
                m.ok = false;
            }
            forward = k == u"n" ? last.forward : !last.forward;
        }
        int found = -1;
        if (m.ok) m.ok = search(forward, n, start, &found);
        m.target = m.ok ? found : from;
    }
    *out = m;
    return true;
}

bool VimMode::search(bool forward, int count, int from, int *found)
{
    const Search &s = shared().search;
    const QString source = text();
    const Text t{source};
    const QStringView view(source);
    const Qt::CaseSensitivity sensitivity = s.caseSensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    const int length = int(s.pattern.size());
    const auto fits = [&](qsizetype at) {
        const int end = int(at) + length;
        if (!sourceGraphemeBoundary(view, int(at)) || !sourceGraphemeBoundary(view, end)) return false;
        return !s.whole || (!isKeyword(t.before(int(at))) && !isKeyword(t.at(end)));
    };
    bool wrapped = false;
    int position = from;
    for (int i = 0; i < count; ++i) {
        qsizetype at = -1;
        if (forward) {
            for (qsizetype c = view.indexOf(s.pattern, position + 1, sensitivity); c >= 0;
                 c = view.indexOf(s.pattern, c + 1, sensitivity))
                if (fits(c)) {
                    at = c;
                    break;
                }
            if (at < 0) {
                for (qsizetype c = view.indexOf(s.pattern, 0, sensitivity); c >= 0 && c <= position;
                     c = view.indexOf(s.pattern, c + 1, sensitivity))
                    if (fits(c)) {
                        at = c;
                        wrapped = true;
                        break;
                    }
            }
        } else {
            for (qsizetype c = position > 0 ? view.lastIndexOf(s.pattern, position - 1, sensitivity) : -1; c >= 0;
                 c = c > 0 ? view.lastIndexOf(s.pattern, c - 1, sensitivity) : -1)
                if (fits(c)) {
                    at = c;
                    break;
                }
            if (at < 0 && !source.isEmpty()) {
                for (qsizetype c = view.lastIndexOf(s.pattern, view.size() - 1, sensitivity); c >= position;
                     c = c > 0 ? view.lastIndexOf(s.pattern, c - 1, sensitivity) : -1)
                    if (fits(c)) {
                        at = c;
                        wrapped = true;
                        break;
                    }
            }
        }
        if (at < 0) {
            setMessage(tr("Pattern not found: %1").arg(s.pattern));
            return false;
        }
        position = int(at);
    }
    if (wrapped)
        setMessage(forward ? tr("Search hit the bottom, went on at the top")
                           : tr("Search hit the top, went on at the bottom"));
    *found = position;
    return true;
}

void VimMode::moveTo(const Motion &motion)
{
    const QString source = text();
    const Text t{source};
    const int c = clamped(motion.target);
    setCursor(c, false);
    if (motion.toEnd) m_want = -1;
    else if (!motion.keepWant) m_want = t.column(c);
}

void VimMode::visualMove(const Motion &motion)
{
    const QString source = text();
    const Text t{source};
    m_cursor = std::clamp(motion.target, 0, t.size());
    if (motion.toEnd) m_want = -1;
    else if (!motion.keepWant) m_want = t.column(m_cursor);
    showVisual();
    emit cursorChanged();
}

VimMode::Region VimMode::regionFor(int from, const Motion &motion, bool forDelete) const
{
    const QString source = text();
    const Text t{source};
    int a = std::min(from, motion.target), b = std::max(from, motion.target);
    if (motion.kind == Motion::Linewise) return {t.lineStart(a), t.lineEnd(b), true};
    if (motion.kind == Motion::Exclusive && !motion.noAdjust && b > a && b == t.lineStart(b)
        && t.lineStart(a) != t.lineStart(b)) {
        // An exclusive motion to the start of a later line stops at the end
        // of the line before; from the indent on, it takes whole lines.
        const int before = t.previousLineEnd(b);
        if (t.inIndent(a)) return {t.lineStart(a), before, true};
        b = before;
    } else if (motion.kind == Motion::Inclusive && !t.atEnd(b)) {
        b = t.next(b);
    }
    if (forDelete && t.lineStart(a) != t.lineStart(b) && t.inIndent(a)) {
        // Vi's rule: a delete over lines with only blanks before its start
        // and after its end takes whole lines.
        bool blankAfter = true;
        for (int i = b, end = t.lineEnd(b); i < end && blankAfter; ++i) blankAfter = isBlank(source[i]);
        if (blankAfter) return {t.lineStart(a), t.lineEnd(b), true};
    }
    return {a, b, false};
}

bool VimMode::edit(int start, int end, const QString &replacement, int cursor)
{
    const Moving moving(m_moving);
    return m_host->apply(TextChange{{SourceEdit{{start, end - start}, replacement}}, cursor, cursor});
}

void VimMode::deleteRegion(const Region &region, bool keep)
{
    const QString source = text();
    const Text t{source};
    int start = region.start, end = region.end;
    if (region.linewise) {
        if (keep) writeRegister(m_register, {source.mid(region.start, region.end - region.start) + u'\n', true}, false);
        // The lines go with their breaks; the last line of the text takes
        // the break before it instead.
        if (end < t.size()) end += t.breakAt(end);
        else if (start > 0) start = t.previousLineEnd(start);
        if (!edit(start, end, {}, start)) return;
        const QString after = text();
        const Text now{after};
        setCursor(clamped(now.firstNonBlank(std::min(start, now.size()))));
        return;
    }
    if (keep) writeRegister(m_register, {source.mid(start, end - start), false}, false);
    if (!edit(start, end, {}, start)) return;
    setCursor(clamped(start));
}

void VimMode::yankRegion(const Region &region, int cursor)
{
    const QString source = text();
    QString yanked = source.mid(region.start, region.end - region.start);
    if (region.linewise) yanked += u'\n';
    writeRegister(m_register, {yanked, region.linewise}, true);
    setCursor(clamped(cursor));
}

void VimMode::changeRegion(const Region &region)
{
    const QString source = text();
    QString removed = source.mid(region.start, region.end - region.start);
    if (region.linewise) removed += u'\n';
    if (!removed.isEmpty()) writeRegister(m_register, {removed, region.linewise}, false);
    // Lines changed leave one empty line to write in.
    if (!edit(region.start, region.end, {}, region.start)) return;
    enterInsert(region.start);
}

void VimMode::operate(QChar op, const Region &region, int cursorAfterYank)
{
    switch (op.unicode()) {
    case u'd':
        deleteRegion(region);
        break;
    case u'y':
        yankRegion(region, cursorAfterYank);
        break;
    case u'c':
        changeRegion(region);
        break;
    case u'>':
    case u'<':
        shiftLines(region.start, region.end, 1, op == u'<');
        break;
    default:
        break;
    }
}

void VimMode::finishOperator(const Motion &motion)
{
    const QChar op = m_operator;
    if (!motion.ok) {
        cancel();
        return;
    }
    if (op != u'y' && !m_host->editable()) {
        setMessage(tr("This text cannot be edited"));
        cancel();
        return;
    }
    const Region region = regionFor(m_cursor, motion, op == u'd');
    if (!region.linewise && region.start == region.end && op != u'c') {
        cancel();
        return;
    }
    const int yankCursor = motion.kind == Motion::Linewise ? std::min(m_cursor, motion.target) : region.start;
    operate(op, region, yankCursor);
    done(op != u'y');
}

void VimMode::shiftLines(int from, int to, int times, bool outdent)
{
    if (!m_host->editable()) return;
    int firstLine = 0, span = 0;
    {
        const QString source = text();
        const Lines lines(Text{source});
        firstLine = lines.lineOf(from);
        span = lines.lineOf(to) - firstLine;
    }
    MarkdownEditing::Options options;
    options.newline = newline();
    for (int i = 0; i < times; ++i) {
        std::optional<TextChange> change;
        {
            const QString source = text();
            const Text t{source};
            const int first = t.lineAtNumber(firstLine);
            const int lastEnd = t.lineEnd(t.lineAtNumber(firstLine + span));
            change = MarkdownEditing::indent(source, first, lastEnd, outdent, options);
            if (!change && first < t.lineEnd(first)) {
                // A line that is no list item: a tab more, or a tab or up to four spaces less.
                if (!outdent) {
                    change = TextChange{{SourceEdit{{first, 0}, QStringLiteral("\t")}}, first, first};
                } else {
                    const int end = t.lineEnd(first);
                    int removed = source[first] == u'\t' ? 1 : 0;
                    if (!removed)
                        while (removed < 4 && first + removed < end && source[first + removed] == u' ') ++removed;
                    if (removed) change = TextChange{{SourceEdit{{first, removed}, {}}}, first, first};
                }
            }
        }
        if (!change) break;
        const Moving moving(m_moving);
        if (!m_host->apply(*change)) return;
    }
    const QString after = text();
    const Text t{after};
    setCursor(t.firstNonBlank(t.lineAtNumber(firstLine)));
}

bool VimMode::joinLines(int from, int lines)
{
    if (!m_host->editable()) return false;
    QList<SourceEdit> edits;
    int cursor = from;
    {
        const QString source = text();
        const Text t{source};
        int line = t.lineStart(from);
        if (t.lastLine(line)) return false;
        int delta = 0;
        int joined = t.lineEnd(line) - line;
        QChar last = joined > 0 ? source[t.lineEnd(line) - 1] : QChar();
        for (int k = 1; k < lines; ++k) {
            const int end = t.lineEnd(line);
            if (end >= t.size()) break;
            const int next = end + t.breakAt(end);
            const int nextEnd = t.lineEnd(next);
            int content = next;
            while (content < nextEnd && isBlank(source[content])) ++content;
            // One space between, unless either side has one, the next line
            // is empty or begins with ")", or nothing is there to join to.
            const bool space =
                content < nextEnd && source[content] != u')' && joined > 0 && last != u'\t' && last != u' ';
            const QString inserted = space ? QStringLiteral(" ") : QString();
            edits.append(SourceEdit{{end, content - end}, inserted});
            cursor = end + delta;
            delta += int(inserted.size()) - (content - end);
            joined += int(inserted.size()) + nextEnd - content;
            if (nextEnd > content) last = source[nextEnd - 1];
            else if (space) last = u' ';
            line = next;
        }
    }
    const Moving moving(m_moving);
    if (!m_host->apply(TextChange{edits, cursor, cursor})) return false;
    setCursor(clamped(cursor));
    return true;
}

void VimMode::changeCase(int start, int end, int how, int cursor)
{
    if (!m_host->editable()) return;
    const QString source = text();
    QString changed;
    for (int i = start; i < end; ++i) {
        const QChar c = source[i];
        char32_t code = c.unicode();
        int units = 1;
        if (c.isHighSurrogate() && i + 1 < end && source[i + 1].isLowSurrogate()) {
            code = QChar::surrogateToUcs4(c, source[i + 1]);
            units = 2;
        }
        char32_t to = code;
        if (how == 1 || (how == 0 && QChar::isUpper(code))) to = QChar::toLower(code);
        else if (how == 2 || (how == 0 && QChar::isLower(code))) to = QChar::toUpper(code);
        changed += QString::fromUcs4(&to, 1);
        i += units - 1;
    }
    if (!edit(start, end, changed, start)) return;
    setCursor(clamped(cursor));
}

bool VimMode::replaceChars(int start, int end, const QString &with, int cursor)
{
    if (!m_host->editable()) return false;
    const QString source = text();
    const Text t{source};
    // Every character becomes the one typed; line breaks stay.
    QString replaced;
    for (int p = start; p < end;) {
        if (t.atEnd(p)) {
            const int after = p + t.breakAt(p);
            replaced += source.mid(p, after - p);
            p = after;
        } else {
            replaced += with;
            p = t.next(p);
        }
    }
    if (!edit(start, end, replaced, start)) return false;
    setCursor(clamped(cursor));
    return true;
}

bool VimMode::put(bool before)
{
    const Register reg = readRegister(m_register);
    if (reg.text.isEmpty()) {
        setMessage(tr("Nothing in register %1").arg(m_register.isNull() ? QChar(u'"') : m_register));
        return false;
    }
    if (!m_host->editable()) {
        setMessage(tr("This text cannot be edited"));
        return false;
    }
    const int n = count();
    if (qsizetype(reg.text.size()) * n > maxPutUnits) {
        setMessage(tr("That is too much text to put"));
        return false;
    }
    const QString nl = newline();
    int at = 0, first = 0;
    QString inserted;
    {
        const QString source = text();
        const Text t{source};
        if (reg.linewise) {
            inserted = withNewline(reg.text, nl).repeated(n);
            if (before) {
                at = first = t.lineStart(m_cursor);
            } else if (const int end = t.lineEnd(m_cursor); end < t.size()) {
                at = first = end + t.breakAt(end);
            } else {
                // Below the last line, which has no break of its own.
                at = end;
                inserted = nl + inserted.chopped(nl.size());
                first = at + int(nl.size());
            }
        } else {
            inserted = withNewline(reg.text, nl).repeated(n);
            at = before || t.lineEmpty(m_cursor) ? m_cursor : t.next(m_cursor);
            first = at;
        }
    }
    if (!edit(at, at, inserted, at)) return false;
    const QString after = text();
    const Text t{after};
    if (reg.linewise) setCursor(clamped(t.firstNonBlank(first)));
    else if (reg.text.contains(u'\n')) setCursor(clamped(at));
    else setCursor(clamped(t.previous(at + int(inserted.size()))));
    return true;
}

void VimMode::visualPut(bool before)
{
    const Register reg = readRegister(m_register);
    if (reg.text.isEmpty()) {
        setMessage(tr("Nothing in register %1").arg(m_register.isNull() ? QChar(u'"') : m_register));
        leaveVisual();
        return;
    }
    const Region region = visualRegion(m_mode == VisualLine);
    setMode(Normal);
    const QString nl = newline();
    QString replacement = withNewline(reg.text, nl);
    if (region.linewise && reg.linewise) replacement.chop(nl.size());
    else if (!region.linewise && reg.linewise) replacement = nl + replacement;
    const QString source = text();
    QString removed = source.mid(region.start, region.end - region.start);
    if (!edit(region.start, region.end, replacement, region.start)) {
        setCursor(clamped(region.start));
        return;
    }
    // p keeps what it replaced in the unnamed register; P leaves the registers.
    if (!before) {
        if (region.linewise) removed += u'\n';
        writeRegister(QChar(), {removed, region.linewise}, false);
    }
    const QString after = text();
    const Text t{after};
    if (reg.linewise) setCursor(clamped(t.firstNonBlank(region.linewise ? region.start : region.start + int(nl.size()))));
    else setCursor(clamped(t.previous(region.start + int(replacement.size()))));
}

void VimMode::undo(bool redo)
{
    SourceBuffer *b = buffer();
    if (!b || !m_host->editable()) {
        cancel();
        return;
    }
    m_skipJoin = true;
    int first = -1;
    const auto watch = connect(b, &SourceBuffer::sourceChanged, this, [&first](Revision, SourceRange range, const QString &) {
        if (first < 0 || range.start < first) first = range.start;
    });
    int steps = 0;
    {
        const Moving moving(m_moving);
        for (int i = 0; i < count(); ++i) {
            if (redo ? !b->canRedo() : !b->canUndo()) break;
            if (redo) b->redo();
            else b->undo();
            ++steps;
        }
    }
    disconnect(watch);
    if (steps == 0) setMessage(redo ? tr("Already at the newest change") : tr("Already at the oldest change"));
    else setCursor(clamped(first >= 0 ? first : m_host->cursor()));
    cancel();
}

void VimMode::repeat()
{
    if (m_last.keys.isEmpty()) {
        cancel();
        return;
    }
    const Change change = m_last;
    const int given = hasCount() ? count() : 0;
    cancel();
    ++m_replaying;
    m_register = change.reg;
    if (const int n = given ? given : change.count) m_count1 = n;
    if (change.visual) {
        // As much text from the cursor as the area held.
        const QString source = text();
        const Text t{source};
        m_visualAnchor = m_cursor;
        int end = t.lineStart(m_cursor);
        for (int i = 1; i < change.lines; ++i) {
            const int next = t.nextLine(end);
            if (next < 0) break;
            end = next;
        }
        if (change.visualLines) {
            m_cursor = end;
        } else if (change.lines == 1) {
            end = m_cursor;
            for (int i = 1; i < change.columns && !t.atEnd(end); ++i) end = t.next(end);
            m_cursor = end;
        } else {
            int p = end;
            for (int i = 0; i < change.columns && !t.atEnd(p); ++i) p = t.next(p);
            m_cursor = p;
        }
        setMode(change.visualLines ? VisualLine : Visual);
    }
    for (const QString &k : change.keys) press(k);
    if (m_mode == Insert) {
        m_insertKeys = change.inserted;
        for (const Key &key : change.inserted) replayInsertKey(key);
        leaveInsert();
    }
    --m_replaying;
}

bool VimMode::enterInsert(int position, InsertKind kind, int count)
{
    if (!m_host->editable()) {
        setMessage(tr("This text cannot be edited"));
        return false;
    }
    m_insertMark = m_keyMark;
    m_insertBuffer = buffer();
    m_insertKind = kind;
    m_insertCount = std::clamp(count, 1, maxCount);
    m_insertKeys.clear();
    m_insertOverflow = false;
    setMode(Insert);
    m_cursor = position;
    {
        const Moving moving(m_moving);
        m_host->select(position, position);
    }
    emit cursorChanged();
    return true;
}

void VimMode::openLine(bool below)
{
    const QString nl = newline();
    int at = 0;
    {
        const QString source = text();
        const Text t{source};
        const int c = m_mode == Insert ? m_host->cursor() : m_cursor;
        at = below ? t.lineEnd(c) : t.lineStart(c);
    }
    const int caret = below ? at + int(nl.size()) : at;
    const Moving moving(m_moving);
    m_host->apply(TextChange{{SourceEdit{{at, 0}, nl}}, caret, caret});
}

void VimMode::replayInsertKey(const Key &key)
{
    if (key.commit) m_host->commit(key.text);
    else m_host->type(key.key, key.modifiers, key.text);
}

void VimMode::leaveInsert()
{
    // A count types the same again: 3ix types three x.
    if (m_insertCount > 1 && !m_insertOverflow && m_host->editable()) {
        ++m_replaying;
        const QList<Key> keys = m_insertKeys;
        for (int i = 1; i < m_insertCount; ++i) {
            if (m_insertKind != InsertKind::Plain) openLine(m_insertKind == InsertKind::OpenBelow);
            for (const Key &key : keys) replayInsertKey(key);
        }
        --m_replaying;
    }
    m_insertCount = 1;
    if (m_insertBuffer) m_insertBuffer->joinSince(m_insertMark);
    m_insertBuffer = nullptr;
    if (m_recordingInsert) {
        if (m_insertOverflow) m_last = {};
        else m_last.inserted = m_insertKeys;
        m_recordingInsert = false;
    }
    setMode(Normal);
    const QString source = text();
    const Text t{source};
    int c = std::clamp(m_host->cursor(), 0, t.size());
    if (c > t.lineStart(c)) c = t.previous(c);
    setCursor(clamped(c));
}

void VimMode::openCommandLine(QChar kind)
{
    m_commandLine = QString(kind);
    emit stateChanged();
}

void VimMode::commandLineKey(const QString &k)
{
    const bool forOperator = !m_operator.isNull();
    if (k == u"<Esc>" || k == u"<C-c>") {
        m_commandLine.clear();
        cancel();
        return;
    }
    if (forOperator) m_keys.append(k);
    if (k == u"<CR>") {
        const QString line = m_commandLine;
        m_commandLine.clear();
        executeCommandLine(line);
        return;
    }
    if (k == u"<BS>") {
        if (m_commandLine.size() <= 1) {
            m_commandLine.clear();
            cancel();
            return;
        }
        m_commandLine.truncate(std::max(1, previousSourceGrapheme(m_commandLine, int(m_commandLine.size()))));
    } else if (k == u"<C-v>") {
        const QString pasted = QGuiApplication::clipboard()->text();
        m_commandLine += pasted.left(pasted.indexOf(u'\n') < 0 ? pasted.size() : pasted.indexOf(u'\n'));
    } else if (!isNamedKey(k)) {
        m_commandLine += k;
    }
    emit stateChanged();
}

void VimMode::executeCommandLine(const QString &line)
{
    const QChar kind = line.isEmpty() ? QChar() : line[0];
    const QString body = line.mid(1);
    if (kind == u':') {
        cancel();
        exCommand(body);
        return;
    }
    Search &last = shared().search;
    if (!body.isEmpty()) {
        // Letters match either case unless the pattern has a capital.
        const bool capital = std::any_of(body.cbegin(), body.cend(), [](QChar c) { return c.isUpper(); });
        last = {body, kind == u'/', false, capital};
    } else if (last.pattern.isEmpty()) {
        setMessage(tr("No previous search"));
        cancel();
        return;
    } else {
        last.forward = kind == u'/';
    }
    Motion m;
    motion(QStringLiteral("n"), {}, m_operator.isNull() ? (m_mode == Normal ? Use::Move : Use::Visual) : Use::Operator, &m);
    if (!m_operator.isNull()) {
        finishOperator(m);
        return;
    }
    if (m.ok) {
        if (m_mode == Normal) moveTo(m);
        else visualMove(m);
    }
    done(false);
}

void VimMode::exCommand(const QString &line)
{
    QString command = line.trimmed();
    if (command.isEmpty()) return;
    bool number = false;
    const int lineNumber = command.toInt(&number);
    if (number || command == u"$") {
        const QString source = text();
        const Text t{source};
        const int start = command == u"$" ? t.lastLineStart() : t.lineAtNumber(std::max(1, lineNumber) - 1);
        Motion m;
        m.target = t.firstNonBlank(start);
        m.kind = Motion::Linewise;
        moveTo(m);
        return;
    }
    // parch never throws text away, so ! changes nothing: :q! closes the
    // tab as :q does, which keeps its text.
    if (command.endsWith(u'!')) command.chop(1);
    static const QStringList write{QStringLiteral("w"), QStringLiteral("write")};
    static const QStringList quit{QStringLiteral("q"), QStringLiteral("quit")};
    static const QStringList writeQuit{QStringLiteral("wq"), QStringLiteral("x"), QStringLiteral("xit"),
                                       QStringLiteral("exit")};
    if (write.contains(command)) {
        request(QStringLiteral("save"));
    } else if (quit.contains(command)) {
        // Closing a tab saves its text first, or asks.
        request(QStringLiteral("closeTab"));
    } else if (writeQuit.contains(command)) {
        // A draft is named, and a conflict reviewed, before anything closes.
        Document *document = m_host->document();
        const bool asks = document && (document->isDraft() || document->state() == Document::State::Conflict);
        request(asks ? QStringLiteral("save") : QStringLiteral("closeTab"));
    } else {
        setMessage(tr("Not an editor command: %1").arg(line.trimmed()));
    }
}

void VimMode::normalKey(const QString &k)
{
    if (!m_operator.isNull()) {
        operatorKey(k);
        return;
    }
    const int n = count();
    Motion m;
    if (motion(k, {}, Use::Move, &m)) {
        if (m.ok) moveTo(m);
        done(false);
        return;
    }
    const QChar c = k.size() == 1 ? k[0] : QChar();
    if (c == u'd' || c == u'c' || c == u'y' || c == u'>' || c == u'<') {
        m_operator = c;
        showTyped(k);
        return;
    }
    if (c == u'f' || c == u't' || c == u'F' || c == u'T' || c == u'r') {
        m_await = Await::Char;
        m_awaitFor = k;
        showTyped(k);
        return;
    }
    if (c == u'g') {
        m_await = Await::G;
        showTyped(k);
        return;
    }
    if (k == u"u" || k == u"<C-r>") {
        undo(k == u"<C-r>");
        return;
    }
    if (k == u".") {
        repeat();
        done(false);
        return;
    }
    if (k == u"v" || k == u"V") {
        startVisual(k == u"v" ? Visual : VisualLine);
        done(false);
        return;
    }
    if (k == u":" || k == u"/" || k == u"?") {
        openCommandLine(c);
        return;
    }
    if (k == u"Y") {
        m_operator = u'y';
        operatorKey(QStringLiteral("y"));
        return;
    }
    // The rest change the text.
    const bool changes = QStringList{QStringLiteral("i"), QStringLiteral("<Insert>"), QStringLiteral("a"),
                                     QStringLiteral("I"), QStringLiteral("A"), QStringLiteral("o"),
                                     QStringLiteral("O"), QStringLiteral("x"), QStringLiteral("<Del>"),
                                     QStringLiteral("X"), QStringLiteral("s"), QStringLiteral("S"),
                                     QStringLiteral("C"), QStringLiteral("D"), QStringLiteral("J"),
                                     QStringLiteral("~"), QStringLiteral("p"), QStringLiteral("P")}
                             .contains(k);
    if (!changes) {
        cancel();
        return;
    }
    if (!m_host->editable()) {
        setMessage(tr("This text cannot be edited"));
        cancel();
        return;
    }
    const QString source = text();
    const Text t{source};
    const int from = std::clamp(m_cursor, 0, t.size());
    if (k == u"i" || k == u"<Insert>") {
        done(enterInsert(from, InsertKind::Plain, n));
    } else if (k == u"a") {
        done(enterInsert(t.atEnd(from) ? from : t.next(from), InsertKind::Plain, n));
    } else if (k == u"I") {
        done(enterInsert(t.firstNonBlank(from), InsertKind::Plain, n));
    } else if (k == u"A") {
        done(enterInsert(t.lineEnd(from), InsertKind::Plain, n));
    } else if (k == u"o" || k == u"O") {
        openLine(k == u"o");
        done(enterInsert(m_host->cursor(), k == u"o" ? InsertKind::OpenBelow : InsertKind::OpenAbove, n));
    } else if (k == u"x" || k == u"<Del>" || k == u"s") {
        int end = from;
        for (int i = 0; i < n && !t.atEnd(end); ++i) end = t.next(end);
        if (k == u"s") {
            changeRegion({from, end, false});
            done(true);
        } else if (end > from) {
            deleteRegion({from, end, false});
            done(true);
        } else {
            cancel();
        }
    } else if (k == u"X") {
        int start = from;
        for (int i = 0; i < n && start > t.lineStart(from); ++i) start = t.previous(start);
        if (start < from) {
            deleteRegion({start, from, false});
            done(true);
        } else {
            cancel();
        }
    } else if (k == u"S") {
        m_operator = u'c';
        operatorKey(QStringLiteral("c"));
    } else if (k == u"C" || k == u"D") {
        m_operator = k == u"C" ? u'c' : u'd';
        operatorKey(QStringLiteral("$"));
    } else if (k == u"J") {
        done(joinLines(from, std::max(2, n)));
    } else if (k == u"~") {
        int end = from;
        for (int i = 0; i < n && !t.atEnd(end); ++i) end = t.next(end);
        if (end > from) {
            changeCase(from, end, 0, end);
            done(true);
        } else {
            cancel();
        }
    } else if (k == u"p" || k == u"P") {
        done(put(k == u"P"));
    }
}

void VimMode::operatorKey(const QString &k)
{
    const QChar op = m_operator;
    const int n = count();
    if (k.size() == 1 && k[0] == op) {
        // dd, cc, yy, >> and <<: count lines from the cursor's.
        const QString source = text();
        const Text t{source};
        if (n > 1 && t.lastLine(m_cursor)) {
            cancel();
            return;
        }
        int last = t.lineStart(m_cursor);
        for (int i = 1; i < n; ++i) {
            const int next = t.nextLine(last);
            if (next < 0) break;
            last = next;
        }
        Motion m;
        m.target = last;
        m.kind = Motion::Linewise;
        // yy leaves the cursor where it is.
        const int keep = m_cursor;
        finishOperator(m);
        if (op == u'y') setCursor(keep);
        return;
    }
    const QChar c = k.size() == 1 ? k[0] : QChar();
    if (c == u'i' || c == u'a') {
        m_await = Await::Object;
        m_awaitFor = k;
        showTyped(k);
        return;
    }
    if (c == u'f' || c == u't' || c == u'F' || c == u'T') {
        m_await = Await::Char;
        m_awaitFor = k;
        showTyped(k);
        return;
    }
    if (c == u'g') {
        m_await = Await::G;
        showTyped(k);
        return;
    }
    if (c == u'/' || c == u'?') {
        openCommandLine(c);
        return;
    }
    Motion m;
    if (op == u'c' && (k == u"w" || k == u"W")) {
        const QString source = text();
        const Text t{source};
        if (!t.atEnd(m_cursor) && t.cls(m_cursor, false) != 0) {
            // cw on a word changes to its end, as ce does.
            int p = m_cursor;
            endWord(t, p, n, k == u"W", true, false);
            m.target = p;
            m.kind = Motion::Inclusive;
            finishOperator(m);
            return;
        }
    }
    if (!motion(k, {}, Use::Operator, &m)) {
        cancel();
        return;
    }
    finishOperator(m);
}

void VimMode::awaited(const QString &k)
{
    const Await await = m_await;
    const QString command = m_awaitFor;
    m_await = Await::None;
    m_awaitFor.clear();
    if (await == Await::Register) {
        const QChar name = k.size() == 1 ? k[0] : QChar();
        if (!isRegisterName(name)) {
            cancel();
            return;
        }
        m_register = name;
        showTyped(k);
        return;
    }
    m_keys.append(k);
    const bool visual = m_mode == Visual || m_mode == VisualLine;
    const Use use = !m_operator.isNull() ? Use::Operator : visual ? Use::Visual : Use::Move;
    const auto finish = [&](const Motion &m) {
        if (use == Use::Operator) {
            finishOperator(m);
            return;
        }
        if (m.ok) {
            if (visual) visualMove(m);
            else moveTo(m);
        }
        done(false);
    };
    if (await == Await::G) {
        Motion m;
        if (k != u"g" || !motion(QStringLiteral("gg"), {}, use, &m)) {
            cancel();
            return;
        }
        finish(m);
        return;
    }
    if (await == Await::Char) {
        QString argument = k;
        if (isNamedKey(k)) {
            if (command == u"r" && k == u"<CR>") argument = QStringLiteral("\n");
            else if (k == u"<Tab>") argument = QStringLiteral("\t");
            else {
                cancel();
                return;
            }
        }
        if (command == u"r") {
            if (!m_host->editable()) {
                setMessage(tr("This text cannot be edited"));
                cancel();
                return;
            }
            const QString source = text();
            const Text t{source};
            if (visual) {
                const Region region = visualRegion(m_mode == VisualLine);
                rememberVisualSize();
                setMode(Normal);
                replaceChars(region.start, region.end, argument, region.start);
                done(true);
                return;
            }
            const int n = count();
            int end = m_cursor;
            for (int i = 0; i < n; ++i) {
                if (t.atEnd(end)) {
                    cancel();
                    return;
                }
                end = t.next(end);
            }
            if (argument == u"\n") {
                const QString nl = newline();
                edit(m_cursor, end, nl, m_cursor + int(nl.size()));
                setCursor(m_cursor + int(nl.size()));
            } else {
                // The cursor ends on the last character replaced.
                replaceChars(m_cursor, end, argument, m_cursor + int(argument.size()) * (n - 1));
            }
            done(true);
            return;
        }
        Motion m;
        motion(command, argument, use, &m);
        finish(m);
        return;
    }
    // A text object.
    const bool inner = command == u"i";
    const QChar type = k.size() == 1 ? k[0] : QChar();
    int start = 0, end = 0;
    bool linewise = false;
    if (!textObject(type, inner, &start, &end, &linewise)) {
        cancel();
        return;
    }
    if (visual) {
        // textObject() has set the area.
        showVisual();
        emit cursorChanged();
        done(false);
        return;
    }
    if (m_operator != u'y' && !m_host->editable()) {
        setMessage(tr("This text cannot be edited"));
        cancel();
        return;
    }
    const QChar op = m_operator;
    if (!linewise && start == end && op != u'c') {
        cancel();
        return;
    }
    operate(op, {start, end, linewise}, start);
    done(op != u'y');
}

bool VimMode::textObject(QChar type, bool inner, int *start, int *end, bool *linewise)
{
    const QString source = text();
    const Text t{source};
    const int n = count();
    const bool visual = m_mode == Visual || m_mode == VisualLine;
    *linewise = false;
    // An operator's region, with the end of an exclusive object treated as
    // an exclusive motion's.
    const auto region = [&](int from, int to, bool exclusive) {
        Motion m;
        if (to <= from) {
            *start = *end = from;
            return;
        }
        m.target = exclusive ? to : t.previous(to);
        m.kind = exclusive ? Motion::Exclusive : Motion::Inclusive;
        const Region r = regionFor(from, m, m_operator == u'd');
        *start = r.start;
        *end = r.end;
        *linewise = r.linewise;
    };
    if (type == u'w' || type == u'W') {
        int p = m_cursor;
        int other = visual ? m_visualAnchor : -1;
        int from = 0;
        bool inclusive = true;
        if (!currentWord(t, p, other, n, !inner, type == u'W', &from, &inclusive)) return false;
        if (visual) {
            if (m_mode == VisualLine) setMode(Visual);
            m_visualAnchor = other;
            m_cursor = p;
            return true;
        }
        if (inclusive) region(from, t.atEnd(p) ? p : t.next(p), false);
        else region(from, p, true);
        return true;
    }
    if (type == u'p') {
        int first = 0, last = 0;
        bool extended = false;
        if (!currentParagraph(t, m_cursor, visual ? m_visualAnchor : -1, m_mode == VisualLine, n, !inner, &first, &last,
                              &extended))
            return false;
        if (visual) {
            if (extended) {
                const Lines lines(t);
                m_cursor = lines.starts[first];
            } else {
                m_visualAnchor = first;
                m_cursor = last;
            }
            setMode(VisualLine);
            return true;
        }
        *start = first;
        *end = t.lineEnd(last);
        *linewise = true;
        return true;
    }
    static const QHash<QChar, std::pair<QChar, QChar>> blocks{
        {u'(', {u'(', u')'}}, {u')', {u'(', u')'}}, {u'b', {u'(', u')'}}, {u'[', {u'[', u']'}},
        {u']', {u'[', u']'}}, {u'{', {u'{', u'}'}}, {u'}', {u'{', u'}'}}, {u'B', {u'{', u'}'}}};
    if (blocks.contains(type)) {
        const auto [open, close] = blocks.value(type);
        int from = 0, to = 0;
        bool exclusive = false;
        const int low = visual ? std::min(m_visualAnchor, m_cursor) : -1;
        const int high = visual ? std::max(m_visualAnchor, m_cursor) : -1;
        if (!currentBlock(t, m_cursor, low, high, n, !inner, open, close, &from, &to, &exclusive)) return false;
        if (visual) {
            if (to <= from) return false;
            setMode(Visual);
            m_visualAnchor = from;
            m_cursor = to == t.lineStart(to) && to > 0 ? t.previousLineEnd(to) : t.previous(to);
            return true;
        }
        region(from, to, exclusive);
        return true;
    }
    if (type == u'"' || type == u'\'' || type == u'`') {
        int from = 0, to = 0;
        if (!currentQuote(t, m_cursor, n, !inner, type, &from, &to)) return false;
        if (visual) {
            if (to <= from) return false;
            setMode(Visual);
            m_visualAnchor = from;
            m_cursor = t.previous(to);
            return true;
        }
        *start = from;
        *end = to;
        return true;
    }
    return false;
}

void VimMode::rememberVisualSize()
{
    const QString source = text();
    const Text t{source};
    const int a = std::min(m_visualAnchor, m_cursor), b = std::max(m_visualAnchor, m_cursor);
    m_visualSize.visual = true;
    m_visualSize.visualLines = m_mode == VisualLine;
    m_visualSize.lines = 1;
    for (int line = t.nextLine(a); line >= 0 && line <= b; line = t.nextLine(line)) ++m_visualSize.lines;
    // One line: how many characters; more: the column of the last.
    int graphemes = 0;
    for (int p = m_visualSize.lines == 1 ? a : t.lineStart(b); p < b; p = t.next(p)) ++graphemes;
    m_visualSize.columns = m_visualSize.lines == 1 ? graphemes + 1 : graphemes;
}

void VimMode::visualKey(const QString &k)
{
    const int n = count();
    Motion m;
    if (motion(k, {}, Use::Visual, &m)) {
        if (m.ok) visualMove(m);
        done(false);
        return;
    }
    const QChar c = k.size() == 1 ? k[0] : QChar();
    if (c == u'f' || c == u't' || c == u'F' || c == u'T' || c == u'r') {
        m_await = Await::Char;
        m_awaitFor = k;
        showTyped(k);
        return;
    }
    if (c == u'g') {
        m_await = Await::G;
        showTyped(k);
        return;
    }
    if (c == u'i' || c == u'a') {
        m_await = Await::Object;
        m_awaitFor = k;
        showTyped(k);
        return;
    }
    if (k == u"v" || k == u"V") {
        const Mode wanted = k == u"v" ? Visual : VisualLine;
        if (m_mode == wanted) {
            leaveVisual();
        } else {
            setMode(wanted);
            showVisual();
        }
        done(false);
        return;
    }
    if (k == u"o") {
        std::swap(m_visualAnchor, m_cursor);
        showVisual();
        emit cursorChanged();
        done(false);
        return;
    }
    if (k == u":") {
        leaveVisual();
        cancel();
        openCommandLine(c);
        return;
    }
    if (k == u"/" || k == u"?") {
        openCommandLine(c);
        return;
    }
    const bool lines = m_mode == VisualLine;
    if (k == u"y" || k == u"Y") {
        const Region region = visualRegion(lines || k == u"Y");
        setMode(Normal);
        yankRegion(region, region.start);
        done(false);
        return;
    }
    static const QStringList changes{QStringLiteral("d"), QStringLiteral("x"), QStringLiteral("<Del>"), QStringLiteral("D"),
                                     QStringLiteral("X"), QStringLiteral("c"), QStringLiteral("s"),     QStringLiteral("C"),
                                     QStringLiteral("S"), QStringLiteral("R"), QStringLiteral(">"),     QStringLiteral("<"),
                                     QStringLiteral("J"), QStringLiteral("~"), QStringLiteral("u"),     QStringLiteral("U"),
                                     QStringLiteral("p"), QStringLiteral("P")};
    if (!changes.contains(k)) {
        cancel();
        return;
    }
    if (!m_host->editable()) {
        setMessage(tr("This text cannot be edited"));
        cancel();
        return;
    }
    rememberVisualSize();
    const bool upper = k.size() == 1 && k[0].isUpper() && k != u"J" && k != u"U" && k != u"P";
    const Region region = visualRegion(lines || upper);
    if (k == u"d" || k == u"x" || k == u"<Del>" || k == u"D" || k == u"X") {
        setMode(Normal);
        deleteRegion(region);
    } else if (k == u"c" || k == u"s" || k == u"C" || k == u"S" || k == u"R") {
        setMode(Normal);
        changeRegion(region);
    } else if (k == u">" || k == u"<") {
        const Region whole = visualRegion(true);
        setMode(Normal);
        shiftLines(whole.start, whole.end, n, k == u"<");
    } else if (k == u"J") {
        const Region whole = visualRegion(true);
        setMode(Normal);
        joinLines(whole.start, std::max(2, m_visualSize.lines));
    } else if (k == u"~" || k == u"u" || k == u"U") {
        setMode(Normal);
        changeCase(region.start, region.end, k == u"~" ? 0 : k == u"u" ? 1 : 2, region.start);
    } else if (k == u"p" || k == u"P") {
        visualPut(k == u"P");
    }
    done(true);
}
