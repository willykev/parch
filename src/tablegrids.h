#pragma once

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

class EditorBinding;
class EditorDecorations;
class QKeyEvent;
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

    TableGrids *m_owner;
    MdTable::Table m_table;
    // Each cell's text and layout, the model's rows one after another.
    mutable std::vector<CellText> m_cells;
    QList<int> m_rowFirst; // the index in m_cells of each model row's first cell
    QList<qreal> m_columnLeft, m_columnWidth;
    QList<qreal> m_rowTop, m_rowHeight;
    qreal m_top = 0;
    qreal m_width = 0;
    qreal m_height = 0;
    qreal m_scrollX = 0;
    bool m_hasCaret = false;
    QRectF m_caretRect, m_blockRect;
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

public:
    explicit TableGrids(EditorBinding *binding);
    ~TableGrids() override;

    bool enabled() const { return m_enabled; }
    void setEnabled(bool on);
    QList<QObject *> gridObjects() const;
    const QList<TableGrid *> &grids() const { return m_grids; }
    bool caretInGrid() const { return m_caretGrid != nullptr; }
    QString problem() const { return m_problem; }

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
    // their selection goes, in the text after them.
    Adapted adapt(const QList<SourceEdit> &edits, int anchor, int cursor) const;
    // A change of a grid's table through its model, as the row and column
    // commands make one: the table written tidy, as one edit, and the
    // caret in the cell at a model row and column, its text selected or at
    // its start; nothing, refused, with the change's reason.
    Adapted change(const TableGrid *grid, const std::function<QString(MdTable::Model &)> &operation, int row,
                   int column, bool selectText = false) const;
    // A key that moves the caret in or into a grid; false for any other.
    bool key(QKeyEvent *event);
    // The caret or the selection moved: one in a table's syntax is moved
    // into its cells' text.
    void cursorMoved();

    // Where a position is in a grid's cell, to find it again once the table
    // is written anew, as another view's edit writes it; and where that is then.
    struct Spot {
        QPointer<TableGrid> grid;
        TableGrid::Place place;
        int offset = 0;
    };
    Spot spotOf(int position) const;
    std::optional<int> positionOf(const Spot &spot) const;

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
    bool enterFromProse(int position, bool down);

    EditorBinding *m_binding;
    QPointer<QObject> m_decorations;
    bool m_enabled = false;
    QList<TableGrid *> m_grids;
    TableGrid *m_caretGrid = nullptr;
    // Tables the parse found that hold the caret: shown as Markdown until
    // the caret leaves them, so that one being typed is not drawn under it.
    QList<std::pair<int, int>> m_waiting;
    QString m_problem;
    // Where the cursor was last, for the direction a move into syntax came from.
    int m_lastCursor = 0;
    bool m_settling = false;
    // The x a vertical move keeps, in the grid's coordinates.
    qreal m_goalX = -1;
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
