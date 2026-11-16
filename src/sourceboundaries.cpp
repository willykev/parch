#include "sourceboundaries.h"
#include <QTextBoundaryFinder>

namespace {
bool separator(QChar c)
{ return c == u'\r' || c == u'\n' || c == QChar(0x2029); }
bool printableAscii(QChar c)
{ return c.unicode() >= 0x20 && c.unicode() <= 0x7e; }

// Include the complete trailing hard separator, especially both halves of CRLF.
// GCB resets at CR/LF/PS; LS and all other Unicode context stay in the slice.
QStringView boundaryContext(QStringView source, int position, int &start)
{
    const auto paragraph = sourceParagraphRange(source, position);
    start = paragraph.start;
    int end = start + paragraph.length;
    if (end < source.size()) {
        if (source[end] == u'\r' && end + 1 < source.size() && source[end + 1] == u'\n') end += 2;
        else ++end;
    }
    return source.sliced(start, end - start);
}
}

SourceRange sourceParagraphRange(QStringView source, int position)
{
    if (position < 0 || position > source.size()) return {-1, 0};
    // A position between CR and LF belongs to the paragraph before the pair.
    if (position > 0 && position < source.size()
        && source[position - 1] == u'\r' && source[position] == u'\n') --position;
    int start = position;
    while (start > 0 && !separator(source[start - 1])) --start;
    int end = position;
    while (end < source.size() && !separator(source[end])) ++end;
    return {start, end - start};
}

bool sourceGraphemeBoundary(QStringView source, int position)
{
    if (position < 0 || position > source.size()) return false;
    if (position == 0 || position == source.size()) return true;
    // Only this two-sided printable-ASCII case is independent of Unicode context.
    if (printableAscii(source[position - 1]) && printableAscii(source[position])) return true;
    int start;
    const auto context = boundaryContext(source, position, start);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, context);
    finder.setPosition(position - start);
    return finder.isAtBoundary();
}

bool sourceGraphemeRange(QStringView source, SourceRange range)
{
    // Subtract before adding so even hostile INT_MAX ranges cannot overflow.
    if (range.start < 0 || range.length < 0 || range.start > source.size()
        || range.length > source.size() - range.start) return false;
    return sourceGraphemeBoundary(source, range.start)
        && (range.length == 0 || sourceGraphemeBoundary(source, range.start + range.length));
}

int previousSourceGrapheme(QStringView source, int position)
{
    if (position <= 0 || position > source.size()) return -1;
    if (sourceGraphemeBoundary(source, position - 1)) return position - 1;
    int start;
    // Look to the left so paragraph-start navigation includes the preceding separator.
    const auto context = boundaryContext(source, position - 1, start);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, context);
    finder.setPosition(position - start);
    const auto previous = finder.toPreviousBoundary();
    return previous < 0 ? -1 : start + previous;
}

int nextSourceGrapheme(QStringView source, int position)
{
    if (position < 0 || position >= source.size()) return -1;
    if (sourceGraphemeBoundary(source, position + 1)) return position + 1;
    int start;
    const auto context = boundaryContext(source, position, start);
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, context);
    finder.setPosition(position - start);
    const auto next = finder.toNextBoundary();
    return next < 0 ? -1 : start + next;
}
