#include "mdtable.h"

#include <QTextBoundaryFinder>
#include <algorithm>

namespace MdTable {

namespace {

bool isSpaceOrTab(QChar c) { return c == u' ' || c == u'\t'; }
bool isBreak(QChar c) { return c == u'\n' || c == u'\r'; }
bool isAsciiPunct(char16_t c)
{
    return (c >= 0x21 && c <= 0x2f) || (c >= 0x3a && c <= 0x40) || (c >= 0x5b && c <= 0x60) || (c >= 0x7b && c <= 0x7e);
}

// The line holding a position, its text without the quote markers and
// indentation before it.
Line lineAt(QStringView text, int position)
{
    const int size = int(text.size());
    position = std::clamp(position, 0, size);
    int start = position;
    while (start > 0 && !isBreak(text[start - 1])) --start;
    // Between the CR and LF of a pair, the line is the one the pair ends.
    if (start == position && start > 0 && text[start - 1] == u'\r' && start < size && text[start] == u'\n') {
        --start;
        while (start > 0 && !isBreak(text[start - 1])) --start;
    }
    int end = start;
    while (end < size && !isBreak(text[end])) ++end;
    int next = end;
    if (next < size) next += text[next] == u'\r' && next + 1 < size && text[next + 1] == u'\n' ? 2 : 1;
    int content = start;
    while (content < end && (isSpaceOrTab(text[content]) || text[content] == u'>')) ++content;
    return {start, content, end, next};
}

std::optional<Line> lineAfter(QStringView text, const Line &line)
{
    if (line.next == line.end) return std::nullopt;
    return lineAt(text, line.next);
}

std::optional<Line> lineBefore(QStringView text, const Line &line)
{
    if (line.start == 0) return std::nullopt;
    int previous = line.start - 1;
    if (previous > 0 && text[previous] == u'\n' && text[previous - 1] == u'\r') --previous;
    return lineAt(text, previous);
}

// The end of a line's text without the spaces and tabs after it.
int trimmedEnd(QStringView text, const Line &line)
{
    int end = line.end;
    while (end > line.content && isSpaceOrTab(text[end - 1])) --end;
    return end;
}

bool isBlank(QStringView text, const Line &line) { return trimmedEnd(text, line) == line.content; }

bool isComment(QStringView text, const Line &line, const Dialect &dialect)
{
    return dialect.lineComments && line.end - line.start >= 2 && text[line.start] == u'/' && text[line.start + 1] == u'/';
}

bool hasPipe(QStringView text, const Line &line, const Dialect &dialect)
{
    QList<Field> fields;
    QList<int> pipes;
    splitRow(text, line.content, trimmedEnd(text, line), dialect.protectCode, fields, &pipes);
    return !pipes.isEmpty();
}

int codePointWidth(char32_t c)
{
    switch (QChar::category(c)) {
    case QChar::Mark_NonSpacing:
    case QChar::Mark_Enclosing:
    case QChar::Other_Format:
    case QChar::Other_Control:
        return 0;
    default:
        break;
    }
    // East Asian Wide and Fullwidth characters, and emoji shown as wide.
    static const std::pair<char32_t, char32_t> wide[] = {
        {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},
        {0x23F3, 0x23F3},   {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},
        {0x2693, 0x2693},   {0x26A1, 0x26A1},   {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},
        {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},   {0x26F2, 0x26F3},   {0x26F5, 0x26F5},
        {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},   {0x2728, 0x2728},
        {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},
        {0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},
        {0x2E80, 0x303E},   {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},
        {0xA960, 0xA97F},   {0xAC00, 0xD7A3},   {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},
        {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4}, {0x17000, 0x18AFF}, {0x1B000, 0x1B2FF},
        {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F200, 0x1F251},
        {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF},
        {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
    };
    for (const auto &[first, last] : wide)
        if (c >= first && c <= last) return 2;
    return 1;
}

// A row's fields as cells: a field of no width widens the cell before it
// when the dialect merges, and cells the row leaves out are added, empty,
// at its end.
Row rowOf(QStringView text, const Line &line, int columns, const Dialect &dialect, bool *extra, bool *leadingMerge)
{
    Row row;
    row.line = line;
    const int end = trimmedEnd(text, line);
    QList<Field> fields;
    splitRow(text, line.content, end, dialect.protectCode, fields);
    int column = 0;
    for (const Field &field : fields) {
        if (dialect.mergedCells && field.zeroWidth && !row.cells.isEmpty()) {
            Cell &owner = row.cells.last();
            ++owner.span;
            owner.rawEnd = field.rawEnd;
            ++column;
            continue;
        }
        if (dialect.mergedCells && field.zeroWidth) *leadingMerge = true;
        Cell cell;
        cell.column = column;
        cell.start = field.start;
        cell.end = field.end;
        cell.rawStart = field.rawStart;
        cell.rawEnd = field.rawEnd;
        row.cells.append(cell);
        ++column;
    }
    if (column > columns) *extra = true;
    while (column < columns) {
        Cell cell;
        cell.column = column++;
        cell.start = cell.end = cell.rawStart = cell.rawEnd = end;
        cell.missing = true;
        row.cells.append(cell);
    }
    return row;
}

QString padded(QStringView cell, int width, Align align)
{
    const int room = qMax(0, width - displayWidth(cell));
    switch (align) {
    case Align::Right:
        return QString(room, u' ') + cell;
    case Align::Center:
        return QString(room / 2, u' ') + cell + QString(room - room / 2, u' ');
    default:
        return cell + QString(room, u' ');
    }
}

QString dashes(int width, Align align)
{
    switch (align) {
    case Align::Left:
        return u':' + QString(width - 1, u'-');
    case Align::Right:
        return QString(width - 1, u'-') + u':';
    case Align::Center:
        return u':' + QString(width - 2, u'-') + u':';
    default:
        return QString(width, u'-');
    }
}

} // namespace

bool splitRow(QStringView text, int from, int to, bool protectCode, QList<Field> &fields, QList<int> *pipes)
{
    fields.clear();
    int i = from;
    if (i < to && text[i] == u'|') {
        if (pipes) pipes->append(i);
        ++i;
    }
    quint64 unclosed = 0; // lengths of backtick runs known to have no partner
    while (i < to) {
        const int start = i;
        while (i < to) {
            const char16_t c = text[i].unicode();
            if (c == u'\\' && i + 1 < to && isAsciiPunct(text[i + 1].unicode())) {
                i += 2;
                continue;
            }
            if (c == u'|') break;
            if (c == u'`' && protectCode) {
                int run = i;
                while (run < to && text[run] == u'`') ++run;
                const int length = run - i;
                int close = -1;
                if (length < 64 && !(unclosed & (quint64(1) << length))) {
                    for (int k = run; k < to;) {
                        if (text[k] != u'`') {
                            ++k;
                            continue;
                        }
                        int e = k;
                        while (e < to && text[e] == u'`') ++e;
                        if (e - k == length) {
                            close = e;
                            break;
                        }
                        k = e;
                    }
                    if (close < 0) unclosed |= quint64(1) << length;
                }
                i = close >= 0 ? close : run;
                continue;
            }
            ++i;
        }
        const bool pipe = i < to;
        if (i > start || pipe) {
            Field field;
            field.rawStart = field.start = start;
            field.rawEnd = field.end = i;
            field.zeroWidth = i == start;
            while (field.start < field.end && isSpaceOrTab(text[field.start])) ++field.start;
            while (field.end > field.start && isSpaceOrTab(text[field.end - 1])) --field.end;
            fields.append(field);
        }
        if (pipe) {
            if (pipes) pipes->append(i);
            ++i;
        }
    }
    return !fields.isEmpty();
}

std::optional<QList<Align>> delimiterRow(QStringView text, int from, int to)
{
    QList<Field> fields;
    if (!splitRow(text, from, to, false, fields)) return std::nullopt;
    QList<Align> aligns;
    for (const Field &field : fields) {
        int a = field.start, b = field.end;
        const bool left = a < b && text[a] == u':';
        if (left) ++a;
        const bool right = b > a && text[b - 1] == u':';
        if (right) --b;
        if (a >= b) return std::nullopt;
        for (int i = a; i < b; ++i)
            if (text[i] != u'-') return std::nullopt;
        aligns.append(left && right ? Align::Center : left ? Align::Left : right ? Align::Right : Align::None);
    }
    return aligns;
}

int displayWidth(QStringView text)
{
    int width = 0;
    // ASCII is one column a character, controls none.
    bool ascii = true;
    for (QChar c : text)
        if (c.unicode() >= 0x7f) {
            ascii = false;
            break;
        }
    if (ascii) {
        for (QChar c : text) width += c.unicode() >= 0x20 ? 1 : 0;
        return width;
    }
    QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, text);
    int start = 0;
    for (qsizetype end = graphemes.toNextBoundary(); end > 0; end = graphemes.toNextBoundary()) {
        int widest = 0;
        const QStringView grapheme = text.mid(start, end - start);
        for (char32_t c : grapheme.toUcs4()) widest = qMax(widest, codePointWidth(c));
        width += widest;
        start = int(end);
    }
    return width;
}

std::optional<Table> scan(QStringView text, int from, int to, const Dialect &dialect)
{
    const Line header = lineAt(text, from);
    const auto second = lineAfter(text, header);
    if (!second || isBlank(text, header)) return std::nullopt;
    QList<Field> fields;
    splitRow(text, header.content, trimmedEnd(text, header), dialect.protectCode, fields);
    const auto aligns = delimiterRow(text, second->content, trimmedEnd(text, *second));
    if (!aligns || fields.size() != aligns->size()) return std::nullopt;

    Table table;
    table.start = header.start;
    table.columns = int(aligns->size());
    table.aligns = *aligns;
    bool extra = false, leading = false;
    table.rows.append(rowOf(text, header, table.columns, dialect, &extra, &leading));
    if (extra || leading) {
        table.unfit = leading ? Unfit::LeadingMerge : Unfit::ExtraCells;
        table.unfitRow = 0;
    }
    table.rows.append(Row{*second, {}});
    table.end = second->end;
    const int last = std::max(lineAt(text, to).start, second->start);
    for (auto line = lineAfter(text, *second); line && line->start <= last; line = lineAfter(text, *line)) {
        if (isBlank(text, *line)) break;
        if (isComment(text, *line, dialect)) {
            if (table.unfit == Unfit::None) {
                table.unfit = Unfit::CommentLine;
                table.unfitRow = int(table.rows.size());
            }
            break;
        }
        extra = leading = false;
        table.rows.append(rowOf(text, *line, table.columns, dialect, &extra, &leading));
        table.end = line->end;
        if ((extra || leading) && table.unfit == Unfit::None) {
            table.unfit = leading ? Unfit::LeadingMerge : Unfit::ExtraCells;
            table.unfitRow = int(table.rows.size()) - 1;
        }
    }
    if (table.unfit == Unfit::None
        && (table.columns > MaxColumns || qsizetype(table.columns) * (table.rows.size() - 1) > MaxCells))
        table.unfit = Unfit::TooLarge;
    return table;
}

std::optional<Table> locate(QStringView text, int position, const Dialect &dialect)
{
    const Line here = lineAt(text, position);
    const auto rowLike = [&](const Line &line) {
        return !isBlank(text, line) && !isComment(text, line, dialect) && hasPipe(text, line, dialect);
    };
    if (!rowLike(here)) return std::nullopt;
    Line first = here;
    while (const auto above = lineBefore(text, first)) {
        if (!rowLike(*above)) break;
        first = *above;
    }
    QList<Line> lines;
    for (std::optional<Line> line = first; line && rowLike(*line); line = lineAfter(text, *line)) lines.append(*line);
    // The header is the line just above the first delimiter row.
    for (int i = 1; i < lines.size(); ++i) {
        const auto aligns = delimiterRow(text, lines[i].content, trimmedEnd(text, lines[i]));
        if (!aligns) continue;
        if (lines[i - 1].start > here.start) return std::nullopt;
        return scan(text, lines[i - 1].start, lines.last().start, dialect);
    }
    return std::nullopt;
}

int rowAt(const Table &table, int position)
{
    for (int i = 0; i < table.rows.size(); ++i) {
        const Line &line = table.rows[i].line;
        if (position >= line.start && (position < line.next || position <= line.end)) return i;
    }
    return -1;
}

int cellAt(const Table &table, int row, int position)
{
    const QList<Cell> &cells = table.rows[row].cells;
    int lastPresent = 0;
    for (int i = 0; i < cells.size(); ++i) {
        if (cells[i].missing) continue;
        if (position <= cells[i].rawEnd) return i;
        lastPresent = i;
    }
    return lastPresent;
}

Model model(QStringView text, const Table &table)
{
    Model model;
    model.aligns = table.aligns;
    for (int r = 0; r < table.rows.size(); ++r) {
        const Row &row = table.rows[r];
        model.prefixes.append(text.mid(row.line.start, row.line.content - row.line.start).toString());
        if (r + 1 < table.rows.size()) model.breaks.append(text.mid(row.line.end, row.line.next - row.line.end).toString());
        if (r == 1) continue;
        QList<Item> items;
        items.reserve(row.cells.size());
        for (const Cell &cell : row.cells) items.append(Item{text.mid(cell.start, cell.end - cell.start).toString(), cell.span});
        model.rows.append(items);
    }
    return model;
}

Layout write(const Model &model)
{
    int columns = int(model.aligns.size());
    for (const QList<Item> &row : model.rows) {
        int width = 0;
        for (const Item &item : row) width += item.span;
        columns = qMax(columns, width);
    }
    QList<int> widths(columns, 3);
    struct Spanning {
        int first, last, width;
    };
    QList<Spanning> spanning;
    for (const QList<Item> &row : model.rows) {
        int column = 0;
        for (const Item &item : row) {
            const int width = displayWidth(item.text);
            if (item.span == 1) widths[column] = qMax(widths[column], width);
            else spanning.append({column, column + item.span - 1, width});
            column += item.span;
        }
    }
    // A merged cell takes the columns it covers and the pipes between them;
    // one wider than that widens the last of its columns.
    std::stable_sort(spanning.begin(), spanning.end(), [](const Spanning &a, const Spanning &b) {
        return a.last != b.last ? a.last < b.last : a.first < b.first;
    });
    for (const Spanning &span : spanning) {
        int room = 2 * (span.last - span.first);
        for (int c = span.first; c <= span.last; ++c) room += widths[c];
        if (span.width > room) widths[span.last] += span.width - room;
    }
    const auto alignOf = [&](int column) { return column < model.aligns.size() ? model.aligns[column] : Align::None; };

    Layout layout;
    const int lines = int(model.rows.size()) + 1;
    layout.cells.resize(model.rows.size());
    QString &out = layout.text;
    for (int line = 0; line < lines; ++line) {
        layout.lineStarts.append(int(out.size()));
        out += line < model.prefixes.size() ? model.prefixes[line] : (model.prefixes.isEmpty() ? QString() : model.prefixes[0]);
        out += u'|';
        if (line == 1) {
            for (int c = 0; c < model.aligns.size(); ++c) out += u' ' + dashes(widths[c], model.aligns[c]) + QStringLiteral(" |");
        } else {
            const int r = line == 0 ? 0 : line - 1;
            int column = 0;
            QList<Placed> &placed = layout.cells[r];
            for (const Item &item : model.rows[r]) {
                int room = 2 * (item.span - 1);
                for (int c = column; c < column + item.span && c < columns; ++c) room += widths[c];
                const Align align = alignOf(column);
                const int width = displayWidth(item.text);
                const int free = qMax(0, room - width);
                const int lead = align == Align::Right ? free : align == Align::Center ? free / 2 : 0;
                out += u' ';
                placed.append(Placed{int(out.size()) + lead, int(item.text.size()), free - lead + 1});
                out += padded(item.text, room, align);
                out += u' ';
                out += QString(item.span, u'|');
                column += item.span;
            }
        }
        if (line + 1 < lines) out += line < model.breaks.size() ? model.breaks[line] : QStringLiteral("\n");
    }
    return layout;
}

} // namespace MdTable
