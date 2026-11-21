#include "tablegrids.h"

#include "diskfile.h"
#include "document.h"
#include "documentparse.h"
#include "editorbinding.h"
#include "editordecorations.h"
#include "htmlmarkdown.h"
#include "mdhtml.h"
#include "mdtree.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QStyleHints>
#include <QMimeData>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QCoreApplication>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QSGFlatColorMaterial>
#include <QSGGeometryNode>
#include <QSGRectangleNode>
#include <QWheelEvent>
#include <QSGTextNode>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <algorithm>
#include <cmath>
#include <span>

namespace {

// While the grids change the binding's projection, its notices of the
// change are its own, as when it follows the source.
class Projecting {
public:
    explicit Projecting(bool &flag) : m_flag(flag), m_was(std::exchange(flag, true)) {}
    ~Projecting() { m_flag = m_was; }

private:
    bool &m_flag;
    bool m_was;
};

void shiftTable(MdTable::Table &table, int delta)
{
    if (delta == 0) return;
    table.start += delta;
    table.end += delta;
    for (MdTable::Row &row : table.rows) {
        row.line.start += delta;
        row.line.content += delta;
        row.line.end += delta;
        row.line.next += delta;
        for (MdTable::Cell &cell : row.cells) {
            cell.start += delta;
            cell.end += delta;
            cell.rawStart += delta;
            cell.rawEnd += delta;
        }
    }
}

void collectTables(const md::Node &node, QList<const md::Node *> &tables)
{
    if (node.type == md::NodeType::Table) {
        if (node.file == 0) tables.append(&node);
        return;
    }
    if (node.type == md::NodeType::Include) return;
    for (const md::Node &child : node.children) collectTables(child, tables);
}

template<typename Read>
void readCellInlines(const ParsedTextPtr &parsed, const MdTable::Cell &cell, const QString &markdown,
                     const md::Options &options, Read read)
{
    if (parsed && !markdown.isEmpty()) {
        const md::Document &document = parsed->document();
        for (const md::Node *node : document.pathAt(cell.start)) {
            if (node->type != md::NodeType::TableCell || node->range.start != cell.start || node->range.end != cell.end)
                continue;
            read(document, *node);
            return;
        }
    }
    const md::Document inlineCell = md::parseTableCell(markdown, options);
    read(inlineCell, inlineCell.root);
}

int breakBefore(QStringView text, int position)
{
    if (position >= 2 && text[position - 1] == u'\n' && text[position - 2] == u'\r') return 2;
    return position >= 1 && (text[position - 1] == u'\n' || text[position - 1] == u'\r') ? 1 : 0;
}

int breakAfter(QStringView text, int position)
{
    if (position + 1 < text.size() && text[position] == u'\r' && text[position + 1] == u'\n') return 2;
    return position < text.size() && (text[position] == u'\n' || text[position] == u'\r') ? 1 : 0;
}

// The spaces after a cell's text up to its closing pipe: the text may grow
// into them, as when a space is typed at its end.
int tailOf(QStringView text, const MdTable::Cell &cell)
{
    int tail = 0;
    while (cell.end + tail < cell.rawEnd && text[cell.end + tail] == u' ') ++tail;
    return tail;
}

bool isBreak(QChar c) { return c == u'\n' || c == u'\r'; }

// Whether an edit of [from, to) ends where a table starts and leaves its
// header at the start of a line: text above the table, its line breaks
// too, and none of the table's own.
bool endsBefore(QStringView text, int from, int to, const QString &replacement, int tableStart)
{
    if (to != tableStart || from >= to) return false;
    if (!replacement.isEmpty()) return isBreak(replacement.back());
    return from == 0 || isBreak(text[from - 1]);
}

} // namespace

// ---- TableGrid ------------------------------------------------------------------

TableGrid::TableGrid(TableGrids *owner) : QObject(owner), m_owner(owner) {}

TableGrid::~TableGrid() = default;

const MdTable::Cell &TableGrid::cellAt(Place place) const
{
    const int tableRow = place.row == 0 ? 0 : place.row + 1;
    return m_table.rows[tableRow].cells[place.cell];
}

int TableGrid::cellCount(int row) const
{
    const int tableRow = row == 0 ? 0 : row + 1;
    return tableRow < m_table.rows.size() ? int(m_table.rows[tableRow].cells.size()) : 0;
}

int TableGrid::indexOf(Place place) const { return m_rowFirst.value(place.row) + place.cell; }

TableGrid::Place TableGrid::placeOf(int position) const
{
    const int row = MdTable::rowAt(m_table, position);
    if (row < 0 || row == 1) return {};
    const int modelRow = MdTable::modelRowOf(row);
    const QList<MdTable::Cell> &cells = m_table.rows[row].cells;
    for (int i = 0; i < cells.size(); ++i) {
        if (position < cells[i].rawStart || position > cells[i].rawEnd) continue;
        // The cells a short row leaves out all stand at its end: the one the
        // caret was taken to is the one there.
        if (cells[i].missing && position == m_missingAt && m_missing.row == modelRow && m_missing.cell > i
            && m_missing.cell < cells.size() && cells[m_missing.cell].missing)
            return m_missing;
        return {modelRow, i};
    }
    return {};
}

void TableGrid::preferMissing(Place place, int position) const
{
    m_missing = place;
    m_missingAt = position;
}

QStringList TableGrid::rowTexts(int row) const
{
    QStringList texts;
    for (int i = 0; i < cellCount(row); ++i) {
        const int index = indexOf({row, i});
        texts.append(index >= 0 && size_t(index) < m_cells.size() ? m_cells[size_t(index)].text : QString());
    }
    return texts;
}

std::pair<int, int> TableGrid::lineRange(Place place, int line) const
{
    const MdTable::Cell &cell = cellAt(place);
    QTextLayout *layout = layoutOf(place);
    if (!layout || line < 0 || line >= layout->lineCount()) return {cell.start, cell.end};
    const QTextLine shown = layout->lineAt(line);
    const QString &text = m_cells[size_t(indexOf(place))].text;
    int end = shown.textStart() + shown.textLength();
    // A line that wraps ends before the space it wrapped at.
    if (line + 1 < layout->lineCount() && end > shown.textStart() && end <= text.size() && text[end - 1].isSpace()) --end;
    return {cell.start + qMin(int(text.size()), shown.textStart()), cell.start + qMin(int(text.size()), end)};
}

qreal TableGrid::padding() const { return std::round(m_owner->lineHeight() * 0.4); }

qreal TableGrid::lineHeight() const { return m_owner->lineHeight(); }

qreal TableGrid::viewWidth() const { return qMin(m_width, qMax<qreal>(1, m_owner->availableWidth())); }

void TableGrid::setScrollX(qreal x)
{
    x = std::round(qBound<qreal>(0, x, qMax<qreal>(0, m_width - viewWidth())));
    if (qFuzzyCompare(x + 1, m_scrollX + 1)) return;
    m_scrollX = x;
    emit scrollChanged();
    emit contentChanged();
}

void TableGrid::reveal(const QRectF &rect)
{
    const qreal room = viewWidth();
    if (m_width <= room) {
        setScrollX(0);
        return;
    }
    const qreal margin = padding();
    if (rect.left() - margin < m_scrollX) setScrollX(rect.left() - margin);
    else if (rect.right() + margin > m_scrollX + room) setScrollX(rect.right() + margin - room);
}

std::pair<qreal, qreal> TableGrid::extent(const MdTable::Cell &cell) const
{
    if (m_columnLeft.isEmpty()) return {0, 0};
    const int first = qMin(cell.column, int(m_columnLeft.size()) - 1);
    const int last = qMin(cell.column + cell.span - 1, int(m_columnLeft.size()) - 1);
    return {m_columnLeft[first], m_columnLeft[last] + m_columnWidth[last]};
}

QRectF TableGrid::cellRect(Place place) const
{
    if (!place.valid() || place.row >= m_rowTop.size()) return {};
    const auto [left, right] = extent(cellAt(place));
    return QRectF(left, m_rowTop[place.row], right - left, m_rowHeight[place.row]);
}

TableGrid::Place TableGrid::placeAt(QPointF point) const
{
    if (m_rowTop.isEmpty()) return {};
    int row = int(std::upper_bound(m_rowTop.cbegin(), m_rowTop.cend(), point.y()) - m_rowTop.cbegin()) - 1;
    row = qBound(0, row, int(m_rowTop.size()) - 1);
    int best = 0;
    for (int i = 0; i < cellCount(row); ++i) {
        const QRectF rect = cellRect({row, i});
        if (point.x() >= rect.left() - 1) best = i;
    }
    return {row, best};
}

QTextLayout *TableGrid::layoutOf(Place place) const
{
    const int index = indexOf(place);
    if (index < 0 || size_t(index) >= m_cells.size()) return nullptr;
    CellText &cell = m_cells[size_t(index)];
    const auto [left, right] = extent(cellAt(place));
    const qreal width = right - left - 2 * padding();
    if (cell.layout && qFuzzyCompare(cell.width, width)) return cell.layout.get();
    const MdTable::Cell &source = cellAt(place);
    const MdTable::Align align =
        source.column < m_table.aligns.size() ? m_table.aligns[source.column] : MdTable::Align::None;
    cell.layout = std::make_unique<QTextLayout>(cell.text, place.row == 0 ? m_owner->headerFont() : m_owner->font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setAlignment(align == MdTable::Align::Right    ? Qt::AlignRight
                        : align == MdTable::Align::Center ? Qt::AlignHCenter
                                                          : Qt::AlignLeft);
    cell.layout->setTextOption(option);
    cell.layout->setCacheEnabled(true);
    // Formatted as the text view formats the Markdown around it.
    QList<QTextLayout::FormatRange> formats;
    if (EditorDecorations *decorations = m_owner->decorations())
        formats = decorations->formatsFor(source.start, int(cell.text.size()));
    // What an input method is composing here shows where it goes, underlined.
    const QString preedit = m_owner->binding()->preedit();
    const int composing = m_owner->binding()->preeditAt();
    if (!preedit.isEmpty() && composing >= source.start && composing <= source.end + tailOf(m_owner->text(), source)) {
        const int at = qMin(composing - source.start, int(cell.text.size()));
        const int length = int(preedit.size());
        cell.layout->setPreeditArea(at, preedit);
        for (QTextLayout::FormatRange &range : formats) {
            if (range.start >= at) range.start += length;
            else if (range.start + range.length > at) range.length += length;
        }
        QTextCharFormat underline;
        underline.setFontUnderline(true);
        formats.append({at, length, underline});
    }
    cell.layout->setFormats(formats);
    const qreal lineHeight = m_owner->lineHeight();
    cell.layout->beginLayout();
    int lines = 0;
    for (QTextLine line = cell.layout->createLine(); line.isValid(); line = cell.layout->createLine()) {
        line.setLineWidth(qMax<qreal>(1, width));
        // The text sits in its line as the editor's does: its line's
        // ascent and descent centred in the line height.
        line.setPosition(QPointF(0, lines * lineHeight + (lineHeight - line.height()) / 2));
        ++lines;
    }
    cell.layout->endLayout();
    cell.width = width;
    cell.lines = qMax(1, lines);
    return cell.layout.get();
}

void TableGrid::restyle(int start, int end)
{
    if (end < m_table.start || start > m_table.end) return;
    bool any = false;
    // The rows the range reaches, found by where their lines are.
    const auto first = std::lower_bound(m_table.rows.cbegin(), m_table.rows.cend(), start,
                                        [](const MdTable::Row &row, int at) { return row.line.end < at; });
    for (auto at = first; at != m_table.rows.cend() && at->line.start <= end; ++at) {
        const int tableRow = int(at - m_table.rows.cbegin());
        if (tableRow == 1) continue;
        const int row = MdTable::modelRowOf(tableRow);
        for (int i = 0; i < cellCount(row); ++i) {
            const MdTable::Cell &cell = cellAt({row, i});
            if (cell.end < start || cell.start > end) continue;
            CellText &text = m_cells[size_t(indexOf({row, i}))];
            if (text.layout) {
                text.layout.reset();
                any = true;
            }
        }
    }
    if (any) emit contentChanged();
}

int TableGrid::lineCount(Place place) const
{
    QTextLayout *layout = layoutOf(place);
    return layout ? qMax(1, layout->lineCount()) : 1;
}

int TableGrid::lineOf(Place place, int position) const
{
    QTextLayout *layout = layoutOf(place);
    if (!layout || layout->lineCount() == 0) return 0;
    const int offset = qBound(0, position - cellAt(place).start, int(m_cells[size_t(indexOf(place))].text.size()));
    const QTextLine line = layout->lineForTextPosition(offset);
    return line.isValid() ? line.lineNumber() : layout->lineCount() - 1;
}

qreal TableGrid::xOf(Place place, int position) const
{
    QTextLayout *layout = layoutOf(place);
    const QRectF rect = cellRect(place);
    if (!layout || layout->lineCount() == 0) return rect.left() + padding();
    const MdTable::Cell &cell = cellAt(place);
    const int length = int(m_cells[size_t(indexOf(place))].text.size());
    const int offset = position - cell.start;
    const int inside = qBound(0, offset, length);
    const QTextLine line = layout->lineForTextPosition(inside);
    qreal x = (line.isValid() ? line : layout->lineAt(layout->lineCount() - 1)).cursorToX(inside);
    // Spaces typed after the text, which the table keeps only once more follows them.
    if (offset > length) x += (offset - length) * QFontMetricsF(layout->font()).horizontalAdvance(u' ');
    return rect.left() + padding() + x;
}

int TableGrid::positionAtLine(Place place, int lineNumber, qreal x) const
{
    QTextLayout *layout = layoutOf(place);
    const MdTable::Cell &cell = cellAt(place);
    if (!layout || layout->lineCount() == 0) return cell.start;
    const QTextLine line = layout->lineAt(qBound(0, lineNumber, layout->lineCount() - 1));
    const int offset = line.xToCursor(x - cellRect(place).left() - padding());
    return cell.start + qBound(0, offset, int(m_cells[size_t(indexOf(place))].text.size()));
}

int TableGrid::sourceAt(qreal x, qreal y) const
{
    const Place place = placeAt(QPointF(x, y));
    if (!place.valid()) return m_table.start;
    // A point in a cell a short row leaves out is in that one.
    if (cellAt(place).missing) preferMissing(place, cellAt(place).start);
    const QRectF rect = cellRect(place);
    const qreal lineHeight = m_owner->lineHeight();
    const int line = int(std::floor((y - rect.top() - padding() / 2) / lineHeight));
    return positionAtLine(place, qBound(0, line, lineCount(place) - 1), x);
}

int TableGrid::rowAt(qreal y) const
{
    if (m_rowTop.isEmpty()) return -1;
    const int row = int(std::upper_bound(m_rowTop.cbegin(), m_rowTop.cend(), y) - m_rowTop.cbegin()) - 1;
    return qBound(0, row, int(m_rowTop.size()) - 1);
}

int TableGrid::columnAt(qreal x) const
{
    if (m_columnLeft.isEmpty()) return -1;
    const int column = int(std::upper_bound(m_columnLeft.cbegin(), m_columnLeft.cend(), x) - m_columnLeft.cbegin()) - 1;
    return qBound(0, column, int(m_columnLeft.size()) - 1);
}

QRectF TableGrid::rowRect(int row) const
{
    if (row < 0 || row >= m_rowTop.size()) return {};
    return QRectF(0, m_rowTop[row], m_width, m_rowHeight[row]);
}

QRectF TableGrid::columnRect(int column) const
{
    if (column < 0 || column >= m_columnLeft.size()) return {};
    return QRectF(m_columnLeft[column], 0, m_columnWidth[column], m_height);
}

int TableGrid::caretRow() const
{
    const Place place = placeOf(m_owner->cursor());
    return place.valid() ? place.row : -1;
}

int TableGrid::caretColumn() const
{
    const Place place = placeOf(m_owner->cursor());
    return place.valid() ? cellAt(place).column : -1;
}

QRectF TableGrid::caretAt(int position) const
{
    const Place place = placeOf(position);
    if (!place.valid()) return {};
    const QRectF rect = cellRect(place);
    const qreal lineHeight = m_owner->lineHeight();
    const int line = lineOf(place, position);
    return QRectF(xOf(place, position), rect.top() + std::round(padding() / 2) + line * lineHeight, 0, lineHeight);
}

void TableGrid::read(const MdTable::Table &table, QStringView text)
{
    // Columns aligned anew are laid out anew.
    const bool realigned = m_table.aligns != table.aligns;
    m_table = table;
    m_missingAt = -1;
    std::vector<CellText> cells;
    // The header's cells come first, drawn in a font of their own.
    const size_t oldHeaderCells = m_rowFirst.size() > 1 ? size_t(m_rowFirst[1]) : m_cells.size();
    m_rowFirst.clear();
    for (int r = 0; r < table.rows.size(); ++r) {
        if (r == 1) continue;
        m_rowFirst.append(int(cells.size()));
        for (const MdTable::Cell &cell : table.rows[r].cells) {
            CellText item;
            item.text = text.mid(cell.start, cell.end - cell.start).toString();
            // A cell whose text is unchanged, in the same kind of row, keeps
            // its layout and its widths.
            const size_t previous = cells.size();
            if (previous < m_cells.size() && m_cells[previous].text == item.text
                && (r == 0) == (previous < oldHeaderCells)) {
                if (!realigned) {
                    item.layout = std::move(m_cells[previous].layout);
                    item.width = m_cells[previous].width;
                }
                item.lines = m_cells[previous].lines;
                item.natural = m_cells[previous].natural;
                item.word = m_cells[previous].word;
            }
            cells.push_back(std::move(item));
        }
    }
    m_cells = std::move(cells);
    measure();
    emit contentChanged();
}

void TableGrid::measure()
{
    const QFontMetricsF metrics(m_owner->font());
    const QFontMetricsF headerMetrics(m_owner->headerFont());
    const qreal rule = 1;
    const qreal pad = padding();
    const int columns = qMax(1, m_table.columns);
    const qreal least = metrics.horizontalAdvance(QStringLiteral("000"));
    // Widths measured in another font are measured again.
    const QString font = m_owner->font().toString();
    if (font != m_measuredFont) {
        for (CellText &cell : m_cells) cell.natural = cell.word = -1;
        m_measuredFont = font;
    }
    QList<qreal> natural(columns, least), minimal(columns, least);
    struct Spanning {
        int first, last;
        qreal natural, minimal;
    };
    QList<Spanning> spanning;
    for (int row = 0; row < m_rowFirst.size(); ++row) {
        const QFontMetricsF &m = row == 0 ? headerMetrics : metrics;
        for (int i = 0; i < cellCount(row); ++i) {
            const MdTable::Cell &cell = cellAt({row, i});
            if (cell.column >= columns) continue;
            CellText &measured = m_cells[size_t(indexOf({row, i}))];
            if (measured.natural < 0) {
                measured.natural = std::ceil(m.horizontalAdvance(measured.text));
                measured.word = 0;
                // Only a cell wider than a word needs its words measured.
                if (measured.text.contains(u' '))
                    for (const QStringView part : QStringView(measured.text).split(u' ', Qt::SkipEmptyParts))
                        measured.word = qMax(measured.word, std::ceil(m.horizontalAdvance(part.toString())));
                else
                    measured.word = measured.natural;
            }
            const qreal width = measured.natural;
            // A word longer than the column at its widest wraps anywhere.
            const qreal word = qMin(measured.word, 30 * least / 3);
            if (cell.span == 1) {
                natural[cell.column] = qMax(natural[cell.column], width);
                minimal[cell.column] = qMax(minimal[cell.column], word);
            } else {
                spanning.append({cell.column, qMin(columns, cell.column + cell.span) - 1, width, word});
            }
        }
    }
    // A merged cell takes its columns and the room between them; one wider
    // widens the last of them.
    const qreal between = 2 * pad + rule;
    for (const Spanning &span : std::as_const(spanning)) {
        qreal sumNatural = between * (span.last - span.first), sumMinimal = sumNatural;
        for (int c = span.first; c <= span.last; ++c) {
            sumNatural += natural[c];
            sumMinimal += minimal[c];
        }
        if (span.natural > sumNatural) natural[span.last] += span.natural - sumNatural;
        if (span.minimal > sumMinimal) minimal[span.last] += span.minimal - sumMinimal;
    }
    for (int c = 0; c < columns; ++c) natural[c] = qMax(natural[c], minimal[c]);
    // The columns take their natural widths where the room allows; with less
    // room, each gives up a share of what it has over its narrowest.
    const qreal frame = rule + columns * (between);
    const qreal room = qMax<qreal>(0, m_owner->availableWidth() - frame);
    qreal totalNatural = 0, totalMinimal = 0;
    for (int c = 0; c < columns; ++c) {
        totalNatural += natural[c];
        totalMinimal += minimal[c];
    }
    QList<qreal> widths = natural;
    if (totalNatural > room) {
        if (totalMinimal >= room) {
            widths = minimal;
        } else {
            // The widest columns give up room first: every column is held to
            // one width, its narrowest if that is more, the width that fills the room.
            const auto filled = [&](qreal cap) {
                qreal total = 0;
                for (int c = 0; c < columns; ++c) total += qMax(minimal[c], qMin(natural[c], cap));
                return total;
            };
            qreal low = 0, high = *std::max_element(natural.cbegin(), natural.cend());
            for (int i = 0; i < 40 && high - low > 0.5; ++i) {
                const qreal middle = (low + high) / 2;
                (filled(middle) > room ? high : low) = middle;
            }
            for (int c = 0; c < columns; ++c) widths[c] = std::floor(qMax(minimal[c], qMin(natural[c], low)));
        }
    }
    m_columnLeft.clear();
    m_columnWidth.clear();
    qreal x = rule;
    for (int c = 0; c < columns; ++c) {
        m_columnLeft.append(x);
        m_columnWidth.append(widths[c] + 2 * pad);
        x += widths[c] + 2 * pad + rule;
    }
    const qreal width = x;
    // Every row as tall as its tallest cell's lines.
    const qreal lineHeight = m_owner->lineHeight();
    m_rowTop.clear();
    m_rowHeight.clear();
    qreal y = rule;
    for (int row = 0; row < m_rowFirst.size(); ++row) {
        int lines = 1;
        for (int i = 0; i < cellCount(row); ++i) {
            const MdTable::Cell &cell = cellAt({row, i});
            if (cell.column >= columns) continue;
            CellText &text = m_cells[size_t(indexOf({row, i}))];
            const auto [left, right] = extent(cell);
            const qreal room = right - left - 2 * pad;
            if (text.natural <= room) {
                if (text.layout && !qFuzzyCompare(text.width, room)) text.layout.reset();
                continue;
            }
            lines = qMax(lines, lineCount({row, i}));
        }
        const qreal height = lines * lineHeight + std::round(pad / 2) * 2;
        m_rowTop.append(y);
        m_rowHeight.append(height);
        y += height + rule;
    }
    const bool changed = !qFuzzyCompare(width, m_width) || !qFuzzyCompare(y, m_height);
    m_width = width;
    m_height = y;
    if (changed) emit geometryChanged();
}

QList<TableGrid::Drawn> TableGrid::drawn(qreal top, qreal bottom)
{
    QList<Drawn> out;
    const int cursor = m_owner->cursor(), anchor = m_owner->anchor();
    const int from = qMin(cursor, anchor), to = qMax(cursor, anchor);
    const bool whole = from <= m_table.start && to >= m_table.end && from < to;
    int r0 = -1, r1 = -1, c0 = -1, c1 = -1;
    const bool block = m_owner->block(this, &r0, &r1, &c0, &c1);
    for (int row = 0; row < m_rowTop.size(); ++row) {
        if (m_rowTop[row] + m_rowHeight[row] < top) continue;
        if (m_rowTop[row] > bottom) break;
        for (int i = 0; i < cellCount(row); ++i) {
            const MdTable::Cell &cell = cellAt({row, i});
            if (cell.column >= m_columnLeft.size()) continue;
            Drawn d;
            d.rect = cellRect({row, i});
            d.layout = layoutOf({row, i});
            d.header = row == 0;
            if (EditorDecorations *decorations = m_owner->decorations())
                for (const EditorDecorations::Wave &wave : decorations->wavesFor(cell.start, cell.end - cell.start))
                    d.waves.append({wave.start - cell.start, wave.end - cell.start, wave.color});
            if (whole || (block && row >= r0 && row <= r1 && cell.column + cell.span - 1 >= c0 && cell.column <= c1))
                d.selected = true;
            else if (!block && from < to && from >= cell.rawStart && to <= cell.rawEnd) {
                d.selectionStart = qMax(0, from - cell.start);
                d.selectionLength = qMin(to, cell.end) - qMax(from, cell.start);
            }
            out.append(d);
        }
    }
    return out;
}

void TableGrid::updateCaret()
{
    const int cursor = m_owner->cursor();
    const bool has = cursor >= m_table.start && cursor <= m_table.end && placeOf(cursor).valid();
    QRectF caret, block;
    if (has) {
        caret = caretAt(cursor);
        const Place place = placeOf(cursor);
        const QRectF cell = cellRect(place);
        const qreal next = xOf(place, cursor + 1);
        const qreal width = next > caret.x() && next <= cell.right() ? next - caret.x()
                                                                     : QFontMetricsF(m_owner->font()).horizontalAdvance(u'0');
        block = QRectF(caret.x(), caret.y(), width, caret.height());
    }
    if (has) reveal(block);
    else setScrollX(qMin(m_scrollX, qMax<qreal>(0, m_width - viewWidth())));
    if (has == m_hasCaret && caret == m_caretRect && block == m_blockRect) return;
    m_hasCaret = has;
    m_caretRect = caret;
    m_blockRect = block;
    emit caretChanged();
}

// ---- TableGrids -----------------------------------------------------------------

TableGrids::TableGrids(EditorBinding *binding) : QObject(binding), m_binding(binding) {}

TableGrids::~TableGrids() = default;

EditorDecorations *TableGrids::decorations() const { return static_cast<EditorDecorations *>(m_decorations.data()); }

void TableGrids::setDecorations(EditorDecorations *decorations)
{
    if (decorations == m_decorations) return;
    if (m_decorations) disconnect(m_decorations, nullptr, this, nullptr);
    m_decorations = decorations;
    if (!decorations) return;
    // Cells whose formatting changed are laid out again as they are next drawn.
    connect(decorations, &EditorDecorations::formatsChanged, this, [this](int start, int end) {
        for (TableGrid *grid : std::as_const(m_grids)) {
            if (end < grid->start() || start > grid->end()) continue;
            grid->restyle(start, end);
        }
    });
}

void TableGrids::setEnabled(bool on)
{
    if (on == m_enabled) return;
    m_enabled = on;
    emit enabledChanged();
    if (!m_binding->m_loaded) return;
    // The grids give their lines back to the text before going.
    for (TableGrid *grid : std::as_const(m_grids)) unproject(grid->start(), grid->end());
    clear();
    if (on) loaded();
}

QList<QObject *> TableGrids::gridObjects() const
{
    QList<QObject *> objects;
    for (TableGrid *grid : m_grids) objects.append(grid);
    return objects;
}

QFont TableGrids::font() const
{
    return m_binding->m_editor ? m_binding->m_editor->property("font").value<QFont>() : QFont();
}

QFont TableGrids::headerFont() const
{
    QFont header = font();
    header.setBold(true);
    return header;
}

qreal TableGrids::lineHeight() const { return qMax(1, m_binding->m_appliedLineHeight); }

qreal TableGrids::availableWidth() const
{
    QQuickItem *editor = m_binding->m_editor;
    if (!editor) return 0;
    // The column, and the margin to its right but for a gap as wide as the pane's.
    return editor->width() + qMax<qreal>(0, editor->x() - 24);
}

MdTable::Dialect TableGrids::dialectOf(const md::Options &options)
{
    MdTable::Dialect dialect;
    dialect.protectCode = options.profile == md::Profile::Writer;
    dialect.mergedCells = options.profile == md::Profile::Writer && options.mergedCells;
    dialect.lineComments = options.lineComments;
    return dialect;
}

MdTable::Dialect TableGrids::dialect() const
{
    const MarkdownEditing::Options options = m_binding->editingOptions();
    MdTable::Dialect dialect;
    dialect.protectCode = options.protectCodePipes;
    dialect.mergedCells = options.mergedCells;
    dialect.lineComments = options.lineComments;
    return dialect;
}

QTextDocument *TableGrids::document() const { return m_binding->m_textDocument; }

QString TableGrids::text() const { return m_binding->m_buffer ? m_binding->m_buffer->text() : QString(); }

int TableGrids::cursor() const { return m_binding->m_cursorSource; }

int TableGrids::anchor() const { return m_binding->m_anchorSource; }

void TableGrids::select(int anchor, int cursor)
{
    const int length = m_binding->m_buffer->length();
    m_binding->selectEditor(m_binding->m_pairs.toEditor(qBound(0, anchor, length)),
                            m_binding->m_pairs.toEditor(qBound(0, cursor, length)));
    m_binding->refreshCursor();
    m_binding->revealCursor();
}

TableGrid *TableGrids::gridAt(int position) const
{
    for (TableGrid *grid : m_grids)
        if (position >= grid->start() && position <= grid->end()) return grid;
    return nullptr;
}

std::optional<QRectF> TableGrids::rectangleFor(int position) const
{
    TableGrid *grid = gridAt(position);
    if (!grid) return std::nullopt;
    QRectF caret = grid->caretAt(position);
    if (caret.isNull()) {
        // Outside the cells, as in the pipes or at a row's end: the row's place.
        const int row = MdTable::rowAt(grid->table(), position);
        const QRectF rect = grid->rowRect(row <= 1 ? 0 : row - 1);
        caret = QRectF(0, rect.top() + std::round(grid->padding() / 2), 0, lineHeight());
    }
    return caret.translated(grid->x() - grid->scrollX(), grid->y());
}

int TableGrids::sourceAt(QPointF point) const
{
    for (TableGrid *grid : m_grids)
        if (point.y() >= grid->y() && point.y() < grid->y() + grid->height())
            return grid->sourceAt(point.x() - grid->x() + grid->scrollX(), point.y() - grid->y());
    return -1;
}

TableGrids::Spot TableGrids::spotOf(int position, SourceRange range) const
{
    Spot spot;
    TableGrid *grid = gridAt(position);
    if (!grid) return spot;
    const TableGrid::Place place = grid->placeOf(position);
    if (!place.valid()) return spot;
    spot.grid = grid;
    spot.place = place;
    spot.offset = position - grid->cellAt(place).start;
    const MdTable::Line &line = grid->table().rows[MdTable::lineOf(place.row)].line;
    spot.lineStart = line.start;
    spot.lineEnd = line.end;
    spot.rows = grid->rowCount();
    spot.row = grid->rowTexts(place.row);
    // Rows like it above it, where the edit may write its row anew.
    if (range.start >= 0 && spot.lineEnd >= range.start && spot.lineStart <= range.start + range.length)
        for (int row = 1; row < place.row; ++row) spot.likeBefore += grid->rowIs(row, spot.row);
    return spot;
}

bool TableGrid::rowIs(int row, const QStringList &texts) const
{
    if (cellCount(row) != texts.size()) return false;
    for (int i = 0; i < texts.size(); ++i) {
        const int index = indexOf({row, i});
        if (index < 0 || size_t(index) >= m_cells.size() || m_cells[size_t(index)].text != texts[i]) return false;
    }
    return true;
}

TableGrids::CellTexts TableGrids::cellTexts(SourceRange range) const
{
    CellTexts texts;
    TableGrid *grid = gridAt(range.start);
    if (!grid || range.start + range.length > grid->end()) return texts;
    texts.grid = grid;
    for (int row = 0; row < grid->rowCount(); ++row) texts.rows.append(grid->rowTexts(row));
    return texts;
}

std::optional<int> TableGrids::changeEnd(const CellTexts &before) const
{
    const TableGrid *grid = before.grid;
    if (!grid || !m_grids.contains(before.grid.data()) || grid->rowCount() == 0) return std::nullopt;
    const int rows = grid->rowCount();
    for (int row = 0; row < qMax(rows, int(before.rows.size())); ++row) {
        const QStringList now = row < rows ? grid->rowTexts(row) : QStringList();
        const QStringList was = before.rows.value(row);
        if (now == was) continue;
        // Rows gone from the end: the last row there is.
        if (row >= rows) return grid->cellAt({rows - 1, 0}).start;
        int i = 0;
        while (i < now.size() && i < was.size() && now[i] == was[i]) ++i;
        // Cells gone from the row's end: its last.
        i = qMin(i, int(now.size()) - 1);
        if (i < 0) return std::nullopt;
        const QString &text = now[i];
        const QString old = was.value(i);
        int prefix = 0;
        while (prefix < text.size() && prefix < old.size() && text[prefix] == old[prefix]) ++prefix;
        int suffix = 0;
        while (suffix < text.size() - prefix && suffix < old.size() - prefix
               && text[text.size() - 1 - suffix] == old[old.size() - 1 - suffix])
            ++suffix;
        return grid->cellAt({row, i}).start + int(text.size()) - suffix;
    }
    return std::nullopt;
}

bool TableGrids::parsedTableAt(const Document *document, int position)
{
    if (!document) return false;
    const ParsedTextPtr parse = document->parse()->current();
    if (!parse) return false;
    QList<const md::Node *> tables;
    collectTables(parse->document().root, tables);
    return std::any_of(tables.cbegin(), tables.cend(), [&](const md::Node *table) {
        return position >= table->range.start && position <= table->range.end;
    });
}

bool TableGrids::inTableCell(QStringView text, SourceRange range, const MdTable::Dialect &dialect)
{
    const auto table = MdTable::locate(text, range.start, dialect);
    if (!table || table->unfit != MdTable::Unfit::None) return false;
    const int row = MdTable::rowAt(*table, range.start);
    if (row < 0 || row == 1) return false;
    const int cell = MdTable::cellAt(*table, row, range.start);
    if (cell < 0 || cell >= table->rows[row].cells.size()) return false;
    const MdTable::Cell &in = table->rows[row].cells[cell];
    return !in.missing && range.start >= in.start && range.start + range.length <= in.end;
}

std::optional<int> TableGrids::positionOf(const Spot &spot, SourceRange range, int inserted) const
{
    if (!spot.grid || !m_grids.contains(spot.grid.data())) return std::nullopt;
    const TableGrid *grid = spot.grid;
    TableGrid::Place place = spot.place;
    const int rows = grid->rowCount();
    const int oldEnd = range.start + range.length;
    Q_UNUSED(inserted);
    if (spot.lineStart >= oldEnd) {
        // After the edit: by as many rows as came or went before it.
        place.row += rows - spot.rows;
    } else if (spot.lineEnd >= range.start && rows != spot.rows && place.row > 0) {
        // Rows came or went where the edit wrote rows around it anew: the
        // row with its cells, as many like it above it as before.
        for (int row = 1, like = 0; row < rows; ++row) {
            if (!grid->rowIs(row, spot.row)) continue;
            place.row = row;
            if (like++ == spot.likeBefore) break;
        }
    }
    if (place.row < 0 || place.row >= rows || place.cell >= grid->cellCount(place.row)) return std::nullopt;
    const MdTable::Cell &cell = grid->cellAt(place);
    return cell.start + qBound(0, spot.offset, cell.end - cell.start);
}

bool TableGrids::inCellText(int start, int end) const
{
    for (const TableGrid *grid : m_grids) {
        if (end < grid->start() || start > grid->end()) continue;
        const TableGrid::Place place = grid->placeOf(start);
        if (!place.valid() || !(place == grid->placeOf(end))) return false;
        const MdTable::Cell &cell = grid->cellAt(place);
        return start >= cell.start && end <= cell.end;
    }
    return true;
}

bool TableGrids::settled() const
{
    if (!m_enabled || !m_binding->m_loaded || !m_binding->m_document) return true;
    const ParsedTextPtr parse = m_binding->m_document->parse()->current();
    return parse && parse->revision == m_binding->m_buffer->revision();
}

void TableGrids::setProblem(const QString &problem)
{
    if (problem == m_problem) return;
    m_problem = problem;
    emit problemChanged();
}

void TableGrids::clear()
{
    disconnect(m_parseConnection);
    m_rawTables.clear();
    m_waitingTable.reset();
    m_localRawEdit.reset();
    m_markdown.clear();
    m_tickEscapes = {};
    // The view lets go of the grids before they go.
    const QList<TableGrid *> gone = std::exchange(m_grids, {});
    if (std::exchange(m_caretGrid, nullptr)) emit caretChanged();
    if (!gone.isEmpty()) emit gridsChanged();
    for (TableGrid *grid : gone) grid->deleteLater();
    updateShowGrid();
}

void TableGrids::loaded()
{
    clear();
    if (!m_enabled || !m_binding->m_document || !m_binding->m_loaded) return;
    DocumentParse *parse = m_binding->m_document->parse();
    m_parseConnection = connect(parse, &DocumentParse::parsed, this, &TableGrids::parsed);
    if (parse->request()) parsed();
}

void TableGrids::parsed()
{
    if (!m_enabled || !m_binding->m_loaded || !m_binding->m_document) return;
    const ParsedTextPtr parse = m_binding->m_document->parse()->current();
    if (!parse || parse->revision != m_binding->m_buffer->revision()) return;
    const int caret = cursor();
    const bool waiting = m_waitingTable && m_waitingTable->contains(caret);
    const auto localEdit = std::exchange(m_localRawEdit, std::nullopt);
    m_rawTables.clear();
    if (!m_binding->m_document->markdownOptions().tables) {
        m_waitingTable.reset();
        adopt({});
        return;
    }
    QList<const md::Node *> nodes;
    collectTables(parse->document().root, nodes);
    const QString source = text();
    const MdTable::Dialect dialect = this->dialect();
    QList<MdTable::Table> tables;
    for (const md::Node *node : std::as_const(nodes)) {
        const auto table = MdTable::scan(source, node->range.start, node->range.end, dialect);
        if (!table || table->unfit != MdTable::Unfit::None) continue;
        int rows = 0;
        for (const md::Node &row : node->children) rows += row.type == md::NodeType::TableRow;
        if (rows != table->rows.size() - 1 || node->columns != table->columns) continue;
        if (m_markdown.contains(table->start)) {
            m_rawTables.append({table->start, table->end, false});
            continue;
        }
        // A table being written in this view waits while the caret is in it.
        const RawTable raw{table->start, table->end, true};
        const bool writing = localEdit && localEdit->start <= raw.end && localEdit->start + localEdit->length >= raw.start;
        if (raw.contains(caret) && (waiting || writing) && !gridAt(raw.start)) {
            m_waitingTable = raw;
            m_rawTables.append(raw);
            continue;
        }
        tables.append(*table);
    }
    adopt(tables);
}

namespace {

bool sameCells(const MdTable::Table &one, const MdTable::Table &other)
{
    if (one.rows.size() != other.rows.size() || one.aligns != other.aligns) return false;
    for (qsizetype r = 0; r < one.rows.size(); ++r) {
        const QList<MdTable::Cell> &a = one.rows[r].cells, &b = other.rows[r].cells;
        if (a.size() != b.size()) return false;
        for (qsizetype i = 0; i < a.size(); ++i)
            if (a[i].column != b[i].column || a[i].span != b[i].span || a[i].start != b[i].start || a[i].end != b[i].end
                || a[i].missing != b[i].missing)
                return false;
    }
    return true;
}

} // namespace

void TableGrids::adopt(const QList<MdTable::Table> &tables)
{
    const QString source = text();
    QList<TableGrid *> kept;
    bool changed = false;
    for (const MdTable::Table &table : tables) {
        auto existing = std::find_if(m_grids.begin(), m_grids.end(), [&](TableGrid *grid) {
            return grid->start() == table.start && grid->end() == table.end;
        });
        if (existing != m_grids.end()) {
            // The same lines read as other cells, as after a change of
            // dialect, are read again.
            if (!sameCells((*existing)->table(), table)) {
                (*existing)->read(table, source);
                changed = true;
            }
            kept.append(*existing);
            m_grids.erase(existing);
            continue;
        }
        auto *grid = new TableGrid(this);
        grid->read(table, source);
        kept.append(grid);
        changed = true;
    }
    // Tables no longer read as tables show their Markdown again.
    for (TableGrid *gone : std::as_const(m_grids)) {
        unproject(gone->start(), gone->end());
        if (m_caretGrid == gone) m_caretGrid = nullptr;
        gone->deleteLater();
        changed = true;
    }
    m_grids = kept;
    std::sort(m_grids.begin(), m_grids.end(), [](TableGrid *a, TableGrid *b) { return a->start() < b->start(); });
    for (TableGrid *grid : std::as_const(m_grids)) project(grid);
    placeGrids();
    if (changed) emit gridsChanged();
    cursorMoved();
    if (changed) m_binding->queueTableLayout(false);
}

void TableGrids::project(TableGrid *grid)
{
    QTextDocument *doc = document();
    if (!doc) return;
    Projecting projecting(m_binding->m_projecting);
    const CrlfPairs &pairs = m_binding->m_pairs;
    const int first = pairs.toEditor(grid->start());
    const int last = pairs.toEditor(grid->end());
    QTextBlock header = doc->findBlock(first);
    if (!header.isValid()) return;
    QTextBlockFormat format = m_binding->lineFormat();
    format.setLineHeight(std::ceil(grid->height()), QTextBlockFormat::FixedHeight);
    format.setNonBreakableLines(true);
    if (header.blockFormat() != format) {
        QTextCursor cursor(header);
        cursor.setBlockFormat(format);
    }
    bool hid = false;
    for (QTextBlock block = header.next(); block.isValid() && block.position() <= last; block = block.next()) {
        if (!block.isVisible()) continue;
        block.setVisible(false);
        hid = true;
    }
    if (hid) doc->markContentsDirty(first, last - first + 1);
    // A hidden line keeps the place it was last laid out at; it is moved
    // far away, so that nothing finds it where text is now.
    for (QTextBlock block = header.next(); block.isValid() && block.position() <= last; block = block.next()) {
        if (QTextLayout *layout = block.layout()) {
            if (layout->lineCount() > 0) layout->clearLayout();
            layout->setPosition(QPointF(0, -1e7));
        }
    }
}

void TableGrids::unproject(int start, int end)
{
    QTextDocument *doc = document();
    if (!doc) return;
    Projecting projecting(m_binding->m_projecting);
    const CrlfPairs &pairs = m_binding->m_pairs;
    const int length = m_binding->m_buffer->length();
    const int first = pairs.toEditor(qBound(0, start, length));
    const int last = pairs.toEditor(qBound(0, end, length));
    const QTextBlockFormat format = m_binding->lineFormat();
    QTextCursor cursor(doc);
    cursor.setPosition(first);
    cursor.setPosition(last, QTextCursor::KeepAnchor);
    cursor.setBlockFormat(format);
    for (QTextBlock block = doc->findBlock(first); block.isValid() && block.position() <= last; block = block.next())
        block.setVisible(true);
    doc->markContentsDirty(first, last - first + 1);
    // Its rows were not formatted while the grid hid them.
    if (EditorDecorations *decorations = this->decorations()) decorations->reshown(start, end);
}

void TableGrids::placeGrids()
{
    QTextDocument *doc = document();
    if (!doc) return;
    for (TableGrid *grid : std::as_const(m_grids)) {
        const QTextBlock header = doc->findBlock(m_binding->m_pairs.toEditor(grid->start()));
        const qreal top = header.isValid() ? doc->documentLayout()->blockBoundingRect(header).top() : 0;
        if (!qFuzzyCompare(top + 1, grid->m_top + 1)) {
            grid->m_top = top;
            emit grid->geometryChanged();
        }
    }
}

void TableGrids::sourceChanged(SourceRange range, int inserted, bool localRawEdit, bool localUndo)
{
    const int delta = inserted - range.length;
    const int oldEnd = range.start + range.length;
    const int newEnd = range.start + inserted;
    if (m_tickEscapes.grid && range.start <= m_tickEscapes.grid->end() && oldEnd >= m_tickEscapes.grid->start())
        m_tickEscapes = {};
    // Undo and redo continue an edit to Markdown, including a table whose
    // syntax it temporarily removed.
    const auto touches = [&](int start, int end) { return range.start <= end && oldEnd >= start; };
    const bool replayingRaw = localUndo && ((m_waitingTable && touches(m_waitingTable->start, m_waitingTable->end))
                                          || (m_localRawEdit && touches(m_localRawEdit->start,
                                                                       m_localRawEdit->start + m_localRawEdit->length)));
    // Move the writing range and any edit the parser has yet to see along
    // with the changed text.
    const auto follow = [&](int &start, int &end) {
        if (oldEnd < start) {
            start += delta;
            end += delta;
        } else if (range.start <= end) {
            start = qMin(start, range.start);
            end = qMax(end + delta, newEnd);
        }
    };
    if (m_waitingTable) {
        follow(m_waitingTable->start, m_waitingTable->end);
        if (!localRawEdit && !replayingRaw && m_waitingTable->start == m_waitingTable->end) m_waitingTable.reset();
    }
    if (m_enabled && (localRawEdit || replayingRaw)) {
        m_localRawEdit.emplace(SourceRange{range.start, inserted});
    } else if (m_localRawEdit) {
        auto &[start, length] = *m_localRawEdit;
        int end = start + length;
        follow(start, end);
        length = end - start;
    }
    // The next parse checks which tables can be restored as grids.
    m_rawTables.clear();
    updateShowGrid();
    for (int &start : m_markdown)
        if (oldEnd <= start) start += delta;
    if (m_grids.isEmpty()) return;
    const QString source = text();
    bool removed = false;
    for (qsizetype i = 0; i < m_grids.size(); ++i) {
        TableGrid *grid = m_grids[i];
        // Lines put in just before the table, or taken away from just
        // before it, move it, as text before it does.
        const bool linesBefore = oldEnd == grid->start() && (range.length > 0 || inserted > 0)
                                 && (newEnd == 0 || isBreak(source[newEnd - 1]));
        if (oldEnd < grid->start() || linesBefore) {
            shiftTable(grid->m_table, delta);
            // A line break taken from before the header joined its line to
            // the line above, whose format the joined line now has.
            if (linesBefore && range.length > 0) project(grid);
            continue;
        }
        if (range.start > grid->end()) continue;
        // The edit reached into the table: it is read again, and shows as
        // Markdown when it is a table no longer.
        std::optional<MdTable::Table> table;
        if (range.start >= grid->start() && oldEnd <= grid->end()) {
            table = MdTable::scan(source, grid->start(), grid->end() + delta, dialect());
            if (table && (table->start != grid->start() || table->end != grid->end() + delta
                          || table->unfit != MdTable::Unfit::None))
                table.reset();
        }
        if (table) {
            grid->read(*table, source);
            project(grid);
            continue;
        }
        // Replacing an entire grid is an edit to the grid as a whole.
        if (localRawEdit && range.start <= grid->start() && oldEnd >= grid->end()) m_localRawEdit.reset();
        unproject(qMin(range.start, grid->start()), qMax(range.start + inserted, grid->end() + delta));
        if (m_caretGrid == grid) m_caretGrid = nullptr;
        grid->deleteLater();
        m_grids.removeAt(i--);
        removed = true;
    }
    placeGrids();
    if (removed) emit gridsChanged();
}

void TableGrids::relayout()
{
    if (m_grids.isEmpty()) return;
    for (TableGrid *grid : std::as_const(m_grids)) {
        for (TableGrid::CellText &cell : grid->m_cells) cell.layout.reset();
        grid->measure();
        project(grid);
    }
    placeGrids();
    for (TableGrid *grid : std::as_const(m_grids)) {
        grid->updateCaret();
        emit grid->contentChanged();
    }
}

void TableGrids::relayoutLater()
{
    QMetaObject::invokeMethod(this, [this] {
        if (m_binding->m_loaded) relayout();
    }, Qt::QueuedConnection);
}

int TableGrids::settle(const TableGrid *grid, int position, int direction) const
{
    const MdTable::Table &table = grid->table();
    const int row = MdTable::rowAt(table, position);
    if (row < 0) return position;
    const QString source = text();
    if (row == 1) {
        // The delimiter row holds no text: the row above or below, at the
        // same column of its cells.
        const int column = MdTable::cellAt(table, 0, table.rows[0].line.start + (position - table.rows[1].line.start));
        const int target = direction >= 0 && table.rows.size() > 2 ? 2 : 0;
        const MdTable::Row &to = table.rows[target];
        const int index = qMin(column, int(to.cells.size()) - 1);
        return direction >= 0 ? to.cells[index].start : to.cells[index].start;
    }
    const QList<MdTable::Cell> &cells = table.rows[row].cells;
    for (int i = 0; i < cells.size(); ++i) {
        const MdTable::Cell &cell = cells[i];
        if (position > cell.rawEnd) continue;
        // A step is one character, or a move onto the cell's closing pipe, as Vim's word motions make.
        const bool fromInside = m_stepping && m_lastCursor >= cell.start && m_lastCursor <= cell.end
                                && (position - m_lastCursor == 1 || position >= cell.rawEnd);
        if (position < cell.start) {
            // A step back from the start of a cell's text goes on to the end of the cell before it.
            if (direction < 0 && m_stepping && m_lastCursor == cell.start && m_lastCursor - position == 1 && i > 0)
                return cells[i - 1].end;
            return cell.start;
        }
        if (position > cell.end) {
            // Past its text, the caret rests only where typing put it, after spaces typed there.
            if (position == m_binding->m_placedCaret) return qMin(position, cell.end + tailOf(source, cell));
            // A step forward out of a cell's text goes on to the next cell.
            if (direction > 0 && fromInside && i + 1 < cells.size()) return cells[i + 1].start;
            return cell.end;
        }
        return position;
    }
    return cells.last().end;
}

void TableGrids::cursorMoved()
{
    if (!m_binding->m_loaded) return;
    const int cursor = this->cursor(), anchor = this->anchor();
    if (m_picked.anchor != anchor || m_picked.cursor != cursor) m_picked = {};
    // A table being written becomes a grid once the caret leaves it.
    if (!m_settling && m_waitingTable && !m_waitingTable->contains(cursor)) {
        m_waitingTable.reset();
        m_rawTables.removeIf([](const RawTable &table) { return table.waiting; });
        m_localRawEdit.reset();
        if (m_binding->m_document) m_binding->m_document->parse()->request();
        QMetaObject::invokeMethod(this, &TableGrids::parsed, Qt::QueuedConnection);
    }
    updateShowGrid();
    TableGrid *grid = gridAt(cursor);
    if (grid && !m_settling && anchor == cursor) {
        const int direction = cursor >= m_lastCursor ? 1 : -1;
        const int settled = settle(grid, cursor, direction);
        if (settled != cursor) {
            m_settling = true;
            select(settled, settled);
            m_settling = false;
            return;
        }
    }
    if (!m_settling && anchor != cursor) {
        // A selection that crosses a table's edge, from the text around it
        // into it or from a cell out of it, takes the whole table; one from
        // above that stops where the table starts, as whole lines selected
        // above it do, stays above it.
        const bool forward = anchor < cursor;
        const int low = qMin(anchor, cursor), high = qMax(anchor, cursor);
        const TableGrid *first = forward ? gridAt(anchor) : grid;
        const TableGrid *last = forward ? grid : gridAt(anchor);
        if (last && high == last->start()) last = nullptr;
        if (first != last) {
            const int start = first ? first->start() : low;
            const int end = last ? last->end() : high;
            const int a = forward ? start : end, c = forward ? end : start;
            if (a != anchor || c != cursor) {
                m_settling = true;
                select(a, c);
                m_settling = false;
                return;
            }
        }
    }
    m_lastCursor = cursor;
    // A vertical move's x holds only while the caret stays where that move left it.
    if (cursor != m_goalAt) m_goalX = -1;
    if (grid != m_caretGrid) {
        m_caretGrid = grid;
        emit caretChanged();
    }
    for (TableGrid *g : std::as_const(m_grids)) {
        g->updateCaret();
        emit g->contentChanged();
    }
}

bool TableGrids::moveVertically(TableGrid *grid, int position, int lines, bool extend)
{
    const TableGrid::Place place = grid->placeOf(position);
    if (!place.valid()) return false;
    if (m_goalX < 0) m_goalX = grid->xOf(place, position);
    const int line = grid->lineOf(place, position) + lines;
    int target = -1;
    if (line >= 0 && line < grid->lineCount(place)) {
        target = grid->positionAtLine(place, line, m_goalX);
    } else {
        // The cell above or below, whose column covers the goal.
        const int row = place.row + (lines > 0 ? 1 : -1);
        if (row < 0 || row >= grid->rowCount()) {
            const QString source = text();
            if (row < 0) target = grid->start() - breakBefore(source, grid->start());
            else target = grid->end() + breakAfter(source, grid->end());
            if (target < 0) target = grid->start();
        } else {
            TableGrid::Place to{row, 0};
            for (int i = 0; i < grid->cellCount(row); ++i) {
                const QRectF rect = grid->cellRect({row, i});
                if (rect.isNull()) continue;
                if (m_goalX >= rect.left() - 1) to.cell = i;
            }
            target = grid->positionAtLine(to, lines > 0 ? 0 : grid->lineCount(to) - 1, m_goalX);
        }
    }
    const qreal goal = m_goalX;
    select(extend ? anchor() : target, target);
    m_goalX = goal;
    m_goalAt = cursor();
    return true;
}

bool TableGrids::enterFromProse(int position, bool down)
{
    // From the line above a grid, Down enters its first row; from the line
    // below it, Up enters its last row.
    QTextDocument *doc = document();
    if (!doc) return false;
    const QTextBlock block = doc->findBlock(m_binding->m_pairs.toEditor(position));
    if (!block.isValid() || !block.layout() || block.layout()->lineCount() == 0) return false;
    const int inBlock = m_binding->m_pairs.toEditor(position) - block.position();
    const QTextLine line = block.layout()->lineForTextPosition(inBlock);
    if (!line.isValid()) return false;
    if (down && line.lineNumber() != block.layout()->lineCount() - 1) return false;
    if (!down && line.lineNumber() != 0) return false;
    QTextBlock next = down ? block.next() : block.previous();
    while (next.isValid() && !next.isVisible()) next = down ? next.next() : next.previous();
    if (!next.isValid()) return false;
    const int at = m_binding->m_pairs.toSource(next.position());
    TableGrid *grid = gridAt(at);
    if (!grid) return false;
    const qreal x = line.cursorToX(inBlock);
    const int row = down ? 0 : grid->rowCount() - 1;
    TableGrid::Place to{row, 0};
    for (int i = 0; i < grid->cellCount(row); ++i)
        if (x >= grid->cellRect({row, i}).left() - 1) to.cell = i;
    const int target = grid->positionAtLine(to, down ? 0 : grid->lineCount(to) - 1, x);
    select(target, target);
    m_goalX = x;
    m_goalAt = cursor();
    return true;
}

bool TableGrids::run(const Adapted &change)
{
    if (change.kind == Adapted::Refused) {
        emit refused(change.reason);
        return false;
    }
    if (change.kind != Adapted::Changed || !m_binding->editable() || m_binding->m_blocked) return false;
    if (!change.edits.isEmpty()
        && m_binding->m_buffer->replace(change.edits) == SourceBuffer::EditOutcome::Rejected)
        return false;
    select(change.anchor, change.cursor);
    return true;
}

bool TableGrids::key(QKeyEvent *event)
{
    if (m_grids.isEmpty()) return false;
    const int cursor = this->cursor();
    TableGrid *grid = gridAt(cursor);
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool plain = (mods & ~Qt::ShiftModifier) == Qt::NoModifier;
    const bool control = (mods & ~Qt::ShiftModifier) == Qt::ControlModifier;
    const int key = event->key();
    const QString source = text();
    if (!grid) {
        if (plain && !shift && (key == Qt::Key_Down || key == Qt::Key_Up)) return enterFromProse(cursor, key == Qt::Key_Down);
        // Backspace on the blank line after a table, or Delete at the end of
        // the line before one, selects it; again, it goes.
        if (mods == Qt::NoModifier && (key == Qt::Key_Backspace || key == Qt::Key_Delete) && cursor == anchor()) {
            for (TableGrid *g : std::as_const(m_grids)) {
                const bool after = key == Qt::Key_Backspace && cursor == lineAfter(g);
                const bool before = key == Qt::Key_Delete && cursor == g->start() - breakBefore(source, g->start());
                if (!after && !before) continue;
                select(g->start(), g->end() + breakAfter(source, g->end()));
                return true;
            }
        }
        return false;
    }
    if (key != Qt::Key_Up && key != Qt::Key_Down && key != Qt::Key_PageUp && key != Qt::Key_PageDown) m_goalX = -1;
    const TableGrid::Place place = grid->placeOf(cursor);
    if (!place.valid()) return false;
    const MdTable::Cell &cell = grid->cellAt(place);
    int r0 = 0, r1 = 0, c0 = 0, c1 = 0;
    const bool inBlock = block(grid, &r0, &r1, &c0, &c1);
    // A move with Shift extends the selection; without, it collapses it at the new place.
    const auto moveTo = [&](int target) { select(shift ? this->anchor() : target, target); };
    const auto previousCell = [&](TableGrid::Place p) -> TableGrid::Place {
        if (p.cell > 0) return {p.row, p.cell - 1};
        if (p.row > 0) return {p.row - 1, grid->cellCount(p.row - 1) - 1};
        return {};
    };
    const auto nextCell = [&](TableGrid::Place p) -> TableGrid::Place {
        if (p.cell + 1 < grid->cellCount(p.row)) return {p.row, p.cell + 1};
        if (p.row + 1 < grid->rowCount()) return {p.row + 1, 0};
        return {};
    };
    // The cell of a row under a column.
    const auto cellUnder = [&](int row, int column) {
        TableGrid::Place to{row, 0};
        for (int i = 0; i < grid->cellCount(row); ++i)
            if (grid->cellAt({row, i}).column <= column) to.cell = i;
        return to;
    };
    const QString newline = m_binding->m_document ? m_binding->m_document->newline() : QStringLiteral("\n");
    // A row added at the end, the caret in its cell under a column.
    const auto appendRow = [&](int column) {
        const int rows = grid->rowCount();
        return run(change(grid, [&](MdTable::Model &model) { return MdTable::insertRows(model, rows, 1, newline); },
                          rows, column));
    };
    // A cell a short row leaves out shares its place with the others left
    // out: the caret is in the one it was taken to.
    const auto prefer = [&](TableGrid::Place to) {
        if (!to.valid() || !grid->cellAt(to).missing) return;
        grid->preferMissing(to, grid->cellAt(to).start);
        grid->updateCaret();
    };
    const auto selectCell = [&](TableGrid::Place to) {
        const MdTable::Cell &target = grid->cellAt(to);
        select(target.start, target.end);
        prefer(to);
    };
    // Esc lets go of a block of cells; any other Esc is the view's, as it
    // is in the text, to close the find bar or leave distraction-free writing.
    if (mods == Qt::NoModifier && key == Qt::Key_Escape && inBlock) {
        select(cursor, cursor);
        return true;
    }
    if (plain && (key == Qt::Key_Left || key == Qt::Key_Right)) {
        const bool left = key == Qt::Key_Left;
        if (!shift && cursor != this->anchor()) {
            // A selection collapses to its end in the direction of the key.
            const int to = left ? qMin(cursor, this->anchor()) : qMax(cursor, this->anchor());
            select(to, to);
            return true;
        }
        // Spaces typed after the text are stepped back over; forward, the end of the text is the cell's.
        if (left && cursor > cell.end) {
            moveTo(cursor - 1);
        } else if (left && cursor > cell.start) {
            moveTo(qMax(cell.start, previousSourceGrapheme(source, cursor)));
        } else if (!left && cursor < cell.end) {
            moveTo(qMin(cell.end, nextSourceGrapheme(source, cursor)));
        } else {
            // Past the edge of a cell's text: the next cell, or out of the table;
            // with Shift, a block of cells grows into the next.
            const TableGrid::Place to = left ? previousCell(place) : nextCell(place);
            if (to.valid() && (!shift || to.row == place.row)) {
                const MdTable::Cell &target = grid->cellAt(to);
                moveTo(left ? target.end : target.start);
                prefer(to);
            } else if (!shift) {
                const int at = left ? grid->start() - breakBefore(source, grid->start())
                                    : grid->end() + breakAfter(source, grid->end());
                if (at < 0 || at > source.size() || (!left && breakAfter(source, grid->end()) == 0)) return true;
                select(at, at);
            }
        }
        return true;
    }
    if (control && (key == Qt::Key_Left || key == Qt::Key_Right)) {
        // A word at a time in a cell's text, and on to the next cell at its edge.
        const bool left = key == Qt::Key_Left;
        const QString cellText = source.mid(cell.start, cell.end - cell.start);
        QTextBoundaryFinder words(QTextBoundaryFinder::Word, cellText);
        int offset = qBound(0, cursor - cell.start, int(cellText.size()));
        words.setPosition(offset);
        int target = -1;
        if (left) {
            for (qsizetype at = words.toPreviousBoundary(); at >= 0; at = words.toPreviousBoundary())
                if (words.boundaryReasons() & QTextBoundaryFinder::StartOfItem) {
                    target = int(at);
                    break;
                }
        } else {
            for (qsizetype at = words.toNextBoundary(); at >= 0; at = words.toNextBoundary())
                if (words.boundaryReasons() & QTextBoundaryFinder::EndOfItem) {
                    target = int(at);
                    break;
                }
        }
        if (target >= 0 && target != offset) {
            moveTo(cell.start + target);
        } else {
            const TableGrid::Place to = left ? previousCell(place) : nextCell(place);
            if (to.valid()) moveTo(left ? grid->cellAt(to).end : grid->cellAt(to).start);
        }
        return true;
    }
    if (plain && (key == Qt::Key_Up || key == Qt::Key_Down)) {
        const bool down = key == Qt::Key_Down;
        if (shift && inBlock) {
            // A block grows a row at a time.
            const int row = place.row + (down ? 1 : -1);
            if (row >= 0 && row < grid->rowCount()) moveTo(grid->cellAt(cellUnder(row, cell.column)).start);
            return true;
        }
        // A table at the very start or end of the text has a line made beside it to write on.
        const bool edgeRow = down ? place.row == grid->rowCount() - 1 : place.row == 0;
        if (!shift && edgeRow && grid->lineOf(place, cursor) == (down ? grid->lineCount(place) - 1 : 0)) {
            if (!down && grid->start() == 0) {
                Adapted line;
                line.kind = Adapted::Changed;
                line.edits.append({{0, 0}, newline});
                return run(line);
            }
            if (down && breakAfter(source, grid->end()) == 0) {
                Adapted line;
                line.kind = Adapted::Changed;
                line.edits.append({{grid->end(), 0}, newline + newline});
                line.anchor = line.cursor = grid->end() + int(2 * newline.size());
                return run(line);
            }
        }
        return moveVertically(grid, cursor, down ? 1 : -1, shift);
    }
    if (plain && (key == Qt::Key_PageUp || key == Qt::Key_PageDown)) {
        // A page from a table goes to its first or last row, and then on.
        const int row = key == Qt::Key_PageDown ? grid->rowCount() - 1 : 0;
        if (place.row == row) return false;
        moveTo(grid->cellAt(cellUnder(row, cell.column)).start);
        return true;
    }
    if (plain && (key == Qt::Key_Home || key == Qt::Key_End)) {
        // The start or end of the line the caret is on in the cell's text.
        const auto [lineStart, lineEnd] = grid->lineRange(place, grid->lineOf(place, qMin(cursor, cell.end)));
        moveTo(key == Qt::Key_Home ? lineStart : lineEnd);
        return true;
    }
    if (control && !shift && key == Qt::Key_A) {
        // The cell's text, then every cell of the table, a block to copy,
        // empty or write over, then the whole text.
        // Three presses go to the whole text, whatever the first two selected.
        const int from = qMin(cursor, anchor()), to = qMax(cursor, anchor());
        int level = 1;
        if (m_selectAll.anchor == anchor() && m_selectAll.cursor == cursor) level = m_selectAll.level + 1;
        else if (from == cell.start && to == cell.end && cell.end > cell.start) level = 2;
        if (level >= 3) {
            m_selectAll = {};
            return false;
        }
        const int lastRow = grid->rowCount() - 1;
        if (level == 1) select(cell.start, cell.end);
        else select(grid->cellAt({0, 0}).start, grid->cellAt({lastRow, grid->cellCount(lastRow) - 1}).end);
        m_selectAll = {anchor(), this->cursor(), level};
        return true;
    }
    if (mods == Qt::NoModifier && inBlock && (key == Qt::Key_Backspace || key == Qt::Key_Delete)) {
        // A block's cells are emptied, rows and columns kept.
        return run(change(grid, [&](MdTable::Model &model) { return MdTable::clearCells(model, r0, r1, c0, c1); },
                          r0, c0));
    }
    if ((key == Qt::Key_Tab && mods == Qt::NoModifier) || key == Qt::Key_Backtab
        || (key == Qt::Key_Tab && mods == Qt::ShiftModifier)) {
        const bool back = key == Qt::Key_Backtab || shift;
        const TableGrid::Place to = back ? previousCell(place) : nextCell(place);
        // Tab in the last cell adds a row; Shift+Tab in the first stays.
        if (!to.valid()) return back ? true : appendRow(0);
        selectCell(to);
        return true;
    }
    if (mods == Qt::NoModifier && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
        if (place.row + 1 < grid->rowCount()) {
            selectCell(cellUnder(place.row + 1, cell.column));
            return true;
        }
        // On the last row a row is added; on a last row left empty, it goes,
        // and the caret goes on below the table.
        bool empty = place.row > 0;
        for (int i = 0; empty && i < grid->cellCount(place.row); ++i) {
            const MdTable::Cell &c = grid->cellAt({place.row, i});
            empty = c.start == c.end;
        }
        if (!empty) return appendRow(cell.column);
        const int row = place.row;
        Adapted removed = change(grid, [&](MdTable::Model &model) { return MdTable::deleteRows(model, row, 1); }, 0, 0);
        if (removed.kind != Adapted::Changed) return run(removed);
        // Below the table, on the blank line after it, made if it is missing.
        const int lengthBefore = grid->end() - grid->start();
        int tableEnd = grid->start() + lengthBefore;
        for (const SourceEdit &e : std::as_const(removed.edits)) tableEnd += int(e.replacement.size()) - e.range.length;
        const int gap = breakAfter(source, grid->end());
        if (gap == 0) removed.edits.append({{grid->end(), 0}, newline + newline});
        removed.anchor = removed.cursor = tableEnd + (gap == 0 ? int(2 * newline.size()) : gap);
        return run(removed);
    }
    if (mods == Qt::ShiftModifier && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
        // A line break inside a cell, as HTML has it.
        m_binding->insertText(QStringLiteral("<br>"), EditKind::Typing);
        return true;
    }
    return false;
}

TableGrids::Adapted TableGrids::adapt(SourceRange range, const QString &inserted) const
{
    const int caret = range.start + int(inserted.size());
    return adapt(QList<SourceEdit>{{range, inserted}}, caret, caret);
}

namespace {

// The covering edit that turns a table's text into another: the two with
// what they share at either end left out.
SourceEdit coveringEdit(int start, QStringView before, const QString &after)
{
    int same = 0;
    const int shortest = int(qMin(before.size(), after.size()));
    while (same < shortest && before[same] == after[same]) ++same;
    // Never inside a character or a grapheme, where the text takes no edit:
    // one emoji for another shares its first unit.
    const auto boundary = [&](int inBefore, int inAfter) {
        return sourceGraphemeBoundary(before, inBefore) && sourceGraphemeBoundary(after, inAfter);
    };
    while (same > 0 && !boundary(same, same)) --same;
    int tail = 0;
    while (tail < shortest - same && before[before.size() - 1 - tail] == after[after.size() - 1 - tail]) ++tail;
    while (tail > 0 && !boundary(int(before.size()) - tail, int(after.size()) - tail)) --tail;
    return SourceEdit{{start + same, int(before.size()) - same - tail}, after.mid(same, after.size() - same - tail)};
}

} // namespace

int TableGrids::lineAfter(const TableGrid *grid) const
{
    const QString source = text();
    const int gap = breakAfter(source, grid->end());
    return gap == 0 ? -1 : grid->end() + gap;
}

bool TableGrids::block(const TableGrid *grid, int *firstRow, int *lastRow, int *firstColumn, int *lastColumn) const
{
    const int a = anchor(), c = cursor();
    if (a == c || !gridAt(a) || gridAt(a) != gridAt(c) || gridAt(a) != grid) return false;
    const TableGrid::Place from = grid->placeOf(a), to = grid->placeOf(c);
    if (!from.valid() || !to.valid() || from == to) return false;
    const MdTable::Cell &one = grid->cellAt(from), &other = grid->cellAt(to);
    *firstRow = qMin(from.row, to.row);
    *lastRow = qMax(from.row, to.row);
    *firstColumn = qMin(one.column, other.column);
    *lastColumn = qMax(one.column + one.span, other.column + other.span) - 1;
    // The block grows until it cuts no merged cell.
    for (bool grew = true; grew;) {
        grew = false;
        for (int row = *firstRow; row <= *lastRow; ++row) {
            for (int i = 0; i < grid->cellCount(row); ++i) {
                const MdTable::Cell &cell = grid->cellAt({row, i});
                const int last = cell.column + cell.span - 1;
                if (last < *firstColumn || cell.column > *lastColumn) continue;
                if (cell.column < *firstColumn) {
                    *firstColumn = cell.column;
                    grew = true;
                }
                if (last > *lastColumn) {
                    *lastColumn = last;
                    grew = true;
                }
            }
        }
    }
    return true;
}

namespace {

// The refusal of an edit that would make two cells of a row one.
QString joinsCells()
{
    return TableGrids::tr("That would join this cell with the next: code started with ` here would end in another cell.");
}

// The refusal of a change that would take a table past what a grid holds.
QString tooLarge()
{
    return TableGrids::tr("A table shown as a grid holds at most %L1 cells and %2 columns.")
        .arg(qlonglong(MdTable::MaxCells))
        .arg(MdTable::MaxColumns);
}

} // namespace

std::optional<QList<QList<MdTable::Item>>> TableGrids::rowsToPut(const TableGrid *grid, const QString &text, bool strict,
                                                                 QString *refusal) const
{
    // Each line is read as a row under the table's own header and
    // delimiter, so that it has the cells, merged ones too, the table
    // would read in it.
    const QString source = this->text();
    const MdTable::Table &table = grid->table();
    const MdTable::Line &head = table.rows[0].line, &delimiter = table.rows[1].line;
    const QString lead = source.mid(head.content, head.end - head.content) + u'\n'
                         + source.mid(delimiter.content, delimiter.end - delimiter.content) + u'\n';
    const MdTable::Dialect dialect = this->dialect();
    QList<QList<MdTable::Item>> rows;
    const QStringList lines =
        QString(text).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(u'\r', u'\n').split(u'\n');
    QString above;
    for (const QString &line : lines) {
        int content = 0;
        while (content < line.size() && (line[content] == u' ' || line[content] == u'\t' || line[content] == u'>'))
            ++content;
        const QString row = line.mid(content).trimmed();
        // A row with a delimiter row under it is the header of a table of
        // its own, which is no rows of this one.
        if (!above.isEmpty() && !row.isEmpty() && MdTable::delimiterRow(row, 0, int(row.size()))) {
            const QString pair = above + u'\n' + row;
            if (MdTable::scan(pair, 0, int(pair.size()), dialect)) return std::nullopt;
        }
        above = row;
        if (row.isEmpty()) {
            rows.append(QList<MdTable::Item>{});
            continue;
        }
        QList<MdTable::Field> fields;
        QList<int> pipes;
        MdTable::splitRow(row, 0, int(row.size()), dialect.protectCode, fields, &pipes);
        if (pipes.isEmpty()) return std::nullopt;
        if (strict && (pipes.first() != 0 || pipes.last() != row.size() - 1)) return std::nullopt;
        const QString composed = lead + row;
        const auto read = MdTable::scan(composed, 0, int(composed.size()), dialect);
        if (!read || read->rows.size() != 3) return std::nullopt;
        if (read->unfit != MdTable::Unfit::None) {
            if (refusal) *refusal = tr("A row put in a table has no more cells than the table has columns.");
            return std::nullopt;
        }
        rows.append(MdTable::model(composed, *read).rows.value(1));
    }
    return rows;
}

std::optional<TableGrids::Adapted> TableGrids::adaptOne(const SourceEdit &edit) const
{
    const QString source = text();
    const int from = edit.range.start, to = edit.range.start + edit.range.length;
    const MdTable::Dialect dialect = this->dialect();
    const QString newline = m_binding->m_document ? m_binding->m_document->newline() : QStringLiteral("\n");
    const auto refuse = [&](const QString &reason) {
        Adapted refused;
        refused.kind = Adapted::Refused;
        refused.reason = reason;
        return refused;
    };
    for (const TableGrid *grid : m_grids) {
        // What replaces a block of cells empties them and goes in the first.
        int r0 = 0, r1 = 0, c0 = 0, c1 = 0;
        if (from == qMin(anchor(), cursor()) && to == qMax(anchor(), cursor()) && block(grid, &r0, &r1, &c0, &c1)) {
            const QString written = MdTable::encodeCell(edit.replacement, dialect);
            Adapted cleared = change(grid, [&](MdTable::Model &model) {
                MdTable::clearCells(model, r0, r1, c0, c1);
                const int item = MdTable::itemAt(model.rows[r0], c0);
                if (item >= 0) model.rows[r0][item].text = written;
                return QString();
            }, r0, c0, true);
            cleared.anchor = cleared.cursor;
            return cleared;
        }
        // Text written on the blank line after a table starts a paragraph
        // a line lower, so that the blank line stays and the text is no row.
        const int blank = lineAfter(grid);
        if (blank >= 0 && from == to && from >= blank && !edit.replacement.isEmpty()) {
            int end = blank;
            while (end < source.size() && (source[end] == u' ' || source[end] == u'\t')) ++end;
            const bool blankLine = end == source.size() || breakAfter(source, end) > 0;
            const QChar first = edit.replacement.front();
            if (blankLine && from <= end && first != u'\n' && first != u'\r') {
                // Whole table rows put there, a pipe at either end of each and
                // no more cells than the table has, join it, as a row put
                // after its last; anything else is a paragraph.
                const bool rows = edit.replacement.endsWith(u'\n') || edit.replacement.endsWith(u'\r');
                const auto parsed =
                    rows && from == blank ? rowsToPut(grid, edit.replacement.trimmed(), true, nullptr) : std::nullopt;
                if (parsed) {
                    const QList<QList<MdTable::Item>> values = *parsed;
                    const int at = grid->rowCount();
                    return change(grid, [&](MdTable::Model &model) {
                        MdTable::insertRows(model, at, int(values.size()), newline);
                        for (int i = 0; i < values.size(); ++i)
                            if (!values[i].isEmpty()) model.rows[at + i] = values[i];
                        return QString();
                    }, at, 0);
                }
                // The old blank line's break can end the new paragraph.
                // Keep a blank line after it as well when prose follows.
                int next = end + breakAfter(source, end);
                while (next < source.size() && (source[next] == u' ' || source[next] == u'\t')) ++next;
                int typedEnd = int(edit.replacement.size());
                while (typedEnd > 0 && (edit.replacement[typedEnd - 1] == u' ' || edit.replacement[typedEnd - 1] == u'\t'))
                    --typedEnd;
                QString inserted = newline + edit.replacement;
                if (next < source.size() && !isBreak(source[next]) && breakBefore(edit.replacement, typedEnd) == 0)
                    inserted += newline;
                Adapted below;
                below.kind = Adapted::Changed;
                below.edits.append({{from, 0}, inserted});
                below.anchor = below.cursor = from + int(newline.size() + edit.replacement.size());
                return below;
            }
        }
        if (to < grid->start() || from > grid->end()) continue;
        if (endsBefore(source, from, to, edit.replacement, grid->start())) continue;
        // A table taken whole is the writer's to take.
        if (from <= grid->start() && to >= grid->end()) continue;
        // Whole rows: the lines of body rows, with their breaks.
        const MdTable::Table &table = grid->table();
        const int rows = int(table.rows.size());
        const auto lineOf = [&](int row) -> const MdTable::Line & { return table.rows[row].line; };
        const auto rowStartingAt = [&](int position) {
            for (int r = 0; r < rows; ++r)
                if (lineOf(r).start == position) return r;
            return -1;
        };
        const auto rowEndingAt = [&](int position) {
            for (int r = 0; r < rows; ++r)
                if (lineOf(r).end == position) return r;
            return -1;
        };
        const auto rowNextAt = [&](int position) {
            for (int r = 0; r < rows; ++r)
                if (lineOf(r).next == position && lineOf(r).next > lineOf(r).end) return r;
            return -1;
        };
        const QString headerStays = tr("The header row and the line under it stay; delete the table to remove them.");
        if (edit.replacement.isEmpty()) {
            int first = -1, last = -1;
            if (const int r = rowStartingAt(from); r >= 0) {
                first = r;
                last = rowNextAt(to);
                // Rows emptied but kept, their line breaks too, as when a line is changed.
                if (last < 0 && rowEndingAt(to) >= first) {
                    const int lastRow = rowEndingAt(to);
                    if (first < 2) return refuse(headerStays);
                    return change(grid, [&](MdTable::Model &model) {
                        return MdTable::clearCells(model, first - 1, lastRow - 1, 0, MdTable::columnCount(model) - 1);
                    }, first - 1, 0);
                }
            } else if (const int r = rowEndingAt(from); r >= 0 && rowEndingAt(to) > r) {
                // The last rows of a table, with the break before them.
                first = r + 1;
                last = rowEndingAt(to);
            }
            if (first >= 0 && last >= first) {
                if (first < 2) return refuse(headerStays);
                const int count = last - first + 1;
                return change(grid, [&](MdTable::Model &model) { return MdTable::deleteRows(model, first - 1, count); },
                              qMin(first - 1, rows - 2 - count), 0);
            }
        } else if (from == to) {
            // Lines put in between rows: table rows, or empty lines as empty rows.
            const bool before = rowStartingAt(from) >= 0 && (edit.replacement.endsWith(u'\n') || edit.replacement.endsWith(u'\r'));
            const bool after = rowEndingAt(from) >= 0 && (edit.replacement.startsWith(u'\n') || edit.replacement.startsWith(u'\r'));
            if (before || after) {
                const int row = before ? rowStartingAt(from) : rowEndingAt(from);
                QString lines = edit.replacement;
                if (before) lines.chop(lines.endsWith(QStringLiteral("\r\n")) ? 2 : 1);
                else lines.remove(0, lines.startsWith(QStringLiteral("\r\n")) ? 2 : 1);
                QString wide;
                const auto parsed = rowsToPut(grid, lines, false, &wide);
                if (!parsed) return refuse(wide.isEmpty() ? tr("Only table rows fit between the rows of a table.") : wide);
                // Model rows: the header is 0, and a row put before or after
                // the header or its delimiter goes first among the body.
                int at = before ? row - 1 : row;
                at = qMax(1, at);
                const QList<QList<MdTable::Item>> values = *parsed;
                return change(grid, [&](MdTable::Model &model) {
                    MdTable::insertRows(model, at, int(values.size()), newline);
                    for (int i = 0; i < values.size(); ++i)
                        if (!values[i].isEmpty()) model.rows[at + i] = values[i];
                    return QString();
                }, at, 0);
            }
        }
    }
    return std::nullopt;
}

namespace {

bool asciiPunct(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= 0x21 && u <= 0x2f) || (u >= 0x3a && u <= 0x40) || (u >= 0x5b && u <= 0x60) || (u >= 0x7b && u <= 0x7e);
}

struct TickRun {
    int start;
    int length;
    int close;
};

// Read runs as code spans are read: an escape can hide an opener, and a
// run of the same length closes it. A close of -1 marks an unmatched run.
QList<TickRun> tickRuns(QStringView text)
{
    QList<TickRun> runs;
    const int size = int(text.size());
    for (int i = 0; i < size;) {
        if (text[i] == u'\\' && i + 1 < size && asciiPunct(text[i + 1])) {
            i += 2;
            continue;
        }
        if (text[i] != u'`') {
            ++i;
            continue;
        }
        int run = i;
        while (run < size && text[run] == u'`') ++run;
        int close = -1;
        for (int k = run; k < size;) {
            if (text[k] != u'`') {
                ++k;
                continue;
            }
            int end = k;
            while (end < size && text[end] == u'`') ++end;
            if (end - k == run - i) {
                close = k;
                break;
            }
            k = end;
        }
        const int length = run - i;
        runs.append({i, length, close});
        i = close < 0 ? run : close + length;
    }
    return runs;
}

struct GridCellEdit {
    TableGrid *grid;
    TableGrid::Place place;
    SourceEdit edit;
};

struct CellRewrite {
    TableGrid::Place place;
    QString original;
    QString payload;
    QString text;
    struct BreakRun {
        int rawStart;
        int rawEnd;
        int at;
    };
    struct Write {
        SourceRange before;
        SourceRange after;
    };
    QList<BreakRun> breaks;
    QList<Write> writes;
    int rawLength = 0;
    int lead = 0;
    QList<int> tracked; // Existing tick escapes, carried along with the cell's text
    QList<int> freed;   // Removed from the payload
    QList<int> pipes;   // Escaped after freeing ticks
    QList<int> added;   // New tick escapes after encoding pipes

    void encode(const MdTable::Dialect &dialect, bool closeTicks);
    void protectOpenTicks();
    bool restoresEscape() const;
    int offsetOf(int raw) const;
    QList<int> protectedSlashes() const;
};

int countBefore(const QList<int> &positions, int offset)
{
    return int(std::lower_bound(positions.cbegin(), positions.cend(), offset) - positions.cbegin());
}

QList<int> closedTickEscapes(QStringView text, const QList<int> &tracked, const QList<CellRewrite::Write> &writes)
{
    if (tracked.isEmpty()) return {};
    // Look for matching delimiters with the editor's escapes out of the
    // way. Keep the escapes on every run outside the pair being edited.
    QString bare;
    QList<int> bareOffsets;
    int from = 0;
    for (int slash : tracked) {
        bare += text.mid(from, slash - from);
        bareOffsets.append(int(bare.size()));
        from = slash + 1;
    }
    bare += text.mid(from);
    QList<int> freed;
    for (const auto &[start, length, close] : tickRuns(bare)) {
        if (close < 0) continue;
        const int first = countBefore(bareOffsets, start), last = countBefore(bareOffsets, start + length);
        const int closing = countBefore(bareOffsets, close), end = countBefore(bareOffsets, close + length);
        if (last == first && end == closing) continue;
        const auto touched = [&](int begin, int limit) {
            return std::any_of(writes.cbegin(), writes.cend(), [&](const CellRewrite::Write &edit) {
                const int at = edit.after.start;
                if (edit.after.length == 0) return edit.before.length > 0 && at >= begin && at <= limit;
                return at < limit && at + edit.after.length > begin;
            });
        };
        if (!touched(start + first, start + length + last) && !touched(close + closing, close + length + end))
            continue;
        for (int i = first; i < last; ++i) freed.append(tracked[i]);
        for (int i = closing; i < end; ++i) freed.append(tracked[i]);
    }
    return freed;
}

void CellRewrite::encode(const MdTable::Dialect &dialect, bool closeTicks)
{
    freed = closeTicks && dialect.protectCode ? closedTickEscapes(payload, tracked, writes) : QList<int>();
    pipes.clear();
    added.clear();
    text = payload;
    for (auto slash = freed.crbegin(); slash != freed.crend(); ++slash) text.remove(*slash, 1);
    QList<MdTable::Field> fields;
    MdTable::splitRow(text, 0, int(text.size()), dialect.protectCode, fields, &pipes);
    for (auto pipe = pipes.crbegin(); pipe != pipes.crend(); ++pipe) text.insert(*pipe, u'\\');
    while (!text.isEmpty() && (text.back() == u' ' || text.back() == u'\t')) text.chop(1);
}

void CellRewrite::protectOpenTicks()
{
    for (const auto &[start, length, close] : tickRuns(text))
        if (close < 0)
            for (int i = 0; i < length; ++i) added.append(start + i);
    for (auto at = added.crbegin(); at != added.crend(); ++at) text.insert(*at, u'\\');
}

bool CellRewrite::restoresEscape() const
{
    if (added.isEmpty()) return false;
    for (const Write &write : writes) {
        const QStringView removed = QStringView(original).mid(write.before.start, write.before.length);
        if (!removed.contains(u'\\')) continue;
        // At the end of a replacement, a slash inserted before the next
        // tick can restore a deletion, even if other matches change the cell.
        const int from = qMax(0, write.after.start);
        const int to = qMax(0, write.after.start + write.after.length);
        const int first = from + countBefore(pipes, from), last = to + countBefore(pipes, to);
        const int earlier = countBefore(added, first), inside = countBefore(added, last);
        const int throughEnd = int(std::upper_bound(added.cbegin(), added.cend(), last) - added.cbegin());
        if (earlier == throughEnd) continue;
        const int start = first + earlier;
        if (QStringView(text).mid(start, last + throughEnd - start) == removed
            || (inside > earlier && QStringView(text).mid(start, last + inside - start) == removed))
            return true;
    }
    return false;
}

int CellRewrite::offsetOf(int raw) const
{
    int shift = 0;
    for (const BreakRun &run : breaks) {
        if (raw >= run.rawEnd) shift += run.rawEnd - run.rawStart - 1;
        else if (raw > run.rawStart) {
            raw = run.at + 1 + shift;
            break;
        } else break;
    }
    int offset = qMax(0, raw - shift - lead);
    offset -= countBefore(freed, offset);
    offset += countBefore(pipes, offset);
    return offset + countBefore(added, offset);
}

QList<int> CellRewrite::protectedSlashes() const
{
    QList<int> slashes;
    for (int at : tracked) {
        if (std::binary_search(freed.cbegin(), freed.cend(), at)) continue;
        at -= countBefore(freed, at);
        at += countBefore(pipes, at);
        slashes.append(at + countBefore(added, at));
    }
    for (qsizetype i = 0; i < added.size(); ++i) slashes.append(added[i] + int(i));
    std::sort(slashes.begin(), slashes.end());
    return slashes;
}

CellRewrite rewriteCell(QStringView source, std::span<const GridCellEdit> edits,
                        const TableGrids::TickEscapes &escapes)
{
    const GridCellEdit &first = edits.front();
    const MdTable::Cell &cell = first.grid->cellAt(first.place);
    const int tail = tailOf(source, cell);
    CellRewrite out;
    out.place = first.place;
    out.original = source.mid(cell.start, cell.end - cell.start).toString();
    const QList<int> remembered = first.grid == escapes.grid && first.place == escapes.place ? escapes.slashes : QList<int>();
    // Work from the original offsets even when only some matches are tried.
    int maxEnd = cell.end;
    for (const GridCellEdit &in : edits) maxEnd = qMax(maxEnd, in.edit.range.start + in.edit.range.length);
    const QStringView base = source.mid(cell.start, qMin(maxEnd, cell.end + tail) - cell.start);
    QString &payload = out.payload;
    qsizetype rememberedAt = 0;
    int raw = 0, from = 0;
    const auto copy = [&](int to) {
        while (rememberedAt < remembered.size() && remembered[rememberedAt] < to) {
            const int slash = remembered[rememberedAt++];
            if (slash >= from && slash + 1 < to && base[slash] == u'\\' && base[slash + 1] == u'`')
                out.tracked.append(int(payload.size()) + slash - from);
        }
        payload += base.mid(from, to - from);
        raw += to - from;
        from = to;
    };
    // A run of line breaks put in a cell becomes one space.
    static const QRegularExpression breaks(QStringLiteral("[ \\t]*[\\r\\n\\x{2028}\\x{2029}]+[ \\t]*"));
    for (const GridCellEdit &in : edits) {
        const SourceEdit &edit = in.edit;
        const int start = qBound(cell.start, edit.range.start, cell.end + tail) - cell.start;
        const int end = qBound(cell.start, edit.range.start + edit.range.length, cell.end + tail) - cell.start;
        copy(qMax(start, from));
        const int at = int(payload.size());
        const QString replacement = QString(edit.replacement).replace(u'\t', u' ');
        int done = 0;
        for (auto found = breaks.globalMatch(replacement); found.hasNext();) {
            const QRegularExpressionMatch run = found.next();
            payload += QStringView(replacement).mid(done, run.capturedStart() - done);
            raw += int(run.capturedStart()) - done;
            out.breaks.append({raw, raw + int(run.capturedLength()), int(payload.size())});
            payload += u' ';
            raw += int(run.capturedLength());
            done = int(run.capturedEnd());
        }
        payload += QStringView(replacement).mid(done);
        raw += int(replacement.size()) - done;
        out.writes.append(CellRewrite::Write{{start, end - start}, {at, int(payload.size()) - at}});
        from = qMax(from, end);
    }
    copy(int(base.size()));
    out.rawLength = raw;
    while (out.lead < payload.size() && (payload[out.lead] == u' ' || payload[out.lead] == u'\t')) ++out.lead;
    // A slash before a tick is still protective only if a newer edit
    // has left it unescaped.
    out.tracked.removeIf([&](int slash) {
        int at = slash;
        while (at > 0 && payload[at - 1] == u'\\') --at;
        return (slash - at) % 2 != 0;
    });
    for (int &slash : out.tracked) slash -= out.lead;
    for (CellRewrite::Write &write : out.writes) write.after.start -= out.lead;
    payload.remove(0, out.lead);
    return out;
}

bool fitRow(QList<MdTable::Item> &row, std::span<CellRewrite> cells, int columns, const MdTable::Dialect &dialect)
{
    const QList<MdTable::Item> before = row;
    const auto readsBack = [&] {
        for (const CellRewrite &cell : cells) row[cell.place.cell].text = cell.text;
        return MdTable::readsBack(row, columns, dialect);
    };
    bool freed = false;
    for (CellRewrite &cell : cells) {
        cell.encode(dialect, true);
        freed |= !cell.freed.isEmpty();
    }
    if (readsBack()) return true;
    if (freed) {
        for (CellRewrite &cell : cells) cell.encode(dialect, false);
        if (readsBack()) return true;
    }
    if (dialect.protectCode) {
        for (CellRewrite &cell : cells) cell.protectOpenTicks();
        if (std::none_of(cells.begin(), cells.end(), [](const CellRewrite &cell) { return cell.restoresEscape(); })
            && readsBack())
            return true;
    }
    row = before;
    return false;
}

} // namespace

TableGrids::Adapted TableGrids::adapt(const QList<SourceEdit> &edits, int anchor, int cursor, int *left) const
{
    Adapted out;
    if (left) *left = 0;
    if (m_grids.isEmpty()) return out;
    if (edits.size() == 1)
        if (std::optional<Adapted> one = adaptOne(edits.first())) {
            if (left && one->kind == Adapted::Refused) *left = 1;
            return *one;
        }
    const auto refuse = [&](const QString &reason) {
        Adapted refused;
        refused.kind = Adapted::Refused;
        refused.reason = reason;
        if (left) *left = int(edits.size());
        return refused;
    };
    // An edit that cannot be made refuses them all; or, with `left`, it is
    // left out, and counted, and the first one's reason kept.
    QString reason;
    int dropped = 0;
    const auto drop = [&](const QString &why, int count) {
        if (reason.isEmpty()) reason = why;
        dropped += count;
        return left != nullptr;
    };
    QList<SourceEdit> passing;
    QList<GridCellEdit> inCells;
    const QString source = text();
    for (const SourceEdit &edit : edits) {
        const int from = edit.range.start, to = edit.range.start + edit.range.length;
        // The grid the edit reaches into, without taking it whole; one that
        // reaches into two cuts through one of them.
        TableGrid *grid = nullptr;
        bool two = false;
        for (TableGrid *g : m_grids) {
            if (to < g->start() || from > g->end() || (from <= g->start() && to >= g->end())) continue;
            if (endsBefore(source, from, to, edit.replacement, g->start())) continue;
            if (grid) two = true;
            grid = g;
        }
        if (two) {
            if (!drop(tr("Select the whole table, or stay inside one cell."), 1)) return refuse(reason);
            continue;
        }
        // An edit outside the grids, or of tables whole, is the writer's
        // to make; but none takes away the blank line after a table, which
        // would make the text after it the table's next row.
        if (!grid) {
            bool keeps = true;
            for (const TableGrid *g : m_grids) {
                if (from <= g->start() && to >= g->end()) continue;
                const int blank = lineAfter(g);
                if (blank < 0 || from > blank + 1) continue;
                int next = blank;
                while (next < source.size() && (source[next] == u' ' || source[next] == u'\t')) ++next;
                const int afterBlank = next + breakAfter(source, next);
                if (next < source.size() && breakAfter(source, next) == 0) continue;
                if (from < afterBlank && to > g->end() && edit.replacement.count(u'\n') + edit.replacement.count(u'\r') == 0)
                    keeps = false;
            }
            if (!keeps) {
                if (!drop(tr("A table keeps a blank line after it."), 1)) return refuse(reason);
                continue;
            }
            passing.append(edit);
            continue;
        }
        const TableGrid::Place place = grid->placeOf(from);
        // A deletion from a cell's text on along its row, as to the end of a
        // line or a word, stops at the end of the cell's text.
        if (place.valid() && edit.replacement.isEmpty() && !(place == grid->placeOf(to))
            && MdTable::rowAt(grid->table(), from) == MdTable::rowAt(grid->table(), to) && from <= grid->cellAt(place).end) {
            SourceEdit clamped = edit;
            clamped.range.length = qMax(0, grid->cellAt(place).end - from);
            inCells.append({grid, place, clamped});
            continue;
        }
        if (!place.valid() || !(place == grid->placeOf(to))) {
            if (!drop(tr("Select the whole table, or stay inside one cell."), 1)) return refuse(reason);
            continue;
        }
        inCells.append({grid, place, edit});
    }
    if (inCells.isEmpty() && dropped == 0) return out;

    const MdTable::Dialect dialect = this->dialect();
    out.kind = Adapted::Changed;
    out.anchor = anchor;
    out.cursor = cursor;
    // Keep each table, row and cell together, with its original edit order.
    std::stable_sort(inCells.begin(), inCells.end(), [](const GridCellEdit &a, const GridCellEdit &b) {
        return std::tuple{a.grid->start(), a.place.row, a.place.cell, a.edit.range.start}
               < std::tuple{b.grid->start(), b.place.row, b.place.cell, b.edit.range.start};
    });
    std::span<const GridCellEdit> remaining(inCells.constData(), size_t(inCells.size()));
    QList<SourceEdit> written = std::move(passing);
    const TickEscapes remembered = left ? TickEscapes{} : m_tickEscapes;
    for (size_t first = 0; first < remaining.size();) {
        TableGrid *grid = remaining[first].grid;
        size_t next = first + 1;
        while (next < remaining.size() && remaining[next].grid == grid) ++next;
        const auto mine = remaining.subspan(first, next - first);
        first = next;
        MdTable::Model model = MdTable::model(source, grid->table());
        const int columns = MdTable::columnCount(model);
        QList<CellRewrite> mappings;
        bool accepted = false;
        for (size_t i = 0; i < mine.size();) {
            const int row = mine[i].place.row;
            QList<std::span<const GridCellEdit>> groups;
            QList<CellRewrite> cells;
            while (i < mine.size() && mine[i].place.row == row) {
                size_t end = i + 1;
                while (end < mine.size() && mine[end].place == mine[i].place) ++end;
                auto group = mine.subspan(i, end - i);
                groups.append(group);
                cells.append(rewriteCell(source, group, remembered));
                i = end;
            }
            auto &items = model.rows[row];
            if (fitRow(items, {cells.data(), size_t(cells.size())}, columns, dialect)) {
                accepted = true;
                if (!left) mappings += std::move(cells);
                continue;
            }
            if (!left) return refuse(joinsCells());
            // Try the row together first, since the edits can make one
            // another safe. If that fails, keep what fits in source order.
            for (qsizetype g = 0; g < groups.size(); ++g) {
                const auto group = groups[g];
                if (groups.size() > 1 && fitRow(items, {&cells[g], 1}, columns, dialect)) {
                    accepted = true;
                    continue;
                }
                if (group.size() == 1) {
                    drop(joinsCells(), 1);
                    continue;
                }
                QList<GridCellEdit> kept;
                for (const GridCellEdit &match : group) {
                    kept.append(match);
                    CellRewrite trial = rewriteCell(source, {kept.constData(), size_t(kept.size())}, remembered);
                    if (fitRow(items, {&trial, 1}, columns, dialect)) {
                        accepted = true;
                    } else {
                        kept.removeLast();
                        drop(joinsCells(), 1);
                    }
                }
            }
        }
        if (!accepted) continue;
        const MdTable::Layout layout = MdTable::write(model);
        const int tableLength = grid->end() - grid->start();
        const QStringView before = QStringView(source).mid(grid->start(), tableLength);
        if (before != layout.text) written.append(coveringEdit(grid->start(), before, layout.text));
        if (left) continue; // Replace All leaves the view's selection to the source edits.

        // Map the ordinary edit's selection from the text it would have made
        // to the retidied table. Keep any offset added by an earlier table.
        int naiveStart = grid->start();
        for (const SourceEdit &edit : edits) {
            const int end = edit.range.start + edit.range.length;
            if (end < grid->start() || endsBefore(source, edit.range.start, end, edit.replacement, grid->start()))
                naiveStart += int(edit.replacement.size()) - edit.range.length;
        }
        int length = tableLength;
        for (const GridCellEdit &in : mine) length += int(in.edit.replacement.size()) - in.edit.range.length;
        const auto mapPosition = [&](int naive, int &position) {
            if (naive < naiveStart) return;
            if (naive > naiveStart + length) {
                position += int(layout.text.size()) - length;
                return;
            }
            int shift = naiveStart - grid->start();
            size_t counted = 0;
            for (const CellRewrite &cell : std::as_const(mappings)) {
                const MdTable::Cell &original = grid->cellAt(cell.place);
                while (counted < mine.size()
                       && mine[counted].edit.range.start + mine[counted].edit.range.length < original.rawStart) {
                    const SourceEdit &edit = mine[counted++].edit;
                    shift += int(edit.replacement.size()) - edit.range.length;
                }
                const int raw = naive - original.start - shift;
                if (raw < 0 || raw > cell.rawLength) continue;
                const MdTable::Placed &at = layout.cells[cell.place.row][cell.place.cell];
                position += naiveStart + at.start + qMin(cell.offsetOf(raw), at.length + at.tail) - naive;
                return;
            }
        };
        mapPosition(anchor, out.anchor);
        mapPosition(cursor, out.cursor);
        if (edits.size() == 1 && mappings.size() == 1) {
            const CellRewrite &cell = mappings.constFirst();
            out.tickEscapes = {grid, cell.place, cell.protectedSlashes()};
        }
    }
    std::stable_sort(written.begin(), written.end(),
                     [](const SourceEdit &a, const SourceEdit &b) { return a.range.start < b.range.start; });
    for (qsizetype i = 1; i < written.size(); ++i)
        if (written[i].range.start < written[i - 1].range.start + written[i - 1].range.length)
            return refuse(tr("Select the whole table, or stay inside one cell."));
    if (left) *left = dropped;
    out.reason = reason;
    // Every edit left out: nothing is made.
    if (dropped == int(edits.size())) {
        out.kind = Adapted::Refused;
        return out;
    }
    out.edits = written;
    return out;
}

TableGrids::Adapted TableGrids::change(const TableGrid *grid, const std::function<QString(MdTable::Model &)> &operation,
                                       int row, int column, bool selectText) const
{
    Adapted out;
    const QString source = text();
    MdTable::Model model = MdTable::model(source, grid->table());
    const QString refused = operation(model);
    if (!refused.isEmpty()) {
        out.kind = Adapted::Refused;
        out.reason = refused;
        return out;
    }
    // A table past the limits a grid is laid out within would show as its
    // Markdown: it grows no further from the grid.
    const int columnsNow = MdTable::columnCount(model);
    if (columnsNow > MdTable::MaxColumns || qsizetype(columnsNow) * model.rows.size() > MdTable::MaxCells) {
        out.kind = Adapted::Refused;
        out.reason = tooLarge();
        return out;
    }
    const MdTable::Layout layout = MdTable::write(model);
    // Every row reads back as the cells the change gave it.
    const MdTable::Dialect written = dialect();
    const int columns = MdTable::columnCount(model);
    for (int r = 0; r < model.rows.size(); ++r) {
        if (MdTable::readsBack(layout, model, r, written, columns)) continue;
        out.kind = Adapted::Refused;
        out.reason = joinsCells();
        return out;
    }
    out.kind = Adapted::Changed;
    row = qBound(0, row, int(model.rows.size()) - 1);
    const int item = qMax(0, MdTable::itemAt(model.rows[row], qBound(0, column, MdTable::columnCount(model) - 1)));
    const MdTable::Placed &at = layout.cells[row][item];
    out.anchor = grid->start() + at.start;
    out.cursor = grid->start() + at.start + (selectText ? at.length : 0);
    const QStringView before = QStringView(source).mid(grid->start(), grid->end() - grid->start());
    if (before != layout.text) out.edits.append(coveringEdit(grid->start(), before, layout.text));
    return out;
}

// ---- TableGridView ----------------------------------------------------------------

TableGridView::TableGridView(QQuickItem *parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    connect(this, &TableGridView::viewChanged, this, &QQuickItem::update);
    connect(this, &TableGridView::lookChanged, this, &QQuickItem::update);
}

void TableGridView::setGrid(TableGrid *grid)
{
    if (grid == m_grid) return;
    if (m_grid) disconnect(m_grid, nullptr, this, nullptr);
    m_grid = grid;
    if (grid) {
        connect(grid, &TableGrid::contentChanged, this, &QQuickItem::update);
        connect(grid, &TableGrid::geometryChanged, this, &QQuickItem::update);
    }
    emit gridChanged();
    update();
}

void TableGridView::setViewTop(qreal top)
{
    if (qFuzzyCompare(top + 1, m_viewTop + 1)) return;
    m_viewTop = top;
    emit viewChanged();
}

void TableGridView::setViewHeight(qreal height)
{
    if (qFuzzyCompare(height + 1, m_viewHeight + 1)) return;
    m_viewHeight = height;
    emit viewChanged();
}

QSGNode *TableGridView::updatePaintNode(QSGNode *old, UpdatePaintNodeData *)
{
    delete old;
    if (!m_grid || !window()) return nullptr;
    auto *root = new QSGTransformNode;
    QMatrix4x4 shift;
    shift.translate(float(-m_grid->scrollX()), 0);
    root->setMatrix(shift);
    const auto rect = [&](const QRectF &r, const QColor &color) {
        QSGRectangleNode *node = window()->createRectangleNode();
        node->setRect(r);
        node->setColor(color);
        root->appendChildNode(node);
    };
    const qreal top = qMax<qreal>(0, m_viewTop);
    const qreal bottom = qMin(m_grid->height(), m_viewTop + m_viewHeight);
    if (bottom <= top) return root;
    const QList<TableGrid::Drawn> cells = m_grid->drawn(top, bottom);
    if (cells.isEmpty()) return root;
    qreal firstTop = cells.first().rect.top(), lastBottom = 0;
    for (const TableGrid::Drawn &cell : cells) lastBottom = qMax(lastBottom, cell.rect.bottom());
    // The rules show between the cells, which are filled over them.
    rect(QRectF(0, firstTop - 1, m_grid->width(), lastBottom - firstTop + 2), m_ruleColor);
    for (const TableGrid::Drawn &cell : cells)
        rect(cell.rect, cell.selected ? m_selectionColor : cell.header ? m_headerColor : m_backgroundColor);
    QSGTextNode *text = window()->createTextNode();
    text->setColor(m_textColor);
    text->setSelectionColor(m_selectionColor);
    text->setSelectionTextColor(m_selectedTextColor);
    text->setRenderType(m_nativeRendering ? QSGTextNode::NativeRendering : QSGTextNode::QtRendering);
    const qreal pad = m_grid->padding();
    for (const TableGrid::Drawn &cell : cells) {
        if (!cell.layout) continue;
        const QPointF origin(cell.rect.left() + pad, cell.rect.top() + std::round(pad / 2));
        text->addTextLayout(origin, cell.layout, cell.selectionLength > 0 ? cell.selectionStart : -1,
                            cell.selectionLength > 0 ? cell.selectionLength : -1);
    }
    root->appendChildNode(text);
    // Wavy underlines where the text view would draw them, one zigzag a line.
    for (const TableGrid::Drawn &cell : cells) {
        if (!cell.layout || cell.waves.isEmpty()) continue;
        const QPointF origin(cell.rect.left() + pad, cell.rect.top() + std::round(pad / 2));
        const QFontMetricsF metrics(cell.layout->font());
        const qreal height = qMax<qreal>(2, std::round(cell.layout->font().pixelSize() / 6.0));
        for (const auto &[from, to, color] : cell.waves) {
            for (int i = 0; i < cell.layout->lineCount(); ++i) {
                const QTextLine line = cell.layout->lineAt(i);
                const int start = qMax(from, line.textStart()), end = qMin(to, line.textStart() + line.textLength());
                if (start >= end) continue;
                const qreal left = origin.x() + line.cursorToX(start), right = origin.x() + line.cursorToX(end);
                const qreal base = origin.y() + line.y() + line.ascent() + qMax<qreal>(1, std::round(metrics.underlinePos()));
                const int steps = qMax(1, int(std::ceil((right - left) / height)));
                auto *geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), steps + 1);
                geometry->setDrawingMode(QSGGeometry::DrawLineStrip);
                geometry->setLineWidth(qMax<float>(1, float(std::round(cell.layout->font().pixelSize() / 14.0))));
                QSGGeometry::Point2D *points = geometry->vertexDataAsPoint2D();
                for (int k = 0; k <= steps; ++k)
                    points[k].set(float(qMin(right, left + k * height)), float(base + (k % 2 ? height : 0)));
                auto *node = new QSGGeometryNode;
                node->setGeometry(geometry);
                node->setFlag(QSGNode::OwnsGeometry);
                auto *material = new QSGFlatColorMaterial;
                material->setColor(color);
                node->setMaterial(material);
                node->setFlag(QSGNode::OwnsMaterial);
                root->appendChildNode(node);
            }
        }
    }
    return root;
}

void TableGridView::wheelEvent(QWheelEvent *event)
{
    if (!m_grid || m_grid->width() <= m_grid->viewWidth()) {
        event->ignore();
        return;
    }
    // A touchpad moves it as far as the fingers go. A wheel's click, whose
    // few pixels the system also reports, moves it as far as the text
    // scrolls for one: the desktop's lines per click.
    QPointF delta = event->pixelDelta();
    if (event->phase() == Qt::NoScrollPhase || delta.isNull())
        delta = QPointF(event->angleDelta()) / 120.0 * QGuiApplication::styleHints()->wheelScrollLines()
              * m_grid->lineHeight();
    // Shift turns the wheel's turn into a sideways one, as everywhere.
    qreal across = delta.x(), down = delta.y();
    if (qFuzzyIsNull(across) && (event->modifiers() & Qt::ShiftModifier)) std::swap(across, down);
    // A scroll more up or down than sideways is the text's, which scrolls
    // on over the table.
    if (qFuzzyIsNull(across) || qAbs(down) > qAbs(across)) {
        event->ignore();
        return;
    }
    m_grid->setScrollX(m_grid->scrollX() - across);
    event->accept();
}

// ---- Commands -----------------------------------------------------------------

namespace {

const char *const tableCommandIds[] = {
    "insertTableRowAbove", "insertTableRowBelow", "duplicateTableRow", "moveTableRowUp", "moveTableRowDown",
    "deleteTableRow", "insertTableColumnLeft", "insertTableColumnRight", "moveTableColumnLeft",
    "moveTableColumnRight", "alignTableColumnDefault", "alignTableColumnLeft", "alignTableColumnCenter",
    "alignTableColumnRight", "sortTableAscending", "sortTableDescending", "deleteTableColumn", "mergeTableCells",
    "splitTableCell", "showTableMarkdown", "showTableGrid", "deleteTable",
};

} // namespace

QStringList TableGrids::commands()
{
    QStringList ids;
    for (const char *id : tableCommandIds) ids.append(QString::fromLatin1(id));
    return ids;
}

bool TableGrids::isCommand(const QString &id) { return commands().contains(id); }

std::optional<TableGrids::Span> TableGrids::span(const TableGrid *grid) const
{
    Span out;
    if (grid && m_picked.grid == grid && m_picked.anchor == anchor() && m_picked.cursor == cursor()) {
        if (m_picked.firstRow >= 0)
            return Span{m_picked.firstRow, m_picked.lastRow, 0, grid->columnCount() - 1};
        return Span{0, grid->rowCount() - 1, m_picked.firstColumn, m_picked.lastColumn};
    }
    if (block(grid, &out.firstRow, &out.lastRow, &out.firstColumn, &out.lastColumn)) return out;
    const TableGrid::Place place = grid->placeOf(cursor());
    if (!place.valid()) return std::nullopt;
    const MdTable::Cell &cell = grid->cellAt(place);
    out.firstRow = out.lastRow = place.row;
    out.firstColumn = cell.column;
    out.lastColumn = cell.column + cell.span - 1;
    return out;
}

std::optional<TableGrids::Span> TableGrids::selectedCells(const TableGrid *grid) const
{
    if (m_selectAll.level == 2 && m_selectAll.anchor == anchor() && m_selectAll.cursor == cursor())
        return Span{0, grid->rowCount() - 1, 0, grid->columnCount() - 1};
    Span out;
    if (block(grid, &out.firstRow, &out.lastRow, &out.firstColumn, &out.lastColumn)) return out;
    // A handle can select a row or column made of a single logical cell.
    if (m_picked.grid == grid && m_picked.anchor == anchor() && m_picked.cursor == cursor()) return span(grid);
    return std::nullopt;
}

std::optional<bool>
TableGrids::formatCells(const std::function<std::optional<TextChange>(QStringView, int, int)> &operation, bool inCells)
{
    TableGrid *grid = gridAt(cursor());
    int r0 = 0, r1 = 0, c0 = 0, c1 = 0;
    if (!grid || !block(grid, &r0, &r1, &c0, &c1)) return std::nullopt;
    if (!m_binding->editable()) return false;
    if (!inCells) {
        emit refused(tr("Only text formatting, such as bold or a link, goes into every cell of a block."));
        return false;
    }
    const MdTable::Dialect dialect = this->dialect();
    return run(changeBlock(grid, [&](MdTable::Model &model) {
        for (int r = r0; r <= r1; ++r) {
            int column = 0;
            for (MdTable::Item &item : model.rows[r]) {
                const int first = column;
                column += item.span;
                if (first + item.span - 1 < c0 || first > c1 || item.text.isEmpty()) continue;
                const std::optional<TextChange> made = operation(item.text, 0, int(item.text.size()));
                if (!made || made->edits.isEmpty()) continue;
                QList<SourceEdit> edits = made->edits;
                std::sort(edits.begin(), edits.end(),
                          [](const SourceEdit &a, const SourceEdit &b) { return a.range.start < b.range.start; });
                QString text = item.text;
                for (auto e = edits.crbegin(); e != edits.crend(); ++e)
                    text.replace(e->range.start, e->range.length, e->replacement);
                item.text = MdTable::encodeCell(text, dialect);
            }
        }
        return QString();
    }, {r0, r1, c0, c1}));
}

QList<SourceEdit> TableGrids::tidyCellEdits(QStringView text, const QList<SourceEdit> &edits,
                                            const MdTable::Dialect &dialect)
{
    struct InCell {
        int row = 0, cell = 0;
        SourceEdit edit;
    };
    struct Pending {
        MdTable::Table table;
        QList<InCell> edits;
    };
    QList<SourceEdit> out;
    QList<Pending> tables;
    for (const SourceEdit &edit : edits) {
        const int from = edit.range.start, to = from + edit.range.length;
        const auto table = MdTable::locate(text, from, dialect);
        const int row = table && table->unfit == MdTable::Unfit::None ? MdTable::rowAt(*table, from) : -1;
        const int cell = row >= 0 && row != 1 ? MdTable::cellAt(*table, row, from) : -1;
        const MdTable::Cell *in = cell >= 0 && cell < table->rows[row].cells.size() ? &table->rows[row].cells[cell] : nullptr;
        if (!in || in->missing || from < in->start || to > in->end) {
            out.append(edit);
            continue;
        }
        auto known = std::find_if(tables.begin(), tables.end(),
                                  [&](const Pending &pending) { return pending.table.start == table->start; });
        if (known == tables.end()) {
            tables.append({*table, {}});
            known = tables.end() - 1;
        }
        known->edits.append({row, cell, edit});
    }
    for (Pending &pending : tables) {
        const MdTable::Table &table = pending.table;
        // A table that other edits reach into is left to them as they are.
        const bool touched = std::any_of(out.cbegin(), out.cend(), [&](const SourceEdit &edit) {
            return edit.range.start <= table.end && edit.range.start + edit.range.length >= table.start;
        });
        MdTable::Model model = MdTable::model(text, table);
        std::sort(pending.edits.begin(), pending.edits.end(),
                  [](const InCell &a, const InCell &b) { return a.edit.range.start > b.edit.range.start; });
        for (const InCell &in : std::as_const(pending.edits)) {
            const MdTable::Cell &cell = table.rows[in.row].cells[in.cell];
            QString &item = model.rows[MdTable::modelRowOf(in.row)][in.cell].text;
            item.replace(in.edit.range.start - cell.start, in.edit.range.length, in.edit.replacement);
        }
        for (const InCell &in : std::as_const(pending.edits)) {
            QString &item = model.rows[MdTable::modelRowOf(in.row)][in.cell].text;
            item = MdTable::encodeCell(item, dialect);
        }
        const MdTable::Layout layout = MdTable::write(model);
        bool readsBack = true;
        const int columns = MdTable::columnCount(model);
        for (int r = 0; readsBack && r < model.rows.size(); ++r)
            readsBack = MdTable::readsBack(layout, model, r, dialect, columns);
        if (touched || !readsBack) {
            for (const InCell &in : std::as_const(pending.edits)) out.append(in.edit);
            continue;
        }
        const QStringView before = text.mid(table.start, table.end - table.start);
        if (before != layout.text) out.append(coveringEdit(table.start, before, layout.text));
    }
    std::sort(out.begin(), out.end(), [](const SourceEdit &a, const SourceEdit &b) { return a.range.start < b.range.start; });
    return out;
}

TableGrids::Adapted TableGrids::changeBlock(const TableGrid *grid, const std::function<QString(MdTable::Model &)> &operation,
                                            const Span &selected) const
{
    Adapted out = change(grid, operation, selected.firstRow, selected.firstColumn);
    if (out.kind != Adapted::Changed) return out;
    if (selected.firstRow == selected.lastRow && selected.firstColumn == selected.lastColumn) return out;
    // The block is selected again where the change left it: from its first cell to its last.
    Adapted last = change(grid, operation, selected.lastRow, selected.lastColumn, true);
    out.cursor = last.cursor;
    return out;
}

const TableGrids::RawTable *TableGrids::rawTableAt(int position) const
{
    for (const RawTable &table : m_rawTables)
        if (table.contains(position)) return &table;
    return nullptr;
}

bool TableGrids::canShowGrid() const
{
    if (!m_enabled || !m_binding->m_loaded) return false;
    const RawTable *table = rawTableAt(cursor());
    return table && (!table->waiting || settled());
}

void TableGrids::updateShowGrid()
{
    const bool can = canShowGrid();
    if (can == m_canShowGrid) return;
    m_canShowGrid = can;
    emit canShowGridChanged();
}

bool TableGrids::can(const QString &command) const
{
    if (!m_binding->m_loaded) return false;
    if (command == u"showTableGrid") return canShowGrid();
    TableGrid *grid = gridAt(cursor());
    if (!grid) return false;
    const bool editable = m_binding->editable();
    if (command == u"showTableMarkdown") return true;
    const auto selected = span(grid);
    if (!selected || !editable) return false;
    const int rows = grid->rowCount();
    const int columns = grid->columnCount();
    const bool body = selected->firstRow > 0;
    if (command == u"insertTableRowAbove" || command == u"duplicateTableRow" || command == u"deleteTableRow") return body;
    if (command == u"insertTableRowBelow") return true;
    if (command == u"moveTableRowUp") return selected->firstRow > 1;
    if (command == u"moveTableRowDown") return body && selected->lastRow + 1 < rows;
    if (command == u"moveTableColumnLeft") return selected->firstColumn > 0;
    if (command == u"moveTableColumnRight") return selected->lastColumn + 1 < columns;
    if (command == u"deleteTableColumn") return selected->lastColumn - selected->firstColumn + 1 < columns;
    if (command == u"sortTableAscending" || command == u"sortTableDescending") return rows > 2;
    if (command == u"mergeTableCells") {
        if (!dialect().mergedCells) return false;
        if (selected->lastColumn > selected->firstColumn) return true;
        return selected->lastColumn + 1 < columns;
    }
    if (command == u"splitTableCell") {
        const TableGrid::Place place = grid->placeOf(cursor());
        return place.valid() && grid->cellAt(place).span > 1;
    }
    return isCommand(command);
}

bool TableGrids::runCommand(const QString &command)
{
    if (!can(command)) return false;
    const QString source = text();
    if (command == u"showTableGrid") {
        const RawTable *raw = rawTableAt(cursor());
        if (!raw) return false;
        const auto table = MdTable::scan(source, raw->start, raw->end, dialect());
        return table && adoptOne(*table);
    }
    TableGrid *grid = gridAt(cursor());
    if (command == u"showTableMarkdown") {
        // This view shows the table's Markdown, until it is shown as a grid again.
        m_markdown.append(grid->start());
        m_rawTables.append({grid->start(), grid->end(), false});
        const int caret = cursor();
        unproject(grid->start(), grid->end());
        m_grids.removeAll(grid);
        if (m_caretGrid == grid) m_caretGrid = nullptr;
        grid->deleteLater();
        emit gridsChanged();
        placeGrids();
        const int shown = m_binding->sourceToEditor(caret);
        m_binding->selectEditor(shown, shown);
        m_binding->refreshCursor();
        emit caretChanged();
        updateShowGrid();
        // The layout of the raw rows determines how far to scroll.
        m_binding->queueTableLayout(true);
        return true;
    }
    if (command == u"deleteTable") {
        Adapted gone;
        gone.kind = Adapted::Changed;
        const int end = grid->end() + breakAfter(source, grid->end());
        gone.edits.append({{grid->start(), end - grid->start()}, QString()});
        gone.anchor = gone.cursor = grid->start();
        return run(gone);
    }
    const Span selected = *span(grid);
    const int rowCount = selected.lastRow - selected.firstRow + 1;
    const int columnCount = selected.lastColumn - selected.firstColumn + 1;
    const QString newline = m_binding->m_document ? m_binding->m_document->newline() : QStringLiteral("\n");
    const MdTable::Dialect dialect = this->dialect();
    using Model = MdTable::Model;
    const auto shifted = [&](int rows, int columns) {
        Span moved = selected;
        moved.firstRow += rows;
        moved.lastRow += rows;
        moved.firstColumn += columns;
        moved.lastColumn += columns;
        return moved;
    };
    const int r0 = selected.firstRow, r1 = selected.lastRow, c0 = selected.firstColumn, c1 = selected.lastColumn;
    if (command == u"insertTableRowAbove")
        return run(change(grid, [&](Model &m) { return MdTable::insertRows(m, r0, 1, newline); }, r0, c0));
    if (command == u"insertTableRowBelow")
        return run(change(grid, [&](Model &m) { return MdTable::insertRows(m, r1 + 1, 1, newline); }, r1 + 1, c0));
    if (command == u"duplicateTableRow")
        return run(change(grid, [&](Model &m) { return MdTable::duplicateRows(m, r0, rowCount, newline); }, r1 + 1, c0));
    if (command == u"moveTableRowUp")
        return run(changeBlock(grid, [&](Model &m) { return MdTable::moveRows(m, r0, rowCount, r0 - 1); }, shifted(-1, 0)));
    if (command == u"moveTableRowDown")
        return run(changeBlock(grid, [&](Model &m) { return MdTable::moveRows(m, r0, rowCount, r1 + 2); }, shifted(1, 0)));
    if (command == u"deleteTableRow") {
        const int rows = grid->rowCount();
        return run(change(grid, [&](Model &m) { return MdTable::deleteRows(m, r0, rowCount); },
                          qMin(r0, rows - 1 - rowCount), c0));
    }
    if (command == u"insertTableColumnLeft")
        return run(change(grid, [&](Model &m) { return MdTable::insertColumns(m, c0, 1); }, r0, c0));
    if (command == u"insertTableColumnRight")
        return run(change(grid, [&](Model &m) { return MdTable::insertColumns(m, c1 + 1, 1); }, r0, c1 + 1));
    // Columns a handle picked stay picked where they go.
    const bool picked = m_picked.firstRow < 0 && m_picked.grid == grid
                        && m_picked.anchor == anchor() && m_picked.cursor == cursor();
    const auto moveColumns = [&](int to, int by) {
        if (!run(changeBlock(grid, [&](Model &m) { return MdTable::moveColumns(m, c0, columnCount, to); },
                             shifted(0, by))))
            return false;
        if (picked) m_picked = {grid, anchor(), cursor(), c0 + by, c1 + by};
        return true;
    };
    if (command == u"moveTableColumnLeft") return moveColumns(c0 - 1, -1);
    if (command == u"moveTableColumnRight") return moveColumns(c1 + 2, 1);
    if (command == u"deleteTableColumn") {
        const int columns = grid->columnCount();
        return run(change(grid, [&](Model &m) { return MdTable::deleteColumns(m, c0, columnCount); }, r0,
                          qMin(c0, columns - 1 - columnCount)));
    }
    const auto align = [&](MdTable::Align to) {
        if (!run(changeBlock(grid, [&](Model &m) { return MdTable::setAlignment(m, c0, columnCount, to); }, selected)))
            return false;
        if (picked) m_picked = {grid, anchor(), cursor(), c0, c1};
        return true;
    };
    if (command == u"alignTableColumnDefault") return align(MdTable::Align::None);
    if (command == u"alignTableColumnLeft") return align(MdTable::Align::Left);
    if (command == u"alignTableColumnCenter") return align(MdTable::Align::Center);
    if (command == u"alignTableColumnRight") return align(MdTable::Align::Right);
    if (command == u"sortTableAscending" || command == u"sortTableDescending") {
        const bool descending = command == u"sortTableDescending";
        Document *document = m_binding->m_document;
        const ParsedTextPtr parsed = document->parse()->current();
        const md::Options options = document->markdownOptions();
        return run(change(grid, [&](Model &m) {
            QStringList keys;
            for (int row = 1; row < m.rows.size(); ++row) {
                const int item = MdTable::itemAt(m.rows[row], c0);
                QString key;
                if (item >= 0 && !m.rows[row][item].text.isEmpty())
                    readCellInlines(parsed, grid->cellAt({row, item}), m.rows[row][item].text, options,
                                    [&](const md::Document &, const md::Node &node) { key = md::inlineText(node, u' '); });
                keys.append(std::move(key));
            }
            return MdTable::sortRows(m, c0, descending, keys);
        }, 0, c0));
    }
    if (command == u"mergeTableCells") {
        const int last = c1 > c0 ? c1 : c1 + 1;
        return run(change(grid, [&](Model &m) {
            for (int row = r0; row <= r1; ++row) {
                const QString refused = MdTable::mergeCells(m, row, c0, last, dialect);
                if (!refused.isEmpty()) return refused;
            }
            return QString();
        }, r0, c0, true));
    }
    if (command == u"splitTableCell") {
        const TableGrid::Place place = grid->placeOf(cursor());
        const int column = grid->cellAt(place).column;
        const int row = place.row;
        return run(change(grid, [&](Model &m) { return MdTable::splitCell(m, row, column); }, row, column));
    }
    return false;
}

void TableGrids::selectRow(TableGrid *grid, int row, bool extend)
{
    if (!grid || row < 0 || row >= grid->rowCount()) return;
    int fromRow = row;
    if (extend) {
        const TableGrid::Place anchorPlace = grid->placeOf(anchor());
        if (anchorPlace.valid()) fromRow = anchorPlace.row;
    }
    const int first = qMin(fromRow, row), last = qMax(fromRow, row);
    const MdTable::Cell &from = grid->cellAt({first, 0});
    const MdTable::Cell &to = grid->cellAt({last, grid->cellCount(last) - 1});
    select(from.start, to.end);
    m_picked = {grid, anchor(), cursor(), 0, grid->columnCount() - 1, first, last};
}

void TableGrids::selectColumn(TableGrid *grid, int column, bool extend)
{
    if (!grid || column < 0 || column >= grid->columnCount()) return;
    int firstColumn = column, lastColumn = column;
    if (extend) {
        // From the columns picked before, or the anchor's.
        if (const auto picked = span(grid);
            picked && m_picked.firstRow < 0 && m_picked.grid == grid && m_picked.anchor == anchor()) {
            firstColumn = qMin(picked->firstColumn, column);
            lastColumn = qMax(picked->lastColumn, column);
        } else if (const TableGrid::Place anchorPlace = grid->placeOf(anchor()); anchorPlace.valid()) {
            const MdTable::Cell &from = grid->cellAt(anchorPlace);
            firstColumn = qMin(from.column, column);
            lastColumn = qMax(from.column + from.span - 1, column);
        }
    }
    const auto cellIn = [&](int row, int at) {
        TableGrid::Place place{row, 0};
        for (int i = 0; i < grid->cellCount(row); ++i)
            if (grid->cellAt({row, i}).column <= at) place.cell = i;
        return grid->cellAt(place);
    };
    const int last = grid->rowCount() - 1;
    const MdTable::Cell &from = cellIn(0, firstColumn);
    const MdTable::Cell &to = cellIn(last, lastColumn);
    select(from.start, to.end);
    m_picked = {grid, anchor(), cursor(), firstColumn, lastColumn};
}

bool TableGrids::moveRowsTo(TableGrid *grid, int to)
{
    if (!grid || !m_binding->editable()) return false;
    const auto selected = span(grid);
    if (!selected || selected->firstRow < 1) return false;
    const int first = selected->firstRow, count = selected->lastRow - selected->firstRow + 1;
    if (to >= first && to <= first + count) return false;
    const int landed = to > first ? to - count : to;
    Span moved = *selected;
    moved.firstRow = landed;
    moved.lastRow = landed + count - 1;
    return run(changeBlock(grid, [&](MdTable::Model &m) { return MdTable::moveRows(m, first, count, qMax(1, to)); }, moved));
}

bool TableGrids::moveColumnsTo(TableGrid *grid, int to)
{
    if (!grid || !m_binding->editable()) return false;
    const auto selected = span(grid);
    if (!selected) return false;
    const int first = selected->firstColumn, count = selected->lastColumn - selected->firstColumn + 1;
    if (to >= first && to <= first + count) return false;
    const int landed = to > first ? to - count : to;
    Span moved = *selected;
    moved.firstColumn = landed;
    moved.lastColumn = landed + count - 1;
    return run(changeBlock(grid, [&](MdTable::Model &m) { return MdTable::moveColumns(m, first, count, to); }, moved));
}

bool TableGrids::appendRow(TableGrid *grid)
{
    if (!grid || !m_binding->editable()) return false;
    const int rows = grid->rowCount();
    const QString newline = m_binding->m_document ? m_binding->m_document->newline() : QStringLiteral("\n");
    return run(change(grid, [&](MdTable::Model &m) { return MdTable::insertRows(m, rows, 1, newline); }, rows, 0));
}

bool TableGrids::appendColumn(TableGrid *grid)
{
    if (!grid || !m_binding->editable()) return false;
    const int columns = grid->columnCount();
    return run(change(grid, [&](MdTable::Model &m) { return MdTable::insertColumns(m, columns, 1); }, 0, columns));
}

void TableGrids::adoptAt(int position)
{
    if (!m_enabled || !m_binding->m_loaded) return;
    const auto table = MdTable::locate(text(), position, dialect());
    if (table) adoptOne(*table);
}

bool TableGrids::adoptOne(const MdTable::Table &table)
{
    if (table.unfit != MdTable::Unfit::None || gridAt(table.start)) return false;
    m_rawTables.removeIf([&table](const RawTable &raw) { return raw.start == table.start; });
    if (m_waitingTable && m_waitingTable->start == table.start) m_waitingTable.reset();
    m_markdown.removeAll(table.start);
    auto *grid = new TableGrid(this);
    grid->read(table, text());
    m_grids.append(grid);
    std::sort(m_grids.begin(), m_grids.end(), [](TableGrid *a, TableGrid *b) { return a->start() < b->start(); });
    project(grid);
    placeGrids();
    emit gridsChanged();
    cursorMoved();
    return true;
}

// ---- The clipboard -------------------------------------------------------------

namespace {

const QString tsvMime = QStringLiteral("text/tab-separated-values");
const QString libreOfficeMime = QStringLiteral("application/x-libreoffice-tsvc");
const QString cellsMime = QStringLiteral("application/x-parch-table-cells");
const QByteArray cellsHeader = QByteArrayLiteral("parch-table-cells 1\n");

struct ClipboardCells {
    QList<QStringList> rows;
    bool markdown = false;
    bool tooLarge = false;
    bool plainFallback = false;
};

// Quoted fields keep tabs, newlines and doubled quotes. Each record
// terminator finishes one row, including an actual empty row.
std::optional<ClipboardCells> splitSeparated(QStringView text)
{
    if (text.isEmpty() || text.contains(QChar::Null)) return std::nullopt;
    const ClipboardCells tooLarge{{}, false, true};
    ClipboardCells out;
    QStringList row;
    QString field;
    bool quoted = false, started = false, closed = false;
    qsizetype columns = 0;
    const auto finishField = [&] {
        row.append(std::exchange(field, {}));
        started = closed = false;
        return row.size() <= MdTable::MaxColumns;
    };
    const auto finishRow = [&] {
        columns = qMax(columns, row.size());
        if ((out.rows.size() + 1) * columns > MdTable::MaxCells) return false;
        out.rows.append(std::exchange(row, {}));
        return true;
    };
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text[i];
        if (quoted) {
            if (c == u'"' && i + 1 < text.size() && text[i + 1] == u'"') {
                field += u'"';
                ++i;
            } else if (c == u'"') {
                quoted = false;
                closed = true;
            } else {
                field += c;
            }
        } else if (c == u'\t') {
            if (!finishField()) return tooLarge;
        } else if (isBreak(c)) {
            if (c == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') ++i;
            if (!finishField() || !finishRow()) return tooLarge;
        } else if (closed) {
            return std::nullopt;
        } else if (c == u'"' && !started) {
            quoted = started = true;
        } else {
            field += c;
            started = true;
        }
    }
    if (quoted) return std::nullopt;
    if ((started || !row.isEmpty()) && (!finishField() || !finishRow())) return tooLarge;
    return out;
}

std::optional<ClipboardCells> typedSheet(QByteArrayView payload, bool keepBom = false)
{
    if (payload.size() > DiskFile::MaxLoadBytes) return ClipboardCells{{}, false, true};
    QStringConverter::Flags flags = QStringConverter::Flag::Stateless;
    if (keepBom) flags |= QStringConverter::Flag::ConvertInitialBom;
    QStringDecoder decode(QStringDecoder::Utf8, flags);
    const QString text = decode(payload);
    if (decode.hasError()) return std::nullopt;
    return splitSeparated(text);
}

std::optional<ClipboardCells> readCellSource(const QByteArray &payload, const MdTable::Dialect &dialect)
{
    if (!payload.startsWith(cellsHeader)) return std::nullopt;
    // The version line identifies a rectangle of quoted Markdown fields.
    // Read it with the same bounds as a spreadsheet, then validate the rows.
    auto cells = typedSheet(QByteArrayView(payload).sliced(cellsHeader.size()), true);
    if (!cells || cells->tooLarge || cells->rows.isEmpty()) return std::nullopt;
    const qsizetype columns = cells->rows.first().size();
    for (const QStringList &row : std::as_const(cells->rows)) {
        if (row.size() != columns) return std::nullopt;
        QList<MdTable::Item> items;
        for (const QString &text : row) {
            if (std::any_of(text.cbegin(), text.cend(), [](QChar c) {
                    return isBreak(c) || c == QChar::LineSeparator || c == QChar::ParagraphSeparator;
                })) return std::nullopt;
            items.append(MdTable::Item{text});
        }
        if (!MdTable::readsBack(items, int(columns), dialect)) return std::nullopt;
    }
    cells->markdown = true;
    return cells;
}

std::optional<ClipboardCells> cellsOf(const QMimeData *mime, bool block, bool allowPlain,
                                     const MdTable::Dialect &dialect)
{
    if (mime->hasFormat(cellsMime))
        if (auto cells = readCellSource(mime->data(cellsMime), dialect)) return cells;
    if (mime->hasFormat(tsvMime))
        if (auto sheet = typedSheet(mime->data(tsvMime))) return sheet;
    if (mime->hasFormat(libreOfficeMime)) {
        if (auto sheet = typedSheet(mime->data(libreOfficeMime))) return sheet;
        if (auto sheet = splitSeparated(mime->text())) return sheet;
    }
    const QString markup = mime->hasHtml() ? mime->html() : QString();
    if (markup.size() > HtmlMarkdown::SynchronousChars) {
        if (!markup.contains(u"<table", Qt::CaseInsensitive)) return std::nullopt;
        const QString text = mime->text();
        if (text.contains(u'\t'))
            if (auto sheet = splitSeparated(text)) return sheet;
        if (allowPlain && !text.isEmpty()) return ClipboardCells{{}, false, false, true};
        return std::nullopt;
    }
    const auto html = markup.isEmpty() ? std::nullopt : HtmlMarkdown::firstTableCells(markup);
    if (html && html->tooLarge) return ClipboardCells{{}, false, true};
    if (html && !html->rows.isEmpty()) {
        // A simple HTML table may be the companion to spreadsheet values.
        // Spans and additional tables make the HTML's boundaries decisive.
        if (!html->hasGeometry && mime->hasText()) {
            const QString text = mime->text();
            if (text.contains(u'\t'))
                if (auto sheet = splitSeparated(text)) return sheet;
        }
        return ClipboardCells{html->rows};
    }
    if (allowPlain && (!mime->hasHtml() || html)) {
        const QString text = mime->text();
        if (block && !text.isEmpty()) {
            if (text.contains(u'\t'))
                if (auto sheet = splitSeparated(text)) return sheet;
            return ClipboardCells{{{text}}};
        }
        if (html) return ClipboardCells{{}, false, false, true};
    }
    return std::nullopt;
}

QString quotedField(const QString &text)
{
    if (!text.contains(u'\t') && !text.contains(u'\n') && !text.contains(u'\r') && !text.contains(u'"')) return text;
    return u'"' + QString(text).replace(QStringLiteral("\""), QStringLiteral("\"\"")) + u'"';
}

} // namespace

bool TableGrids::copyBlock()
{
    const TableGrid *grid = gridAt(cursor());
    if (!grid) return false;
    const auto selected = selectedCells(grid);
    if (!selected) return false;

    const QString source = text();
    Document *document = m_binding->m_document;
    const ParsedTextPtr parsed = document->parse()->current();
    const md::Options options = document->markdownOptions();
    QStringList lines, sourceLines;
    QString html = QStringLiteral("<table>");
    for (int r = selected->firstRow; r <= selected->lastRow; ++r) {
        QStringList fields, markdownFields;
        html += QStringLiteral("<tr>");
        int item = 0;
        for (int c = selected->firstColumn; c <= selected->lastColumn; ++c) {
            while (item + 1 < grid->cellCount(r) && grid->cellAt({r, item + 1}).column <= c) ++item;
            const MdTable::Cell &cell = grid->cellAt({r, item});
            // A merged cell contributes one value to the rectangle.
            const QString markdown = cell.column == c || c == selected->firstColumn
                ? source.mid(cell.start, cell.end - cell.start) : QString();
            QString plain, rich;
            if (!markdown.isEmpty())
                readCellInlines(parsed, cell, markdown, options, [&](const md::Document &tree, const md::Node &node) {
                    plain = md::inlineText(node, u'\n');
                    rich = md::cellHtml(tree, node);
                });
            fields.append(quotedField(plain));
            const char *tag = r == 0 ? "th" : "td";
            html += QStringLiteral("<%1>%2</%1>").arg(QLatin1String(tag), rich);
            markdownFields.append(quotedField(markdown));
        }
        html += QStringLiteral("</tr>");
        lines.append(fields.join(u'\t'));
        sourceLines.append(markdownFields.join(u'\t'));
    }
    html += QStringLiteral("</table>");
    const QString tsv = lines.join(u'\n') + u'\n';
    auto *mime = new QMimeData;
    mime->setText(tsv);
    mime->setData(tsvMime, tsv.toUtf8());
    mime->setHtml(html);
    mime->setData(cellsMime, cellsHeader + (sourceLines.join(u'\n') + u'\n').toUtf8());
    QGuiApplication::clipboard()->setMimeData(mime);
    return true;
}

bool TableGrids::cutBlock()
{
    TableGrid *grid = gridAt(cursor());
    if (!grid || !m_binding->editable()) return false;
    const auto selected = selectedCells(grid);
    if (!selected) return false;
    copyBlock();
    const auto [r0, r1, c0, c1] = *selected;
    run(change(grid, [&](MdTable::Model &m) { return MdTable::clearCells(m, r0, r1, c0, c1); }, r0, c0));
    return true;
}

TableGrids::Paste TableGrids::pasteCells(const QMimeData *mime, bool allowPlain)
{
    TableGrid *grid = gridAt(cursor());
    if (!grid) return Paste::NotCells;
    const auto selected = selectedCells(grid);
    const MdTable::Dialect dialect = this->dialect();
    const auto input = cellsOf(mime, selected.has_value(), allowPlain, dialect);
    if (!input) return Paste::NotCells;
    if (input->plainFallback) return Paste::PlainText;
    const auto tooBig = [&] {
        emit refused(tooLarge());
        return Paste::Refused;
    };
    if (input->tooLarge) return tooBig();
    if (input->rows.isEmpty()) return Paste::NotCells;
    if (!m_binding->editable()) return Paste::Refused;
    const auto done = [](bool pasted) { return pasted ? Paste::Pasted : Paste::Refused; };
    const auto &cells = input->rows;
    if (selected && cells.size() == 1 && cells.first().size() == 1 && !cells.first().first().isEmpty()) {
        const auto [r0, r1, c0, c1] = *selected;
        const QString value = input->markdown ? cells.first().first() : MdTable::encodeCell(cells.first().first(), dialect);
        return done(run(changeBlock(grid, [&](MdTable::Model &model) {
            return MdTable::fillCells(model, r0, r1, c0, c1, value);
        }, *selected)));
    }

    const TableGrid::Place place = grid->placeOf(cursor());
    if (!selected && !place.valid()) return Paste::NotCells;
    const int row = selected ? selected->firstRow : place.row;
    const int column = selected ? selected->firstColumn : grid->cellAt(place).column;
    int width = 0;
    for (const QStringList &line : cells) width = qMax(width, int(line.size()));
    const int columns = qMax(grid->columnCount(), column + width);
    const int rows = qMax(grid->rowCount(), row + int(cells.size()));
    if (columns > MdTable::MaxColumns || qsizetype(columns) * rows > MdTable::MaxCells) return tooBig();
    const QString newline = m_binding->m_document->newline();
    const Span pasted{row, row + int(cells.size()) - 1, column, column + width - 1};
    return done(run(changeBlock(grid, [&](MdTable::Model &model) {
        return MdTable::pasteCells(model, row, column, cells, dialect, newline, input->markdown);
    }, pasted)));
}

void TableGrids::selectWordAt(int position)
{
    TableGrid *grid = gridAt(position);
    const TableGrid::Place place = grid ? grid->placeOf(position) : TableGrid::Place{};
    if (!place.valid()) return;
    const MdTable::Cell &cell = grid->cellAt(place);
    const QString words = text().mid(cell.start, cell.end - cell.start);
    const int offset = qBound(0, position - cell.start, int(words.size()));
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, words);
    finder.setPosition(offset);
    int start = offset;
    if (!finder.isAtBoundary() || !(finder.boundaryReasons() & QTextBoundaryFinder::StartOfItem))
        start = qMax(0, int(finder.toPreviousBoundary()));
    finder.setPosition(start);
    const qsizetype end = finder.toNextBoundary();
    select(cell.start + start, cell.start + (end < 0 ? int(words.size()) : int(end)));
}

void TableGrids::selectCellAt(int position)
{
    TableGrid *grid = gridAt(position);
    const TableGrid::Place place = grid ? grid->placeOf(position) : TableGrid::Place{};
    if (!place.valid()) return;
    select(grid->cellAt(place).start, grid->cellAt(place).end);
}

void TableGrids::compositionChanged()
{
    // The cell the text was shown in, and the one it is in now, are laid out again.
    const auto relay = [](TableGrid *grid, TableGrid::Place place) {
        if (!grid || !place.valid() || place.row >= grid->rowCount() || place.cell >= grid->cellCount(place.row)) return;
        const int index = grid->indexOf(place);
        if (index >= 0 && size_t(index) < grid->m_cells.size()) grid->m_cells[size_t(index)].layout.reset();
        emit grid->contentChanged();
        grid->updateCaret();
    };
    relay(m_preeditGrid, m_preeditPlace);
    m_preeditGrid = nullptr;
    m_preeditPlace = {};
    if (m_binding->preedit().isEmpty()) return;
    TableGrid *grid = gridAt(m_binding->preeditAt());
    if (!grid) return;
    m_preeditGrid = grid;
    m_preeditPlace = grid->placeOf(m_binding->preeditAt());
    relay(grid, m_preeditPlace);
}

