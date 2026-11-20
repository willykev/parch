#include "tablegrids.h"

#include "document.h"
#include "documentparse.h"
#include "editorbinding.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QKeyEvent>
#include <QQuickWindow>
#include <QSGRectangleNode>
#include <QSGTextNode>
#include <QTextBlock>
#include <QTextCursor>
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
    const bool whole = from < m_table.start && to > m_table.end;
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
            if (whole || (from <= cell.start && to >= cell.end && from < to && (from < cell.rawStart || to > cell.rawEnd)))
                d.selected = true;
            else if (from < to && from >= cell.rawStart && to <= cell.rawEnd) {
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
    if (has == m_hasCaret && caret == m_caretRect && block == m_blockRect) return;
    m_hasCaret = has;
    m_caretRect = caret;
    m_blockRect = block;
    emit caretChanged();
}

// ---- TableGrids -----------------------------------------------------------------

TableGrids::TableGrids(EditorBinding *binding) : QObject(binding), m_binding(binding) {}

TableGrids::~TableGrids() = default;

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
    return caret.translated(grid->x(), grid->y());
}

int TableGrids::sourceAt(QPointF point) const
{
    for (TableGrid *grid : m_grids)
        if (point.y() >= grid->y() && point.y() < grid->y() + grid->height())
            return grid->sourceAt(point.x() - grid->x(), point.y() - grid->y());
    return -1;
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
        if (oldEnd < grid->start()) {
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
    for (const MdTable::Cell &cell : cells) {
        if (position > cell.rawEnd) continue;
        if (position < cell.start) return cell.start;
        return qMin(position, cell.end + tailOf(source, cell));
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

bool TableGrids::key(QKeyEvent *event)
{
    if (m_grids.isEmpty()) return false;
    const int cursor = this->cursor();
    TableGrid *grid = gridAt(cursor);
    const Qt::KeyboardModifiers mods = event->modifiers() & ~Qt::KeypadModifier;
    const bool shift = mods & Qt::ShiftModifier;
    const bool plain = (mods & ~Qt::ShiftModifier) == Qt::NoModifier;
    const int key = event->key();
    if (!grid) {
        if (plain && !shift && (key == Qt::Key_Down || key == Qt::Key_Up)) return enterFromProse(cursor, key == Qt::Key_Down);
        return false;
    }
    if (key != Qt::Key_Up && key != Qt::Key_Down) m_goalX = -1;
    const QString source = text();
    const TableGrid::Place place = grid->placeOf(cursor);
    if (!place.valid()) return false;
    const MdTable::Cell &cell = grid->cellAt(place);
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
        } else if (!shift) {
            const TableGrid::Place to = left ? previousCell(place) : nextCell(place);
            if (to.valid()) {
                const MdTable::Cell &target = grid->cellAt(to);
                const int at = left ? target.end : target.start;
                select(at, at);
            } else {
                const int at = left ? grid->start() - breakBefore(source, grid->start())
                                    : grid->end() + breakAfter(source, grid->end());
                select(qMax(0, at), qMax(0, at));
            }
        }
        return true;
    }
    if (plain && (key == Qt::Key_Up || key == Qt::Key_Down)) return moveVertically(grid, cursor, key == Qt::Key_Down ? 1 : -1, shift);
    if (plain && key == Qt::Key_Home) {
        moveTo(cell.start);
        return true;
    }
    if (plain && key == Qt::Key_End) {
        moveTo(cell.end);
        return true;
    }
    if ((key == Qt::Key_Tab && mods == Qt::NoModifier) || key == Qt::Key_Backtab
        || (key == Qt::Key_Tab && mods == Qt::ShiftModifier)) {
        const bool back = key == Qt::Key_Backtab || shift;
        const TableGrid::Place to = back ? previousCell(place) : nextCell(place);
        if (!to.valid()) return back;
        const MdTable::Cell &target = grid->cellAt(to);
        select(target.start, target.end);
        return true;
    }
    if (mods == Qt::NoModifier && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
        if (place.row + 1 < grid->rowCount()) {
            TableGrid::Place to{place.row + 1, 0};
            for (int i = 0; i < grid->cellCount(to.row); ++i)
                if (grid->cellAt({to.row, i}).column <= cell.column) to.cell = i;
            const MdTable::Cell &target = grid->cellAt(to);
            select(target.start, target.end);
        }
        return true;
    }
    return false;
}

TableGrids::Adapted TableGrids::adapt(SourceRange range, const QString &inserted) const
{
    Adapted out;
    if (m_grids.isEmpty()) return out;
    const int from = range.start, to = range.start + range.length;
    TableGrid *grid = nullptr;
    for (TableGrid *g : m_grids)
        if (to >= g->start() && from <= g->end()) grid = g;
    if (!grid) return out;
    // A table replaced or removed whole is the writer's to do.
    if (from <= grid->start() && to >= grid->end()) return out;
    const QString source = text();
    // An edit right after the table's last character, on its line, is the
    // last cell's; so is one just before its first.
    const TableGrid::Place place = grid->placeOf(from);
    const TableGrid::Place last = grid->placeOf(to);
    if (!place.valid() || !(place == last)) {
        out.kind = Adapted::Refused;
        out.reason = tr("Select the whole table, or stay inside one cell.");
        return out;
    }
    const MdTable::Cell &cell = grid->cellAt(place);
    const int tail = tailOf(source, cell);
    const QString payload = source.mid(cell.start, cell.end - cell.start);
    const int a = qBound(cell.start, from, cell.end + tail) - cell.start;
    const int b = qBound(cell.start, to, cell.end + tail) - cell.start;
    QString base = payload;
    if (qMax(a, b) > base.size()) base += QString(qMax(a, b) - base.size(), u' ');
    // A cell is one line: line breaks in what is put in become spaces.
    QString text = inserted;
    for (QChar &c : text)
        if (c == u'\n' || c == u'\r' || c == QChar::ParagraphSeparator || c == QChar::LineSeparator) c = u' ';
    QString written = base.left(a) + text + base.mid(b);
    int caret = a + int(text.size());
    // Spaces before the text are no part of the cell.
    int lead = 0;
    while (lead < written.size() && (written[lead] == u' ' || written[lead] == u'\t')) ++lead;
    written.remove(0, lead);
    caret = qMax(0, caret - lead);
    // A pipe that would end the cell is written escaped.
    QList<MdTable::Field> fields;
    QList<int> pipes;
    MdTable::splitRow(written, 0, int(written.size()), dialect().protectCode, fields, &pipes);
    for (auto pipe = pipes.crbegin(); pipe != pipes.crend(); ++pipe) {
        written.insert(*pipe, u'\\');
        if (*pipe < caret) ++caret;
    }
    QString trimmed = written;
    while (!trimmed.isEmpty() && (trimmed.back() == u' ' || trimmed.back() == u'\t')) trimmed.chop(1);
    out.kind = Adapted::Changed;
    MdTable::Model model = MdTable::model(source, grid->table());
    model.rows[place.row][place.cell].text = trimmed;
    const MdTable::Layout layout = MdTable::write(model);
    const MdTable::Placed &placed = layout.cells[place.row][place.cell];
    const int caretAt = grid->start() + placed.start + qMin(caret, placed.length + placed.tail);
    out.anchor = out.cursor = caretAt;
    const QStringView before = QStringView(source).mid(grid->start(), grid->end() - grid->start());
    if (before == layout.text) return out;
    int same = 0;
    const int shortest = int(qMin(before.size(), layout.text.size()));
    while (same < shortest && before[same] == layout.text[same]) ++same;
    int tailSame = 0;
    while (tailSame < shortest - same
           && before[before.size() - 1 - tailSame] == layout.text[layout.text.size() - 1 - tailSame])
        ++tailSame;
    out.edits.append(SourceEdit{{grid->start() + same, int(before.size()) - same - tailSame},
                                layout.text.mid(same, layout.text.size() - same - tailSame)});
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
    auto *root = new QSGNode;
    const auto rect = [&](const QRectF &r, const QColor &color) {
        QSGRectangleNode *node = window()->createRectangleNode();
        node->setRect(r);
        node->setColor(color);
        root->appendChildNode(node);
    };
    const qreal top = qMax<qreal>(0, m_viewTop);
    const qreal bottom = qMin(m_grid->height(), m_viewTop + m_viewHeight);
    if (bottom <= top) return root;
    // The band hides the table's Markdown beneath, the header line's text too.
    rect(QRectF(0, top, qMax(m_bandWidth, m_grid->width()), bottom - top), m_backgroundColor);
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
    return root;
}
