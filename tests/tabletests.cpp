#include "tabletests.h"

#include "markdownediting.h"
#include "mdtable.h"
#include "mdtree.h"

#include <QtTest>

using namespace Qt::StringLiterals;

namespace {

MdTable::Dialect dialectFor(const md::Options &options)
{
    MdTable::Dialect dialect;
    dialect.protectCode = options.profile == md::Profile::Writer;
    dialect.mergedCells = dialect.protectCode && options.mergedCells;
    dialect.lineComments = options.lineComments;
    return dialect;
}

void collectTables(const md::Node &node, QList<const md::Node *> &tables)
{
    if (node.type == md::NodeType::Table) tables.append(&node);
    for (const md::Node &child : node.children) collectTables(child, tables);
}

// What the parser reads in a text's tables: each row's cells as text,
// first column and span, and each column's alignment.
struct ParsedCell {
    QString text;
    int column = 0;
    int span = 1;
    md::Align align = md::Align::None;
    bool operator==(const ParsedCell &) const = default;
};
using ParsedTable = QList<QList<ParsedCell>>;

QList<ParsedTable> parsedTables(const QString &text, const md::Options &options)
{
    const md::Document document = md::parse(text, options);
    QList<const md::Node *> nodes;
    collectTables(document.root, nodes);
    QList<ParsedTable> tables;
    for (const md::Node *table : nodes) {
        ParsedTable rows;
        for (const md::Node &row : table->children) {
            if (row.type != md::NodeType::TableRow) continue;
            QList<ParsedCell> cells;
            for (const md::Node &cell : row.children)
                cells.append({text.mid(cell.range.start, cell.range.length()), cell.column, cell.span, cell.align});
            rows.append(cells);
        }
        tables.append(rows);
    }
    return tables;
}

QString describe(const ParsedTable &table)
{
    QStringList rows;
    for (const QList<ParsedCell> &row : table) {
        QStringList cells;
        for (const ParsedCell &cell : row)
            cells.append(QStringLiteral("%1@%2x%3/%4").arg(cell.text).arg(cell.column).arg(cell.span).arg(int(cell.align)));
        rows.append(cells.join(u' '));
    }
    return rows.join(QStringLiteral(" ; "));
}

// The text with every table the parser finds written tidy.
QString tidied(const QString &text, const md::Options &options)
{
    const md::Document document = md::parse(text, options);
    QList<const md::Node *> nodes;
    collectTables(document.root, nodes);
    QString out = text;
    for (auto node = nodes.rbegin(); node != nodes.rend(); ++node) {
        const auto table = MdTable::scan(text, (*node)->range.start, (*node)->range.end, dialectFor(options));
        if (!table) continue;
        out.replace(table->start, table->end - table->start, MdTable::write(MdTable::model(text, *table)).text);
    }
    return out;
}

// A text with the table that holds its first pipe written tidy.
QString formatted(const QString &text, const MdTable::Dialect &dialect = {})
{
    const auto table = MdTable::locate(text, int(text.indexOf(u'|')), dialect);
    if (!table) return QStringLiteral("(no table)");
    QString out = text;
    out.replace(table->start, table->end - table->start, MdTable::write(MdTable::model(text, *table)).text);
    return out;
}

const QStringList &fixtures()
{
    static const QStringList texts{
        QStringLiteral("| a | b |\n|---|:-:|\n| c | d |"),
        QStringLiteral("| A | B | C |\n|---|---|---|\n| Writing || Reading |\n| x | y | z |"),
        QStringLiteral("| `a|b` | c |\n|---|---|\n| x | `y|z` |"),
        QStringLiteral("| a | b | c |\n|---|---|---|\n| `x|y` | z |"),
        QStringLiteral("a | b\n--|--\nc | d"),
        QStringLiteral("| a | b |\n|---|---|\n| x |\n| 1 | 2 | 3 |"),
        QStringLiteral("| a \\| b | c |\n|---|---|\n| x | y \\| z |"),
        QStringLiteral("> | a | b |\n> |---|---|\n> | c | d |"),
        QStringLiteral("- item\n\n  | a | b |\n  |---|---|\n  | c | d |"),
        QStringLiteral("| a | b |\r\n|---|---|\r\n| c | d |\r\n"),
        QStringLiteral("|| x | y |\n|---|---|---|\n| 1 || 2 |"),
        QStringLiteral("text above\n| a | b |\n|---|---|\n| c | d |\n\ntext below"),
        QStringLiteral("| a | b |\n|---|---|\n| c | d |\n// a note\n| e | f |"),
        QStringLiteral("| 名前 | é |\n|---|---|\n| 👩‍💻 | x |"),
        QStringLiteral("| a | b |\n|:--|--:|\n|   |  |\n| | x |"),
        QStringLiteral("| A | B | C | D |\n|---|---|---|---|\n| Maintenance ||| x |\n| a || b ||"),
        QStringLiteral("| a |\n|---|\n| b |"),
        QStringLiteral("| a | b |\n| - | - |\n| c\\\\ | d\\ |"),
        QStringLiteral("| **bold** | [link](a.md) |\n|:-:|---|\n| `code` | ~~gone~~ |"),
        QStringLiteral("| a | b |\n|---|---|\n| c | d |\n| e |\n| f | g | h | i |"),
        QStringLiteral("| a | b |\n|---|---|\n||| \n| x || y"),
    };
    return texts;
}

// Every line of a written table, its quote markers and indentation left
// out, takes the same columns: the pipes line up.
bool pipesLineUp(const QString &table)
{
    int width = -1;
    for (QString line : table.split(QRegularExpression(QStringLiteral("\r\n|\r|\n")))) {
        int at = 0;
        while (at < line.size() && (line[at] == u' ' || line[at] == u'>' || line[at] == u'\t')) ++at;
        const int w = MdTable::displayWidth(QStringView(line).mid(at));
        if (width >= 0 && w != width) return false;
        width = w;
    }
    return true;
}

} // namespace

namespace TableTests {

void formatKeepsMergedCellsWhole()
{
    const QString merged = QStringLiteral("| A | B | C |\n|---|---|---|\n| Writing      || Reading |");
    const auto format = [](const QString &text, const MarkdownEditing::Options &options = {}) {
        const auto change = MarkdownEditing::formatTable(text, int(text.size()), int(text.size()), options);
        if (!change) return QStringLiteral("(nothing)");
        QString out = text;
        for (auto edit = change->edits.rbegin(); edit != change->edits.rend(); ++edit)
            out.replace(edit->range.start, edit->range.length, edit->replacement);
        return out;
    };
    // Writing covers A and B: its padding goes inside it and the two pipes
    // after it stay side by side.
    QCOMPARE(format(merged), QStringLiteral("| A   | B   | C       |\n| --- | --- | ------- |\n| Writing  || Reading |"));
    QCOMPARE(parsedTables(format(merged), md::Options::writer()), parsedTables(merged, md::Options::writer()));
    QCOMPARE(parsedTables(merged, md::Options::writer()).value(0).value(1).value(0).span, 2);
    // A merged cell wider than its columns widens the last of them.
    const QString wide = QStringLiteral("| A | B | C |\n|---|---|---|\n| A very long merged cell || x |");
    const QString written = format(wide);
    QCOMPARE(written,
             QStringLiteral("| A   | B                  | C   |\n| --- | ------------------ | --- |\n| A very long merged cell || x   |"));
    QVERIFY(pipesLineUp(written));
    QCOMPARE(parsedTables(written, md::Options::writer()), parsedTables(wide, md::Options::writer()));
    // Without merged cells, two pipes side by side are an empty cell, and
    // stay one.
    MarkdownEditing::Options standard;
    standard.protectCodePipes = false;
    standard.mergedCells = false;
    standard.lineComments = false;
    QCOMPARE(format(merged, standard),
             QStringLiteral("| A       | B   | C       |\n| ------- | --- | ------- |\n| Writing |     | Reading |"));
    QCOMPARE(parsedTables(format(merged, standard), md::Options::standard()),
             parsedTables(merged, md::Options::standard()));
    // A cell that was blank stays its own cell, not a merge.
    const QString blank = QStringLiteral("| A | B |\n|---|---|\n| x |  |");
    QCOMPARE(parsedTables(format(blank), md::Options::writer()), parsedTables(blank, md::Options::writer()));
}

void scannerAgreesWithTheParser()
{
    for (const md::Options &options : {md::Options::writer(), md::Options::standard()}) {
        const MdTable::Dialect dialect = dialectFor(options);
        for (const QString &text : fixtures()) {
            const md::Document document = md::parse(text, options);
            QList<const md::Node *> nodes;
            collectTables(document.root, nodes);
            for (const md::Node *node : nodes) {
                const auto table = MdTable::scan(text, node->range.start, node->range.end, dialect);
                QVERIFY2(table, qPrintable(text));
                QCOMPARE(table->columns, node->columns);
                QList<const md::Node *> rows;
                for (const md::Node &row : node->children)
                    if (row.type == md::NodeType::TableRow) rows.append(&row);
                // The rows read here are the parser's, up to a comment line.
                const int read = int(table->rows.size()) - 1;
                if (table->unfit == MdTable::Unfit::CommentLine) QVERIFY(read < rows.size());
                else QCOMPARE(read, int(rows.size()));
                for (int r = 0; r < read; ++r) {
                    const MdTable::Row &row = table->rows[r == 0 ? 0 : r + 1];
                    const md::Node *parsed = rows[r];
                    int cells = 0;
                    for (const MdTable::Cell &cell : row.cells) {
                        if (cell.column >= table->columns) continue;
                        QVERIFY2(cells < int(parsed->children.size()), qPrintable(text));
                        const md::Node &expected = parsed->children[size_t(cells++)];
                        const QString context = text + QStringLiteral(" row %1 cell %2").arg(r).arg(cells);
                        QVERIFY2(cell.start == expected.range.start && cell.end == expected.range.end, qPrintable(context));
                        QVERIFY2(cell.column == expected.column, qPrintable(context));
                        if (table->unfit == MdTable::Unfit::None)
                            QVERIFY2(cell.span == expected.span, qPrintable(context));
                    }
                    QCOMPARE(cells, int(parsed->children.size()));
                }
            }
            // Written tidy, every table says what it said.
            const QString tidy = tidied(text, options);
            QVERIFY2(parsedTables(tidy, options) == parsedTables(text, options),
                     qPrintable(text + QStringLiteral("\n=>\n") + tidy + QStringLiteral("\n")
                                + describe(parsedTables(text, options).value(0)) + QStringLiteral("\n")
                                + describe(parsedTables(tidy, options).value(0))));
        }
    }
}

void formatIsIdempotentAndKeepsPayloadBytes()
{
    for (const md::Options &options : {md::Options::writer(), md::Options::standard()}) {
        for (const QString &text : fixtures()) {
            const QString once = tidied(text, options);
            QCOMPARE(tidied(once, options), once);
            // Each cell keeps its Markdown to the byte, escapes and code included.
            const md::Document before = md::parse(text, options);
            const md::Document after = md::parse(once, options);
            QList<const md::Node *> a, b;
            collectTables(before.root, a);
            collectTables(after.root, b);
            QCOMPARE(a.size(), b.size());
            for (int t = 0; t < a.size(); ++t) {
                const auto first = MdTable::scan(text, a[t]->range.start, a[t]->range.end, dialectFor(options));
                const auto second = MdTable::scan(once, b[t]->range.start, b[t]->range.end, dialectFor(options));
                QVERIFY(first && second);
                const MdTable::Model m1 = MdTable::model(text, *first);
                const MdTable::Model m2 = MdTable::model(once, *second);
                QCOMPARE(m1.aligns, m2.aligns);
                QCOMPARE(m1.rows.size(), m2.rows.size());
                for (int r = 0; r < m1.rows.size(); ++r) {
                    for (int c = 0; c < m1.rows[r].size(); ++c) {
                        QCOMPARE(m2.rows[r].value(c).text, m1.rows[r][c].text);
                        QCOMPARE(m2.rows[r].value(c).span, m1.rows[r][c].span);
                    }
                }
                // Rows with cells past the header are longer; the rest line up.
                if (first->unfit == MdTable::Unfit::None)
                    QVERIFY2(pipesLineUp(once.mid(second->start, second->end - second->start)), qPrintable(once));
            }
        }
    }
    // Where each cell was written: its text, and the spaces before its pipe.
    const MdTable::Layout layout = MdTable::write(MdTable::model(
        u"| a | b |\n|:-:|--:|\n| long text | 1 |", *MdTable::locate(u"| a | b |\n|:-:|--:|\n| long text | 1 |", 0)));
    QCOMPARE(layout.text, QStringLiteral("|     a     |   b |\n| :-------: | --: |\n| long text |   1 |"));
    QCOMPARE(layout.text.mid(layout.cells[0][0].start, layout.cells[0][0].length), QStringLiteral("a"));
    QCOMPARE(layout.cells[0][0].tail, 5);
    QCOMPARE(layout.text.mid(layout.cells[1][1].start, 1), QStringLiteral("1"));
    QCOMPARE(layout.cells[1][1].tail, 1);
    QCOMPARE(layout.lineStarts, (QList<int>{0, 20, 40}));
}

void formatKeepsPrefixesAndLineEndings()
{
    QCOMPARE(formatted(u"> |a|b|\n> |-|-|\n> |c|d|"_s), u"> | a   | b   |\n> | --- | --- |\n> | c   | d   |"_s);
    QCOMPARE(formatted(u"- item\n\n  |a|b|\n  |-|-|\n  |c|d|"_s), u"- item\n\n  | a   | b   |\n  | --- | --- |\n  | c   | d   |"_s);
    // Each line keeps its own indentation and break; the text after the
    // table, and a missing last break, stay as they were.
    QCOMPARE(formatted(u"|a|b|\n |-|-|\n  |c|d|"_s), u"| a   | b   |\n | --- | --- |\n  | c   | d   |"_s);
    QCOMPARE(formatted(u"|a|b|\r\n|-|-|\n|c|d|\r\n\r\nafter"_s), u"| a   | b   |\r\n| --- | --- |\n| c   | d   |\r\n\r\nafter"_s);
    QCOMPARE(formatted(u"|a|b|\r|-|-|\r|c|d|"_s), u"| a   | b   |\r| --- | --- |\r| c   | d   |"_s);
    // Trailing spaces after a row go; a row without its outer pipes gains them.
    QCOMPARE(formatted(u"a | b   \n--|--\nc | d"_s), u"| a   | b   |\n| --- | --- |\n| c   | d   |"_s);
    // The paragraph above the header is no part of the table.
    const QString above = u"above | it\n|a|b|\n|-|-|\n|c|d|"_s;
    const auto table = MdTable::locate(above, int(above.indexOf(u"|c"_s)));
    QVERIFY(table);
    QCOMPARE(table->start, int(above.indexOf(u"|a"_s)));
}

void formatAlignsWideAndCombiningText()
{
    QCOMPARE(formatted(u"| 名前 | é |\n|---|---|\n| 👩‍💻 | x |"_s),
             u"| 名前 | é   |\n| ---- | --- |\n| 👩‍💻   | x   |"_s);
    QVERIFY(pipesLineUp(formatted(u"| 名前 | é |\n|---|---|\n| 👩‍💻 | x |"_s)));
    // Alignment pads on the side the column is aligned to.
    QCOMPARE(formatted(u"| a | b | c |\n|:-|:-:|-:|\n| 東京 | 東京 | 東京 |\n| x | y | z |"_s),
             u"| a    |  b   |    c |\n| :--- | :--: | ---: |\n| 東京 | 東京 | 東京 |\n| x    |  y   |    z |"_s);
    QCOMPARE(MdTable::displayWidth(u"é名👩‍💻"), 5);
    QCOMPARE(MdTable::displayWidth(u"plain"), 5);
    QCOMPARE(MdTable::displayWidth(u"tab\there"), 7);
}

void tablesTheGridCannotHoldSayWhy()
{
    const auto unfit = [](const QString &text, const MdTable::Dialect &dialect = {}) {
        const auto table = MdTable::scan(text, 0, int(text.size()), dialect);
        return table ? std::pair{table->unfit, table->unfitRow} : std::pair{MdTable::Unfit::None, -2};
    };
    MdTable::Dialect standard;
    standard.protectCode = standard.mergedCells = standard.lineComments = false;
    QCOMPARE(unfit(u"| a | b |\n|---|---|\n| 1 | 2 | 3 |"_s), std::pair(MdTable::Unfit::ExtraCells, 2));
    QCOMPARE(unfit(u"| a | b |\n|---|---|\n| 1 || 2 | 3 |"_s), std::pair(MdTable::Unfit::ExtraCells, 2));
    QCOMPARE(unfit(u"| a | b |\n|---|---|\n|| x |"_s), std::pair(MdTable::Unfit::LeadingMerge, 2));
    QCOMPARE(unfit(u"| a | b |\n|---|---|\n|| x |"_s, standard), std::pair(MdTable::Unfit::None, -1));
    QCOMPARE(unfit(u"| a | b |\n|---|---|\n| c | d |\n// note\n| e | f |"_s), std::pair(MdTable::Unfit::CommentLine, 3));
    QCOMPARE(unfit(u"| a | b |\n|---|---|\n| c | d |\n// note\n| e | f |"_s, standard), std::pair(MdTable::Unfit::None, -1));
    QString wide = QStringLiteral("|") + QStringLiteral(" a |").repeated(129) + u'\n' + u'|' + QStringLiteral("---|").repeated(129);
    QCOMPARE(unfit(wide).first, MdTable::Unfit::TooLarge);
    QString tall = QStringLiteral("| a | b |\n|---|---|");
    for (int i = 0; i < 10001; ++i) tall += QStringLiteral("\n| x | y |");
    QCOMPARE(unfit(tall).first, MdTable::Unfit::TooLarge);
    // A short row is fine: it is filled out when the table is written.
    QCOMPARE(unfit(u"| a | b |\n|---|---|\n| x |"_s), std::pair(MdTable::Unfit::None, -1));
    QCOMPARE(formatted(u"| a | b |\n|---|---|\n| x |"_s), u"| a   | b   |\n| --- | --- |\n| x   |     |"_s);
    // A header and delimiter row that disagree make no table.
    QCOMPARE(unfit(u"| a | b |\n|---|"_s).second, -2);
    QCOMPARE(unfit(u"| a | b |"_s).second, -2);
}

} // namespace TableTests
