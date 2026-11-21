#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QStringView>
#include <optional>

// Markdown pipe tables as text. The reader splits rows exactly as the
// Markdown parser does (the parser calls splitRow and delimiterRow), so a
// table read here has the cells the preview shows. The writer puts a table
// back tidy: pipes on both sides, one space inside each, every column padded
// to its widest cell in display width, the delimiter row as wide as its
// column and marking its alignment, merged cells kept whole. Positions are
// UTF-16 offsets into the document's text.
namespace MdTable {

enum class Align { None, Left, Center, Right };

// What the dialect makes of a row's pipes.
struct Dialect {
    // A pipe inside a code span is part of the code (the Writer profile);
    // otherwise it splits the cell, as GFM has it.
    bool protectCode = true;
    // Two pipes side by side widen the cell before them over the next column
    // (the Writer profile with merged cells on).
    bool mergedCells = true;
    // A line starting `//` is a comment, which a table keeps between its rows.
    bool lineComments = true;
};

// The text between two pipes of a row, as written: [rawStart, rawEnd), and
// [start, end) without the spaces and tabs around it. Two pipes side by
// side make a field of no width.
struct Field {
    int rawStart = 0;
    int rawEnd = 0;
    int start = 0;
    int end = 0;
    bool zeroWidth = false;
};

// Splits the row in [from, to) at its unescaped pipes. A backslash before
// ASCII punctuation escapes it; with protectCode, a pipe inside a code span
// belongs to the code. A leading pipe opens the first field and a trailing
// one closes the last. `pipes`, when given, receives each splitting pipe.
bool splitRow(QStringView text, int from, int to, bool protectCode, QList<Field> &fields,
              QList<int> *pipes = nullptr);
// The alignments of a delimiter row in [from, to), or nothing when it is no
// delimiter row: each field dashes, with a colon at either end or both.
std::optional<QList<Align>> delimiterRow(QStringView text, int from, int to);

// The columns a text takes in a monospace font: two for a wide or fullwidth
// character or an emoji, none for a combining mark, by grapheme.
int displayWidth(QStringView text);

// A physical line of a table: [start, end) is its text, `content` where it
// starts after the quote markers and indentation it shares with the
// blocks around it, and `next` where the line after it starts.
struct Line {
    int start = 0;
    int content = 0;
    int end = 0;
    int next = 0;
};

// A cell of a row: its Markdown in [start, end), in the field [rawStart,
// rawEnd) between its pipes, and the columns it covers, from `column` for
// `span` columns. A cell the row leaves out has start == end == rawStart ==
// rawEnd at the end of the row, and is `missing`.
struct Cell {
    int column = 0;
    int span = 1;
    int start = 0;
    int end = 0;
    int rawStart = 0;
    int rawEnd = 0;
    bool missing = false;
};

struct Row {
    Line line;
    QList<Cell> cells; // in column order; empty for the delimiter row
};

// Why a table, though the parser reads it, is not one the grid can rewrite
// without losing something.
enum class Unfit {
    None,
    ExtraCells,   // a row has more cells than the header has columns
    LeadingMerge, // a row starts with a merge marker, with no cell to widen
    CommentLine,  // a // comment line sits between its rows
    TooLarge,     // more columns or cells than a table is laid out with
    MixedPrefixes, // its lines are indented, or quoted, differently
};

struct Table {
    // From the start of the header's line to the end of the last row's line,
    // before its line break.
    int start = 0;
    int end = 0;
    int columns = 0;
    QList<Align> aligns;
    // The header, the delimiter row, then the body rows.
    QList<Row> rows;
    Unfit unfit = Unfit::None;
    int unfitRow = -1;
};

// The limits a table is laid out within; past them it shows as written.
inline constexpr int MaxColumns = 128;
inline constexpr qsizetype MaxCells = 20000;

// The table whose header line starts the line holding `from`, with its rows
// down to the line holding `to`: the header, a delimiter row with as many
// fields as the header, and every line after them up to a blank line.
// Nothing when those first two lines are no table.
std::optional<Table> scan(QStringView text, int from, int to, const Dialect &dialect = {});
// The table holding a position, found from the text alone: the header just
// above the first delimiter row among the lines around it that have pipes,
// and the rows after it down to a blank line or a line without a pipe.
std::optional<Table> locate(QStringView text, int position, const Dialect &dialect = {});

// The index of the row holding a position, or -1.
int rowAt(const Table &table, int position);
// The index of the cell of row `row` holding a position: the cell whose
// field it is in, the first for the line's start and the last past its end.
int cellAt(const Table &table, int row, int position);

// ---- What a table says, apart from its spacing ------------------------------

// A cell's Markdown, without the spaces around it, and the columns it covers.
struct Item {
    QString text;
    int span = 1;
};

struct Model {
    QList<Align> aligns; // one for each column
    // The header first, then the body rows: no delimiter row.
    QList<QList<Item>> rows;
    // For each physical line, the delimiter row's second: its quote markers
    // and indentation, and the line break after it (none after the last).
    QStringList prefixes;
    QStringList breaks;
};

// The cells of a table as written, a row the header leaves short filled
// out with empty cells.
Model model(QStringView text, const Table &table);

// Where a written cell's Markdown starts, counted from the start of the
// table's text, and how many spaces follow it before its closing pipe.
struct Placed {
    int start = 0;
    int length = 0;
    int tail = 0;
};

// A table written tidy, and where each of its cells is: `cells[row][item]`
// for the model's rows (the header is row 0, the body from 1).
struct Layout {
    QString text;
    QList<QList<Placed>> cells;
    QList<int> lineStarts; // each physical line's start in the text
};

Layout write(const Model &model);
// Whether a row of a written table reads back as the model has it, the
// same cells over the same columns with the same text: a code span opened
// in one cell and closed in the next, for one, would join them.
// `columns` is the model's column count, worked out when it is not given.
bool readsBack(const Layout &layout, const Model &model, int row, const Dialect &dialect = {}, int columns = -1);
// Checks a staged row before laying out the whole table.
bool readsBack(const QList<Item> &items, int columns, const Dialect &dialect);

// ---- Changes to a table ------------------------------------------------------

// Each change below works on a model and leaves it a table the writer can
// write; or it changes nothing and says why it cannot be made. Rows are the
// model's (the header is row 0, the body from 1), columns count from 0.

// A cell's text as the table holds it: on one line, without the spaces
// around it, a pipe that would end the cell escaped.
QString encodeCell(const QString &text, const Dialect &dialect = {});
// The cell of a row that covers a column, and the first column it covers.
int itemAt(const QList<Item> &row, int column, int *first = nullptr);
int columnCount(const Model &model);

QString insertRows(Model &model, int at, int count, const QString &newline = QStringLiteral("\n"));
QString deleteRows(Model &model, int first, int count);
QString duplicateRows(Model &model, int first, int count, const QString &newline = QStringLiteral("\n"));
// Moves rows [first, first + count) to before row `to` of the table as it
// was (count + first at most: after the last row).
QString moveRows(Model &model, int first, int count, int to);
QString insertColumns(Model &model, int at, int count);
QString deleteColumns(Model &model, int first, int count);
QString moveColumns(Model &model, int first, int count, int to);
QString setAlignment(Model &model, int first, int count, Align align);
// Puts the body rows in the order of their text in a column: numbers as
// numbers and before text, text in the order of its letters, empty cells
// last; rows whose text is the same keep their order.
// A view can supply the body rows' visible keys from its parsed document.
QString sortRows(Model &model, int column, bool descending, const QStringList &keys = {});
// Joins the cells of a row from column `first` to `last` into one, their
// texts joined with a space; splits a joined cell into its columns again,
// its text in the first.
QString mergeCells(Model &model, int row, int first, int last, const Dialect &dialect = {});
QString splitCell(Model &model, int row, int column);
// Empties the cells over rows [firstRow, lastRow] and columns [firstColumn, lastColumn].
QString clearCells(Model &model, int firstRow, int lastRow, int firstColumn, int lastColumn);
QString fillCells(Model &model, int firstRow, int lastRow, int firstColumn, int lastColumn, const QString &markdown);
// Writes values into the cells from a row and column on, adding rows and
// columns where they reach past the table. With markdown, the values are
// already encoded as cell source.
QString pasteCells(Model &model, int row, int column, const QList<QStringList> &values, const Dialect &dialect = {},
                   const QString &newline = QStringLiteral("\n"), bool markdown = false);
// The text of a key for sorting: a cell's Markdown without its markers.
QString plainText(const QString &markdown);
// A new table of empty cells: `rows` counts the header.
Model emptyTable(int columns, int rows, const QString &prefix = {}, const QString &newline = QStringLiteral("\n"));

// The physical line a model row is written on: the header on line 0, the
// body from line 2.
inline int lineOf(int modelRow) { return modelRow == 0 ? 0 : modelRow + 1; }
// The model row a table row is, or -1 for the delimiter.
inline int modelRowOf(int tableRow) { return tableRow == 0 ? 0 : tableRow == 1 ? -1 : tableRow - 1; }

} // namespace MdTable
