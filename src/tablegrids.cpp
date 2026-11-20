#include "tablegrids.h"

#include "document.h"
#include "documentparse.h"
#include "editorbinding.h"
#include "editordecorations.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"

#include <QAbstractTextDocumentLayout>
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
    for (int i = 0; i < cells.size(); ++i)
        if (position >= cells[i].rawStart && position <= cells[i].rawEnd) return {modelRow, i};
    return {};
}

qreal TableGrid::padding() const { return std::round(m_owner->lineHeight() * 0.4); }

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
    if (EditorDecorations *decorations = m_owner->decorations())
        cell.layout->setFormats(decorations->formatsFor(source.start, int(cell.text.size())));
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
    bool any = false;
    for (int row = 0; row < m_rowFirst.size(); ++row) {
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
    const QRectF rect = cellRect(place);
    const qreal lineHeight = m_owner->lineHeight();
    const int line = int(std::floor((y - rect.top() - padding() / 2) / lineHeight));
    return positionAtLine(place, qBound(0, line, lineCount(place) - 1), x);
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
    m_table = table;
    std::vector<CellText> cells;
    m_rowFirst.clear();
    for (int r = 0; r < table.rows.size(); ++r) {
        if (r == 1) continue;
        m_rowFirst.append(int(cells.size()));
        for (const MdTable::Cell &cell : table.rows[r].cells) {
            CellText item;
            item.text = text.mid(cell.start, cell.end - cell.start).toString();
            // A cell whose text is unchanged keeps its layout.
            const size_t previous = cells.size();
            if (previous < m_cells.size() && m_cells[previous].text == item.text) {
                item.layout = std::move(m_cells[previous].layout);
                item.width = m_cells[previous].width;
                item.lines = m_cells[previous].lines;
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
            const QString &text = m_cells[size_t(indexOf({row, i}))].text;
            const qreal width = std::ceil(m.horizontalAdvance(text));
            qreal word = 0;
            for (const QStringView part : QStringView(text).split(u' ', Qt::SkipEmptyParts))
                word = qMax(word, std::ceil(m.horizontalAdvance(part.toString())));
            // A word longer than the column at its widest wraps anywhere.
            word = qMin(word, 30 * least / 3);
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
        const QFontMetricsF &m = row == 0 ? headerMetrics : metrics;
        for (int i = 0; i < cellCount(row); ++i) {
            const MdTable::Cell &cell = cellAt({row, i});
            if (cell.column >= columns) continue;
            CellText &text = m_cells[size_t(indexOf({row, i}))];
            const auto [left, right] = extent(cell);
            const qreal room = right - left - 2 * pad;
            if (m.horizontalAdvance(text.text) <= room) {
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
    if (caret.isNull()) caret = QRectF(0, 0, 0, lineHeight());
    return caret.translated(grid->x() - grid->scrollX(), grid->y());
}

int TableGrids::sourceAt(QPointF point) const
{
    for (TableGrid *grid : m_grids)
        if (point.y() >= grid->y() && point.y() < grid->y() + grid->height())
            return grid->sourceAt(point.x() - grid->x() + grid->scrollX(), point.y() - grid->y());
    return -1;
}

TableGrids::Spot TableGrids::spotOf(int position) const
{
    Spot spot;
    TableGrid *grid = gridAt(position);
    if (!grid) return spot;
    const TableGrid::Place place = grid->placeOf(position);
    if (!place.valid()) return spot;
    spot.grid = grid;
    spot.place = place;
    spot.offset = position - grid->cellAt(place).start;
    return spot;
}

std::optional<int> TableGrids::positionOf(const Spot &spot) const
{
    if (!spot.grid || !m_grids.contains(spot.grid.data())) return std::nullopt;
    const TableGrid *grid = spot.grid;
    if (spot.place.row >= grid->rowCount() || spot.place.cell >= grid->cellCount(spot.place.row)) return std::nullopt;
    const MdTable::Cell &cell = grid->cellAt(spot.place);
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

void TableGrids::setProblem(const QString &problem)
{
    if (problem == m_problem) return;
    m_problem = problem;
    emit problemChanged();
}

void TableGrids::clear()
{
    disconnect(m_parseConnection);
    m_waiting.clear();
    // The view lets go of the grids before they go.
    const QList<TableGrid *> gone = std::exchange(m_grids, {});
    m_caretGrid = nullptr;
    if (!gone.isEmpty()) emit gridsChanged();
    for (TableGrid *grid : gone) grid->deleteLater();
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
    if (!m_binding->m_document->markdownOptions().tables) {
        adopt({});
        return;
    }
    QList<const md::Node *> nodes;
    collectTables(parse->document().root, nodes);
    const QString source = text();
    const MdTable::Dialect dialect = this->dialect();
    QList<MdTable::Table> tables;
    m_waiting.clear();
    const bool first = m_grids.isEmpty() && !m_binding->m_buffer->canUndo();
    for (const md::Node *node : std::as_const(nodes)) {
        const auto table = MdTable::scan(source, node->range.start, node->range.end, dialect);
        if (!table || table->unfit != MdTable::Unfit::None) continue;
        int rows = 0;
        for (const md::Node &row : node->children) rows += row.type == md::NodeType::TableRow;
        if (rows != table->rows.size() - 1 || node->columns != table->columns) continue;
        // A table the caret is in that was not a grid is being written: it
        // waits until the caret leaves it.
        const bool known = std::any_of(m_grids.cbegin(), m_grids.cend(),
                                       [&](TableGrid *grid) { return grid->start() == table->start; });
        const int caret = cursor();
        if (!known && !first && caret >= table->start && caret <= table->end) {
            m_waiting.append({table->start, table->end});
            continue;
        }
        tables.append(*table);
    }
    adopt(tables);
}

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

void TableGrids::sourceChanged(SourceRange range, int inserted)
{
    const int delta = inserted - range.length;
    const int oldEnd = range.start + range.length;
    for (auto &[start, end] : m_waiting) {
        if (oldEnd < start) {
            start += delta;
            end += delta;
        } else if (range.start <= end) {
            end += delta;
        }
    }
    if (m_grids.isEmpty()) return;
    const QString source = text();
    bool removed = false;
    for (qsizetype i = 0; i < m_grids.size(); ++i) {
        TableGrid *grid = m_grids[i];
        // Lines put in just before the table move it down, as text before it does.
        const bool linesBefore = range.length == 0 && range.start == grid->start() && inserted > 0
                                 && (source[range.start + inserted - 1] == u'\n' || source[range.start + inserted - 1] == u'\r');
        if (oldEnd < grid->start() || linesBefore) {
            shiftTable(grid->m_table, delta);
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
    // A table being written becomes a grid once the caret leaves it.
    if (!m_waiting.isEmpty() && !m_settling) {
        QList<std::pair<int, int>> waiting;
        bool left = false;
        for (const auto &[start, end] : std::as_const(m_waiting)) {
            if (cursor >= start && cursor <= end) waiting.append({start, end});
            else left = true;
        }
        if (left) {
            m_waiting = waiting;
            if (m_binding->m_document) m_binding->m_document->parse()->request();
            QMetaObject::invokeMethod(this, &TableGrids::parsed, Qt::QueuedConnection);
        }
    }
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
    if (grid && !m_settling && anchor != cursor) {
        // A selection from the text around a table into it takes the whole table.
        TableGrid *other = gridAt(anchor);
        if (other != grid) {
            const int start = anchor < cursor ? anchor : grid->end();
            const int end = anchor < cursor ? grid->end() : grid->start();
            const int to = anchor < cursor ? grid->end() : grid->start();
            if (cursor != to) {
                m_settling = true;
                select(anchor < cursor ? start : anchor, end);
                m_settling = false;
                return;
            }
        }
    }
    m_lastCursor = cursor;
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
    const auto selectCell = [&](TableGrid::Place to) {
        const MdTable::Cell &target = grid->cellAt(to);
        select(target.start, target.end);
    };
    if (mods == Qt::NoModifier && key == Qt::Key_Escape && cursor != anchor()) {
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
    if (plain && key == Qt::Key_Home) {
        moveTo(cell.start);
        return true;
    }
    if (plain && key == Qt::Key_End) {
        moveTo(cell.end);
        return true;
    }
    if (control && !shift && key == Qt::Key_A) {
        // The cell's text, then the whole table, then the whole text.
        const int from = qMin(cursor, anchor()), to = qMax(cursor, anchor());
        if (from == cell.start && to == cell.end && cell.end > cell.start) {
            select(grid->start(), grid->end());
            return true;
        }
        if (from <= grid->start() && to >= grid->end()) return false;
        if (cell.start == cell.end) {
            select(grid->start(), grid->end());
            return true;
        }
        select(cell.start, cell.end);
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
    int tail = 0;
    while (tail < shortest - same && before[before.size() - 1 - tail] == after[after.size() - 1 - tail]) ++tail;
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

// The lines of a text put between a table's rows, each a row of cells, or
// none when one is not a table row; empty lines are empty rows.
std::optional<QList<QStringList>> rowsOf(const QString &text, const MdTable::Dialect &dialect)
{
    QList<QStringList> rows;
    const QStringList lines = QString(text).replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace(u'\r', u'\n').split(u'\n');
    for (const QString &line : lines) {
        int content = 0;
        while (content < line.size() && (line[content] == u' ' || line[content] == u'\t' || line[content] == u'>'))
            ++content;
        QString trimmed = line.mid(content).trimmed();
        if (trimmed.isEmpty()) {
            rows.append(QStringList{});
            continue;
        }
        QList<MdTable::Field> fields;
        QList<int> pipes;
        MdTable::splitRow(trimmed, 0, int(trimmed.size()), dialect.protectCode, fields, &pipes);
        if (pipes.isEmpty()) return std::nullopt;
        QStringList cells;
        for (const MdTable::Field &field : std::as_const(fields)) cells.append(trimmed.mid(field.start, field.end - field.start));
        rows.append(cells);
    }
    return rows;
}

} // namespace

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
                // Whole table rows put there join the table, as a row put after its last.
                const bool rows = edit.replacement.endsWith(u'\n') || edit.replacement.endsWith(u'\r');
                const auto parsed = rows ? rowsOf(edit.replacement.trimmed(), dialect) : std::nullopt;
                if (parsed && from == blank) {
                    const QList<QStringList> values = *parsed;
                    const int at = grid->rowCount();
                    return change(grid, [&](MdTable::Model &model) {
                        MdTable::insertRows(model, at, int(values.size()), newline);
                        for (int i = 0; i < values.size(); ++i)
                            for (int c = 0; c < values[i].size() && c < MdTable::columnCount(model); ++c)
                                model.rows[at + i][MdTable::itemAt(model.rows[at + i], c)].text = values[i][c];
                        return QString();
                    }, at, 0);
                }
                Adapted below;
                below.kind = Adapted::Changed;
                below.edits.append({{from, 0}, newline + edit.replacement});
                below.anchor = below.cursor = from + int(newline.size() + edit.replacement.size());
                return below;
            }
        }
        if (to < grid->start() || from > grid->end()) continue;
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
                const auto parsed = rowsOf(lines, dialect);
                if (!parsed) return refuse(tr("Only table rows fit between the rows of a table."));
                // Model rows: the header is 0, and a row put before or after
                // the header or its delimiter goes first among the body.
                int at = before ? row - 1 : row;
                at = qMax(1, at);
                const QList<QStringList> values = *parsed;
                return change(grid, [&](MdTable::Model &model) {
                    MdTable::insertRows(model, at, int(values.size()), newline);
                    for (int i = 0; i < values.size(); ++i)
                        for (int c = 0; c < values[i].size() && c < MdTable::columnCount(model); ++c)
                            model.rows[at + i][MdTable::itemAt(model.rows[at + i], c)].text =
                                MdTable::encodeCell(values[i][c], dialect);
                    return QString();
                }, at, 0);
            }
        }
    }
    return std::nullopt;
}

TableGrids::Adapted TableGrids::adapt(const QList<SourceEdit> &edits, int anchor, int cursor) const
{
    Adapted out;
    if (m_grids.isEmpty()) return out;
    if (edits.size() == 1)
        if (std::optional<Adapted> one = adaptOne(edits.first())) return *one;
    const auto refuse = [&](const QString &reason) {
        Adapted refused;
        refused.kind = Adapted::Refused;
        refused.reason = reason;
        return refused;
    };
    // Each edit's grid and cell, or none.
    struct InCell {
        const TableGrid *grid;
        TableGrid::Place place;
        SourceEdit edit;
    };
    QList<SourceEdit> passing;
    QList<InCell> inCells;
    for (const SourceEdit &edit : edits) {
        const int from = edit.range.start, to = edit.range.start + edit.range.length;
        const TableGrid *grid = nullptr;
        for (const TableGrid *g : m_grids)
            if (to >= g->start() && from <= g->end()) grid = g;
        // An edit outside the grids, or of a table whole, is the writer's
        // to make; but none takes away the blank line after a table, which
        // would make the text after it the table's next row.
        if (!grid || (from <= grid->start() && to >= grid->end())) {
            for (const TableGrid *g : m_grids) {
                if (from <= g->start() && to >= g->end()) continue;
                const int blank = lineAfter(g);
                if (blank < 0 || from > blank + 1) continue;
                const QString source = text();
                int next = blank;
                while (next < source.size() && (source[next] == u' ' || source[next] == u'\t')) ++next;
                const int afterBlank = next + breakAfter(source, next);
                if (next < source.size() && breakAfter(source, next) == 0) continue;
                if (from < afterBlank && to > g->end() && edit.replacement.count(u'\n') + edit.replacement.count(u'\r') == 0)
                    return refuse(tr("A table keeps a blank line after it."));
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
        if (!place.valid() || !(place == grid->placeOf(to)))
            return refuse(tr("Select the whole table, or stay inside one cell."));
        inCells.append({grid, place, edit});
    }
    if (inCells.isEmpty()) return out;

    const QString source = text();
    const MdTable::Dialect dialect = this->dialect();
    out.kind = Adapted::Changed;
    out.anchor = anchor;
    out.cursor = cursor;
    // Where the selection was in the text the edits would have made, and
    // where it is in the one they make, are worked out table by table: a
    // position is moved by the edits before it, those of the table it is
    // in left out, which the table's own writing replaces.
    const auto naiveShift = [&](int position, const TableGrid *within) {
        int shift = 0;
        for (const SourceEdit &edit : edits) {
            const int end = edit.range.start + edit.range.length;
            if (end > position) continue;
            if (within && end >= within->start() && edit.range.start <= within->end()) continue;
            shift += int(edit.replacement.size()) - edit.range.length;
        }
        return shift;
    };
    QList<SourceEdit> written = passing;
    QList<const TableGrid *> grids;
    for (const InCell &in : std::as_const(inCells))
        if (!grids.contains(in.grid)) grids.append(in.grid);
    // Positions in the selection are placed once, in the first table that holds them.
    bool anchorPlaced = false, cursorPlaced = false;
    int placedAnchor = anchor, placedCursor = cursor;
    for (const TableGrid *grid : std::as_const(grids)) {
        MdTable::Model model = MdTable::model(source, grid->table());
        struct Cell {
            TableGrid::Place place;
            int naiveStart = 0;
            int naiveLength = 0;
            int lead = 0;
            QList<int> escapes; // offsets in the written text where a backslash was added
            QString text;
        };
        QList<Cell> cells;
        for (const InCell &in : std::as_const(inCells)) {
            if (in.grid != grid) continue;
            auto known = std::find_if(cells.begin(), cells.end(), [&](const Cell &c) { return c.place == in.place; });
            if (known != cells.end()) continue;
            const MdTable::Cell &cell = grid->cellAt(in.place);
            const int tail = tailOf(source, cell);
            // The cell's text with every edit of it, each clamped to the
            // text and the spaces after it.
            QString payload = source.mid(cell.start, cell.end - cell.start);
            int maxEnd = 0;
            QList<SourceEdit> own;
            for (const InCell &other : std::as_const(inCells)) {
                if (other.grid != grid || !(other.place == in.place)) continue;
                const int a = qBound(cell.start, other.edit.range.start, cell.end + tail) - cell.start;
                const int b = qBound(cell.start, other.edit.range.start + other.edit.range.length, cell.end + tail) - cell.start;
                own.append({{a, b - a}, other.edit.replacement});
                maxEnd = qMax(maxEnd, b);
            }
            if (maxEnd > payload.size()) payload += QString(maxEnd - payload.size(), u' ');
            for (auto e = own.crbegin(); e != own.crend(); ++e) {
                QString replacement = e->replacement;
                for (QChar &c : replacement)
                    if (c == u'\n' || c == u'\r' || c == QChar::ParagraphSeparator || c == QChar::LineSeparator) c = u' ';
                payload.replace(e->range.start, e->range.length, replacement);
            }
            Cell c;
            c.place = in.place;
            c.naiveStart = cell.start + naiveShift(grid->start(), grid) + [&] {
                // The table's own edits before this cell, in the text they would make.
                int shift = 0;
                for (const InCell &other : std::as_const(inCells))
                    if (other.grid == grid && other.edit.range.start + other.edit.range.length < cell.rawStart)
                        shift += int(other.edit.replacement.size()) - other.edit.range.length;
                return shift;
            }();
            c.naiveLength = int(payload.size());
            // Spaces before the text are no part of the cell, and a pipe
            // that would end it is written escaped.
            while (c.lead < payload.size() && (payload[c.lead] == u' ' || payload[c.lead] == u'\t')) ++c.lead;
            QString encoded = payload.mid(c.lead);
            QList<MdTable::Field> fields;
            QList<int> pipes;
            MdTable::splitRow(encoded, 0, int(encoded.size()), dialect.protectCode, fields, &pipes);
            for (auto pipe = pipes.crbegin(); pipe != pipes.crend(); ++pipe) encoded.insert(*pipe, u'\\');
            c.escapes = pipes;
            c.text = encoded;
            while (!c.text.isEmpty() && (c.text.back() == u' ' || c.text.back() == u'\t')) c.text.chop(1);
            model.rows[in.place.row][in.place.cell].text = c.text;
            cells.append(c);
        }
        const MdTable::Layout layout = MdTable::write(model);
        const int naiveTableStart = grid->start() + naiveShift(grid->start(), grid);
        // A position in a changed cell goes to the same place in its text,
        // which may run into the spaces after it, as a typed space does.
        const auto place = [&](int naive, bool *placed) {
            for (const Cell &c : std::as_const(cells)) {
                if (naive < c.naiveStart || naive > c.naiveStart + c.naiveLength) continue;
                int offset = qMax(0, naive - c.naiveStart - c.lead);
                for (int escape : c.escapes)
                    if (escape < offset) ++offset;
                const MdTable::Placed &at = layout.cells[c.place.row][c.place.cell];
                *placed = true;
                return naiveTableStart + at.start + qMin(offset, at.length + at.tail);
            }
            return naive;
        };
        int ownShift = 0;
        for (const InCell &in : std::as_const(inCells))
            if (in.grid == grid) ownShift += int(in.edit.replacement.size()) - in.edit.range.length;
        const int naiveTableEnd = naiveTableStart + (grid->end() - grid->start()) + ownShift;
        const int growth = int(layout.text.size()) - (naiveTableEnd - naiveTableStart);
        if (!anchorPlaced) {
            const int at = place(anchor, &anchorPlaced);
            if (anchorPlaced) placedAnchor = at;
            else if (anchor > naiveTableEnd) placedAnchor += growth;
        } else if (anchor > naiveTableEnd) {
            placedAnchor += growth;
        }
        if (!cursorPlaced) {
            const int at = place(cursor, &cursorPlaced);
            if (cursorPlaced) placedCursor = at;
            else if (cursor > naiveTableEnd) placedCursor += growth;
        } else if (cursor > naiveTableEnd) {
            placedCursor += growth;
        }
        const QStringView before = QStringView(source).mid(grid->start(), grid->end() - grid->start());
        if (before != layout.text) written.append(coveringEdit(grid->start(), before, layout.text));
    }
    std::sort(written.begin(), written.end(),
              [](const SourceEdit &a, const SourceEdit &b) { return a.range.start < b.range.start; });
    for (qsizetype i = 1; i < written.size(); ++i)
        if (written[i].range.start < written[i - 1].range.start + written[i - 1].range.length)
            return refuse(tr("Select the whole table, or stay inside one cell."));
    out.edits = written;
    out.anchor = placedAnchor;
    out.cursor = placedCursor;
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
    out.kind = Adapted::Changed;
    const MdTable::Layout layout = MdTable::write(model);
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
    QPointF delta = event->pixelDelta();
    if (delta.isNull()) delta = QPointF(event->angleDelta()) / 8.0 * 2;
    // Shift turns the wheel's turn into a sideways one, as everywhere.
    qreal across = delta.x();
    if (qFuzzyIsNull(across) && (event->modifiers() & Qt::ShiftModifier)) across = delta.y();
    if (qFuzzyIsNull(across)) {
        event->ignore();
        return;
    }
    m_grid->setScrollX(m_grid->scrollX() - across);
    event->accept();
}
