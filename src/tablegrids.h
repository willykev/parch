#pragma once

#include "markdownediting.h"
#include "mdtable.h"
#include "sourcetypes.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QRectF>
#include <QString>
#include <QTextLayout>
#include <functional>
#include <memory>
#include <vector>
#include <optional>
#include <tuple>

class Document;
class EditorBinding;
class EditorDecorations;
namespace md {
struct Options;
}
class QKeyEvent;
class QMimeData;
class QTextDocument;
class TableGrids;

// One Markdown table of one view, drawn as a grid in place of its lines.
// Its Markdown stays in the view's text, every character of it: the header
// line is made as tall as the grid and the lines under it are hidden, and
// the grid is drawn over that place. The view's caret and selection stay
// source positions, inside the table's Markdown; the grid draws them in
// its cells. Cells are laid out with the editor's font and line height,
// each wrapped in its column.
//
// Coordinates are the editor's (the TextEdit's), or the grid's own from its
// top left corner.
class TableGrid : public QObject {
    Q_OBJECT
    Q_PROPERTY(qreal x READ x NOTIFY geometryChanged)
    Q_PROPERTY(qreal y READ y NOTIFY geometryChanged)
    Q_PROPERTY(qreal width READ width NOTIFY geometryChanged)
    Q_PROPERTY(qreal height READ height NOTIFY geometryChanged)
    // How wide the grid shows: all of it, or as much as the room allows,
    // the rest scrolled to sideways, `scrollX` across.
    Q_PROPERTY(qreal viewWidth READ viewWidth NOTIFY geometryChanged)
    Q_PROPERTY(qreal scrollX READ scrollX WRITE setScrollX NOTIFY scrollChanged)
    Q_PROPERTY(int rows READ rowCount NOTIFY geometryChanged)
    Q_PROPERTY(int columns READ columnCount NOTIFY geometryChanged)
    // The caret, while the view's cursor is in this table: its rectangle
    // in the grid's coordinates.
    Q_PROPERTY(bool hasCaret READ hasCaret NOTIFY caretChanged)
    Q_PROPERTY(QRectF caretRect READ caretRect NOTIFY caretChanged)
    // Vim's block caret on the character at the cursor, in the grid's coordinates.
    Q_PROPERTY(QRectF blockRect READ blockRect NOTIFY caretChanged)

public:
    explicit TableGrid(TableGrids *owner);
    ~TableGrid() override;

    qreal x() const { return 0; }
    qreal y() const { return m_top; }
    qreal width() const { return m_width; }
    qreal height() const { return m_height; }
    qreal viewWidth() const;
    qreal scrollX() const { return m_scrollX; }
    void setScrollX(qreal x);
    // Scrolls sideways as little as shows a rectangle of the grid.
    void reveal(const QRectF &rect);
    int rowCount() const { return int(m_rowTop.size()); }
    int columnCount() const { return int(m_columnLeft.size()); }
    bool hasCaret() const { return m_hasCaret; }
    QRectF caretRect() const { return m_caretRect; }
    QRectF blockRect() const { return m_blockRect; }

    const MdTable::Table &table() const { return m_table; }
    // The table's text in the source as it was last read.
    int start() const { return m_table.start; }
    int end() const { return m_table.end; }

    // A cell of the grid: a model row (the header is 0, the body from 1)
    // and its place in that row's cells.
    struct Place {
        int row = -1;
        int cell = -1;
        bool valid() const { return row >= 0; }
        bool operator==(const Place &) const = default;
    };
    // The cell a row's cell is, from the table's rows (with its delimiter row).
    const MdTable::Cell &cellAt(Place place) const;
    int cellCount(int row) const;
    // The cell holding a source position, or none outside the table's cells.
    Place placeOf(int position) const;
    // Where a cell is drawn, in the grid's coordinates, borders excluded.
    QRectF cellRect(Place place) const;
    // The cell under a point of the grid, or the nearest.
    Place placeAt(QPointF point) const;
    // The source position at a point of the grid, in its cell.
    Q_INVOKABLE int sourceAt(qreal x, qreal y) const;
    // For the handles: the row and the column under a point of the grid, the
    // nearest outside it, and where each is, rules excluded.
    Q_INVOKABLE int rowAt(qreal y) const;
    Q_INVOKABLE int columnAt(qreal x) const;
    Q_INVOKABLE QRectF rowRect(int row) const;
    Q_INVOKABLE QRectF columnRect(int column) const;
    // The row and the column of the cell the view's caret is in, or -1.
    Q_INVOKABLE int caretRow() const;
    Q_INVOKABLE int caretColumn() const;
    // Where a caret at a source position in a cell is drawn, in the grid's coordinates.
    QRectF caretAt(int position) const;
    // The text lines of a cell: how many, and the source position at x on
    // one of them.
    int lineCount(Place place) const;
    int lineOf(Place place, int position) const;
    int positionAtLine(Place place, int line, qreal x) const;
    qreal xOf(Place place, int position) const;

    // What the view draws: the visible rows from `top` to `bottom` in the
    // grid's coordinates, and each cell's text laid out.
    struct Drawn {
        QRectF rect;
        QTextLayout *layout = nullptr;
        bool header = false;
        int selectionStart = -1;
        int selectionLength = 0;
        bool selected = false; // the whole cell, as part of a block or table selection
        // Wavy underlines, as offsets into the cell's text, and their colours.
        QList<std::tuple<int, int, QColor>> waves;
    };
    QList<Drawn> drawn(qreal top, qreal bottom);
    QList<qreal> columnLefts() const { return m_columnLeft; }
    QList<qreal> rowTops() const { return m_rowTop; }
    qreal padding() const;
    // The height of a line of the text it stands in.
    qreal lineHeight() const;
    // The text of each cell of a model row.
    QStringList rowTexts(int row) const;
    // Whether a model row's cells hold these texts.
    bool rowIs(int row, const QStringList &texts) const;
    // Where a visual line of a cell's text starts and ends, as source positions.
    std::pair<int, int> lineRange(Place place, int line) const;

signals:
    void geometryChanged();
    void caretChanged();
    void contentChanged();
    void scrollChanged();

private:
    friend class TableGrids;
    struct CellText {
        QString text;
        std::unique_ptr<QTextLayout> layout;
        qreal width = -1;
        int lines = 1;
        // Its text's width on one line, and its longest word's, in the font
        // it is drawn in; -1 until measured.
        qreal natural = -1;
        qreal word = -1;
    };
    // The table changed or the font or width did: reads its cells and lays them out again.
    void read(const MdTable::Table &table, QStringView text);
    void measure();
    QTextLayout *layoutOf(Place place) const;
    // The formatting changed between source positions: those cells are laid out again.
    void restyle(int start, int end);
    // Where a cell's columns start and end, across.
    std::pair<qreal, qreal> extent(const MdTable::Cell &cell) const;
    int indexOf(Place place) const;
    void updateCaret();
    // Of the cells a short row leaves out, which all stand at its end, the
    // one the caret was taken to; any other place for none.
    void preferMissing(Place place, int position) const;

    TableGrids *m_owner;
    MdTable::Table m_table;
    // Each cell's text and layout, the model's rows one after another.
    mutable std::vector<CellText> m_cells;
    // The font the cells' widths were measured in: another measures them again.
    QString m_measuredFont;
    QList<int> m_rowFirst; // the index in m_cells of each model row's first cell
    QList<qreal> m_columnLeft, m_columnWidth;
    QList<qreal> m_rowTop, m_rowHeight;
    qreal m_top = 0;
    qreal m_width = 0;
    qreal m_height = 0;
    qreal m_scrollX = 0;
    bool m_hasCaret = false;
    QRectF m_caretRect, m_blockRect;
    mutable Place m_missing;
    mutable int m_missingAt = -1;
};

// The tables of one view drawn as grids, and what that changes for its
// editing: each edit of the view that reaches into a table is made an edit
// of a cell, written with the whole table tidy; keys move from cell to cell;
// and the caret and selection are kept in the cells' text. The binding owns
// it and calls it at each point where its projection, caret or edits change.
class TableGrids : public QObject {
    Q_OBJECT
    // Tables are shown as grids; off, as their Markdown.
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QList<QObject *> grids READ gridObjects NOTIFY gridsChanged)
    // The view's cursor is in a grid, whose caret the grid draws.
    Q_PROPERTY(bool caretInGrid READ caretInGrid NOTIFY caretChanged)
    Q_PROPERTY(QString problem READ problem NOTIFY problemChanged)
    // The most cells, the header's counted, and columns a table shown as a grid holds.
    Q_PROPERTY(int maxCells READ maxCells CONSTANT)
    Q_PROPERTY(int maxColumns READ maxColumns CONSTANT)

public:
    explicit TableGrids(EditorBinding *binding);
    ~TableGrids() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    QList<QObject *> gridObjects() const;
    const QList<TableGrid *> &grids() const { return m_grids; }
    bool caretInGrid() const { return m_caretGrid != nullptr; }
    // The tables of the text as it is now have been looked for: those that
    // are to be grids are.
    bool settled() const;
    QString problem() const { return m_problem; }
    int maxCells() const { return int(MdTable::MaxCells); }
    int maxColumns() const { return MdTable::MaxColumns; }

    EditorBinding *binding() const { return m_binding; }
    // The view's formatting, which the grids draw their cells' text with.
    void setDecorations(EditorDecorations *decorations);
    EditorDecorations *decorations() const;
    QFont font() const;
    QFont headerFont() const;
    qreal lineHeight() const;
    // The room a grid may take: the column and the margin to its right.
    qreal availableWidth() const;
    MdTable::Dialect dialect() const;
    // The table dialect a document's Markdown is read in.
    static MdTable::Dialect dialectOf(const md::Options &options);
    // Edits of a text, those inside the cells of its tables made with each
    // such table written tidy around them; the others as they are. For a
    // writer with no view, as a link rewritten when its file moves.
    static QList<SourceEdit> tidyCellEdits(QStringView text, const QList<SourceEdit> &edits,
                                           const MdTable::Dialect &dialect);

    // The grid holding a source position, from its header line's start to
    // its last line's end; null for none.
    TableGrid *gridAt(int position) const;
    // Where a caret at a source position in a grid is drawn, in the
    // editor's coordinates; nothing outside the grids.
    std::optional<QRectF> rectangleFor(int position) const;
    // The source position under a point of the editor in a grid; -1 outside.
    int sourceAt(QPointF point) const;
    // Whether a range is text a grid shows: in no grid, or inside one cell's text.
    bool inCellText(int start, int end) const;
    // The block of cells the view's selection makes in a grid: from one
    // cell to another, as rows and columns of the model; false for none.
    bool block(const TableGrid *grid, int *firstRow, int *lastRow, int *firstColumn, int *lastColumn) const;

    // What an edit of the view becomes: the same, when it touches no grid;
    // an edit of a cell, the table written tidy around it, with where the
    // caret goes; or nothing, refused, with the reason the view shows.
    struct Adapted {
        enum Kind { Pass, Changed, Refused } kind = Pass;
        QList<SourceEdit> edits;
        int anchor = 0;
        int cursor = 0;
        QString reason;
    };
    Adapted adapt(SourceRange range, const QString &text) const;
    // The same for several edits made at once, as a command or Replace All
    // makes them, against the text as it is, sorted and apart; and where
    // their selection goes, in the text after them. With `left`, those that
    // cannot be made are left out, as Replace All leaves them, and counted
    // there, the reason being the first one's; Refused only when none can be.
    Adapted adapt(const QList<SourceEdit> &edits, int anchor, int cursor, int *left = nullptr) const;
    // A change of a grid's table through its model, as the row and column
    // commands make one: the table written tidy, as one edit, and the
    // caret in the cell at a model row and column, its text selected or at
    // its start; nothing, refused, with the change's reason.
    Adapted change(const TableGrid *grid, const std::function<QString(MdTable::Model &)> &operation, int row,
                   int column, bool selectText = false) const;
    // A key that moves the caret in or into a grid; false for any other.
    bool key(QKeyEvent *event);
    // A command over a block of cells: an inline one, `inCells`, made on
    // each cell's text, the table written once, one step to undo; any
    // other refused. Nothing when the selection is no block, for the view
    // to make it as anywhere.
    std::optional<bool> formatCells(const std::function<std::optional<TextChange>(QStringView, int, int)> &operation,
                                    bool inCells);
    // The caret or the selection moved: one in a table's syntax is moved
    // into its cells' text.
    void cursorMoved();
    // Vim's motion keys are moving the caret a step at a time: a step out of
    // a cell's text goes on into the next cell.
    void setStepping(bool stepping) { m_stepping = stepping; }
    bool stepping() const { return m_stepping; }

    // Where a position is in a grid's cell, to find it again once the table
    // is written anew, as another view's edit writes it; and where that is then.
    struct Spot {
        QPointer<TableGrid> grid;
        TableGrid::Place place;
        int offset = 0;
        // The row's line and cells as they were, and how many rows the
        // table had: rows coming or going before it move it, and where an
        // edit wrote rows around it anew it is found again by its cells.
        int lineStart = 0;
        int lineEnd = 0;
        int rows = 0;
        QStringList row;
        // Which of the rows with these cells it is, counted from the top;
        // counted only where the edit reaches its row.
        int likeBefore = 0;
    };
    // Where a position is before an edit of `range`.
    Spot spotOf(int position, SourceRange range = {-1, 0}) const;
    // Where a spot is after an edit of the source, of `range` by `inserted` units.
    std::optional<int> positionOf(const Spot &spot, SourceRange range, int inserted) const;
    // Whether a range of a text lies in the text of one cell of a table.
    static bool inTableCell(QStringView text, SourceRange range, const MdTable::Dialect &dialect);
    // Whether the parse of a document's text as it is now reads a table at a
    // position: false in a code block, and while no parse of it is at hand.
    static bool parsedTableAt(const Document *document, int position);
    // The texts of the cells of the grid that a range lies in, row by row,
    // before an edit of it; and once it is made, where it ends in the first
    // cell whose text it changed, if it changed one.
    struct CellTexts {
        QPointer<TableGrid> grid;
        QList<QStringList> rows;
    };
    CellTexts cellTexts(SourceRange range) const;
    std::optional<int> changeEnd(const CellTexts &before) const;

    // The commands for a grid's rows, columns, cells and the table as a
    // whole, by id, in the order a menu lists them.
    static QStringList commands();
    static bool isCommand(const QString &id);
    // Whether a command acts at the caret now, and runs it there, on the
    // rows or columns the selection covers.
    Q_INVOKABLE bool can(const QString &command) const;
    Q_INVOKABLE bool runCommand(const QString &command);
    // From a row's or a column's handle: selects it whole, or the rows or
    // columns from the selection's to it.
    Q_INVOKABLE void selectRow(TableGrid *grid, int row, bool extend);
    Q_INVOKABLE void selectColumn(TableGrid *grid, int column, bool extend);
    // Moves the selected rows, or the caret's, to before row `to` of the
    // table as it is; columns to before column `to`.
    Q_INVOKABLE bool moveRowsTo(TableGrid *grid, int to);
    Q_INVOKABLE bool moveColumnsTo(TableGrid *grid, int to);
    // Adds a row after the last, or a column after the last, from the
    // grid's plus buttons.
    Q_INVOKABLE bool appendRow(TableGrid *grid);
    Q_INVOKABLE bool appendColumn(TableGrid *grid);
    // A table the binding has just put in, shown as a grid at once.
    void adoptAt(int position);
    // The clipboard and a block of cells: copied as cells, tab-separated
    // text and an HTML table; cut, then emptied. False when the selection is
    // no block, for the view's own copy and cut.
    bool copyBlock();
    bool cutBlock();
    // Cells from a spreadsheet pasted at the caret's cell, or over a block
    // of them: they fill it and grow the table where they reach past it. A
    // single value fills every cell of a block, and over a block any text
    // with tabs is cells. NotCells for anything else, which the view pastes
    // as text; Refused when they cannot go there.
    enum class Paste { NotCells, Pasted, Refused };
    Paste pasteCells(const QMimeData *mime);
    // A double click selects the word at a position in a cell, a triple
    // click the cell's text.
    Q_INVOKABLE void selectWordAt(int position);
    Q_INVOKABLE void selectCellAt(int position);
    // What an input method shows changed: the cell it is in is laid out
    // again with it.
    void compositionChanged();
    // The cells a clipboard holds, row by row: from an HTML table, or the
    // text with tabs between them that a spreadsheet gives beside one; with
    // `separated`, from any text with tabs; none for anything else.
    static std::optional<QList<QStringList>> cellsOf(const QMimeData *mime, bool separated = false);

    // The binding's projection: shown anew, a change followed, laid out anew.
    void loaded();
    void sourceChanged(SourceRange range, int inserted);
    void relayout();
    // The binding lets go of its document and editor.
    void clear();

public slots:
    // Lays the grids out anew once the editor has taken a new font.
    void relayoutLater();

signals:
    void enabledChanged();
    void gridsChanged();
    void caretChanged();
    void problemChanged();
    // An edit was refused, and why, for the view to say.
    void refused(const QString &reason);

private:
    friend class TableGrid;
    void parsed();
    // Grids for these tables, read from the text; others are shown as Markdown again.
    void adopt(const QList<MdTable::Table> &tables);
    void project(TableGrid *grid);
    void unproject(int start, int end);
    void placeGrids();
    void setProblem(const QString &problem);
    QTextDocument *document() const;
    QString text() const;
    // Selects in the view, as the binding does for keys.
    void select(int anchor, int cursor);
    int cursor() const;
    int anchor() const;
    // A position to rest at in a grid: in a cell's text, nearest `position`
    // in the direction of travel.
    int settle(const TableGrid *grid, int position, int direction) const;
    bool moveVertically(TableGrid *grid, int position, int lines, bool extend);
    // An edit the rows of a grid, the line after one, or a block of its
    // cells make something other than a cell's edit; nothing for any other.
    std::optional<Adapted> adaptOne(const SourceEdit &edit) const;
    // Where the line after a grid starts, and the line after that, when the
    // first is blank; -1 where there is none.
    int lineAfter(const TableGrid *grid) const;
    // Lines put among a grid's rows as rows of its model, merged cells kept;
    // none when a line is no table row, and with `strict`, when one lacks a
    // pipe at either end. A row wider than the table says so in `refusal`.
    std::optional<QList<QList<MdTable::Item>>> rowsToPut(const TableGrid *grid, const QString &lines, bool strict,
                                                         QString *refusal) const;

    // Runs a change on the grid as the view's own edit, one undo step, and
    // places the selection it says; false when it is refused.
    bool run(const Adapted &change);
    // The rows and columns the selection covers in a grid: a block, or the caret's cell.
    struct Span {
        int firstRow = 0, lastRow = 0, firstColumn = 0, lastColumn = 0;
    };
    std::optional<Span> span(const TableGrid *grid) const;
    // A change that leaves a block selected, from one cell to another.
    Adapted changeBlock(const TableGrid *grid, const std::function<QString(MdTable::Model &)> &operation,
                        const Span &selected) const;
    // The table, shown as Markdown in this view, that holds a position.
    std::optional<MdTable::Table> markdownTableAt(int position) const;
    bool enterFromProse(int position, bool down);

    EditorBinding *m_binding;
    QPointer<QObject> m_decorations;
    bool m_enabled = false;
    QList<TableGrid *> m_grids;
    TableGrid *m_caretGrid = nullptr;
    // Tables the parse found that hold the caret: shown as Markdown until
    // the caret leaves them, so that one being typed is not drawn under it.
    QList<std::pair<int, int>> m_waiting;
    // Tables this view shows as Markdown, by where they start.
    QList<int> m_markdown;
    QString m_problem;
    // Where the cursor was last, for the direction a move into syntax came from.
    int m_lastCursor = 0;
    bool m_settling = false;
    bool m_stepping = false;
    // The x a vertical move keeps, in the grid's coordinates, and where
    // that move left the caret: a caret moved from there drops it.
    qreal m_goalX = -1;
    int m_goalAt = -1;
    // The columns a column's handle selected, while the selection it made
    // stays: the commands act on them, though the block shown takes in the
    // merged cells that reach into them.
    struct Picked {
        QPointer<TableGrid> grid;
        int anchor = -1, cursor = -1, firstColumn = 0, lastColumn = 0;
    };
    Picked m_picked;
    // The selection Ctrl+A made last in a grid, and how far it went: a
    // cell's text, then every cell; the third goes to the whole text.
    struct SelectAll {
        int anchor = -1, cursor = -1, level = 0;
    };
    SelectAll m_selectAll;
    // The cell an input method's text was last shown in.
    QPointer<TableGrid> m_preeditGrid;
    TableGrid::Place m_preeditPlace;
    QMetaObject::Connection m_parseConnection;
};

// Draws one grid: its rules, its cells' text and the selection.
class TableGridView : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(TableGrid *grid READ grid WRITE setGrid NOTIFY gridChanged)
    // The part of the grid in view, in its coordinates.
    Q_PROPERTY(qreal viewTop READ viewTop WRITE setViewTop NOTIFY viewChanged)
    Q_PROPERTY(qreal viewHeight READ viewHeight WRITE setViewHeight NOTIFY viewChanged)
    Q_PROPERTY(QColor textColor MEMBER m_textColor NOTIFY lookChanged)
    Q_PROPERTY(QColor ruleColor MEMBER m_ruleColor NOTIFY lookChanged)
    Q_PROPERTY(QColor headerColor MEMBER m_headerColor NOTIFY lookChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_backgroundColor NOTIFY lookChanged)
    Q_PROPERTY(QColor selectionColor MEMBER m_selectionColor NOTIFY lookChanged)
    Q_PROPERTY(QColor selectedTextColor MEMBER m_selectedTextColor NOTIFY lookChanged)
    Q_PROPERTY(bool nativeRendering MEMBER m_nativeRendering NOTIFY lookChanged)

public:
    explicit TableGridView(QQuickItem *parent = nullptr);

    TableGrid *grid() const { return m_grid; }
    void setGrid(TableGrid *grid);
    qreal viewTop() const { return m_viewTop; }
    void setViewTop(qreal top);
    qreal viewHeight() const { return m_viewHeight; }
    void setViewHeight(qreal height);

signals:
    void gridChanged();
    void viewChanged();
    void lookChanged();

protected:
    QSGNode *updatePaintNode(QSGNode *old, UpdatePaintNodeData *) override;
    // A sideways scroll over a grid wider than its room scrolls it; any
    // other goes on to the text.
    void wheelEvent(QWheelEvent *event) override;

private:
    QPointer<TableGrid> m_grid;
    qreal m_viewTop = 0;
    qreal m_viewHeight = 0;
    QColor m_textColor, m_ruleColor, m_headerColor, m_backgroundColor, m_selectionColor, m_selectedTextColor;
    bool m_nativeRendering = true;
};
