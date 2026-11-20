#include "tabletests.h"

#include "markdownediting.h"
#include "mdtable.h"
#include "mdtree.h"
#include "focusmode.h"
#include "sourcebuffer.h"
#include "tablegrids.h"

#include <QRandomGenerator>
#include <functional>
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
    // Lines indented or quoted differently could not be written back alike.
    QCOMPARE(unfit(u"  | A | B |\n    |---|---|\n   | one | two |"_s), std::pair(MdTable::Unfit::MixedPrefixes, 1));
    QCOMPARE(unfit(u"> | A | B |\n> |---|---|\n>| one | two |"_s), std::pair(MdTable::Unfit::MixedPrefixes, 2));
    QCOMPARE(unfit(u"> | A | B |\n> |---|---|\n> | one | two |"_s), std::pair(MdTable::Unfit::None, -1));
    // A short row is fine: it is filled out when the table is written.
    QCOMPARE(unfit(u"| a | b |\n|---|---|\n| x |"_s), std::pair(MdTable::Unfit::None, -1));
    QCOMPARE(formatted(u"| a | b |\n|---|---|\n| x |"_s), u"| a   | b   |\n| --- | --- |\n| x   |     |"_s);
    // A header and delimiter row that disagree make no table.
    QCOMPARE(unfit(u"| a | b |\n|---|"_s).second, -2);
    QCOMPARE(unfit(u"| a | b |"_s).second, -2);
}

void runsOfReplacementsUndoAsOneChange()
{
    const QString start = u"| a   | b   |\n| --- | --- |\n| c   | d   |"_s;
    SourceBuffer buffer(start);
    qint64 now = 0;
    buffer.setClock([&] { return now; });
    // Each key rewrites the table, as typing in a cell does; the run is one step.
    QCOMPARE(buffer.replace({{{30, 1}, u"cx"_s}}, EditKind::Typing, 7), SourceBuffer::EditOutcome::Applied);
    now += 100;
    QCOMPARE(buffer.replace({{{2, 31}, u"a    | b   |\n| ---- | --- |\n| cxy"_s}}, EditKind::Typing, 7),
             SourceBuffer::EditOutcome::Applied);
    now += 100;
    const QString typed = buffer.text();
    QVERIFY(buffer.canUndo());
    buffer.undo();
    QCOMPARE(buffer.text(), start);
    QVERIFY(!buffer.canUndo());
    buffer.redo();
    QCOMPARE(buffer.text(), typed);
    // Another run, or a kind of edit that differs, is a step of its own.
    QCOMPARE(buffer.replace({{{0, 0}, u"x"_s}}, EditKind::Typing, 8), SourceBuffer::EditOutcome::Applied);
    QCOMPARE(buffer.replace({{{0, 1}, u""_s}}, EditKind::Deleting, 8), SourceBuffer::EditOutcome::Applied);
    buffer.undo();
    QCOMPARE(buffer.text(), u"x"_s + typed);
    buffer.undo();
    QCOMPARE(buffer.text(), typed);
    // Edits of no run, or of kind Other, never join one.
    QCOMPARE(buffer.replace({{{0, 0}, u"y"_s}}, EditKind::Other, 9), SourceBuffer::EditOutcome::Applied);
    QCOMPARE(buffer.replace({{{0, 0}, u"z"_s}}, EditKind::Other, 9), SourceBuffer::EditOutcome::Applied);
    buffer.undo();
    QCOMPARE(buffer.text(), u"y"_s + typed);
}

void aRunEndsAtASaveAPauseOrAnUndo()
{
    SourceBuffer buffer(u"abc"_s);
    qint64 now = 0;
    buffer.setClock([&] { return now; });
    buffer.replace({{{3, 0}, u"d"_s}}, EditKind::Typing, 1);
    buffer.markSaved();
    buffer.replace({{{4, 0}, u"e"_s}}, EditKind::Typing, 1);
    // A run does not join the step the saved text ends.
    buffer.undo();
    QCOMPARE(buffer.text(), u"abcd"_s);
    QVERIFY(!buffer.modified());
    buffer.redo();
    // A pause as long as the one that ends typing ends a run.
    now += 2500;
    buffer.replace({{{5, 0}, u"f"_s}}, EditKind::Typing, 1);
    buffer.undo();
    QCOMPARE(buffer.text(), u"abcde"_s);
    // After an undo, the run's next edit starts a step.
    buffer.replace({{{5, 0}, u"g"_s}}, EditKind::Typing, 1);
    now += 10;
    buffer.replace({{{6, 0}, u"h"_s}}, EditKind::Typing, 1);
    buffer.undo();
    QCOMPARE(buffer.text(), u"abcde"_s);
}

void composedRunsRestoreTheExactBytes()
{
    // Random overlapping and separate edits of one run: one undo gives back
    // the text from before the first, one redo the text after the last.
    QRandomGenerator random(20261119);
    for (int round = 0; round < 200; ++round) {
        QString text;
        const int length = 20 + random.bounded(60);
        for (int i = 0; i < length; ++i) text += QChar(u'a' + random.bounded(26));
        if (round % 3 == 0) text.insert(random.bounded(length), u"\r\n|"_s);
        SourceBuffer buffer(text);
        qint64 now = 0;
        buffer.setClock([&] { return now; });
        const QString before = buffer.text();
        for (int edit = 0; edit < 12; ++edit) {
            const int size = buffer.length();
            const int start = random.bounded(size + 1);
            const int removed = random.bounded(qMin(8, size - start) + 1);
            QString inserted;
            for (int i = random.bounded(10); i > 0; --i) inserted += QChar(u'A' + random.bounded(26));
            const SourceRange range{start, removed};
            // Never half a CRLF pair, as the buffer would refuse it.
            if (start > 0 && start < size && buffer.text()[start - 1] == u'\r' && buffer.text()[start] == u'\n') continue;
            const int end = start + removed;
            if (end > 0 && end < size && buffer.text()[end - 1] == u'\r' && buffer.text()[end] == u'\n') continue;
            buffer.replace({{range, inserted}}, EditKind::Typing, 42);
            now += 10;
        }
        const QString after = buffer.text();
        if (after == before) continue;
        buffer.undo();
        QCOMPARE(buffer.text(), before);
        QVERIFY(!buffer.canUndo());
        buffer.redo();
        QCOMPARE(buffer.text(), after);
    }
}

namespace {

// What a change makes of the table that holds the text's first pipe,
// written tidy; its refusal in brackets when it refuses.
QString changed(const QString &text, const std::function<QString(MdTable::Model &)> &change,
                const MdTable::Dialect &dialect = {})
{
    const auto table = MdTable::locate(text, int(text.indexOf(u'|')), dialect);
    if (!table) return u"(no table)"_s;
    MdTable::Model model = MdTable::model(text, *table);
    const QString refused = change(model);
    if (!refused.isEmpty()) return u"("_s + refused + u")"_s;
    QString out = text;
    out.replace(table->start, table->end - table->start, MdTable::write(model).text);
    return out;
}

// A text with each table written tidy, as one of its edits writes it.
QString tidy(const QString &text, const MdTable::Dialect &dialect = {})
{
    return changed(text, [](MdTable::Model &) { return QString(); }, dialect);
}

} // namespace

void everyOperationLeavesOneTidyEdit()
{
    const QStringList tables{
        u"| a | b | c |\n|---|:-:|--:|\n| 1 | 2 | 3 |\n| 4 | 5 | 6 |"_s,
        u"| a   |  b  |   c |\n| --- | :-: | --: |\n| 1   |  2  |   3 |\n| 4   |  5  |   6 |"_s,
        u"a|b|c\n-|-|-\n1|2\n4|5|6"_s,
    };
    using Change = std::function<QString(MdTable::Model &)>;
    const QList<Change> changes{
        [](MdTable::Model &m) { return MdTable::insertRows(m, 1, 1); },
        [](MdTable::Model &m) { return MdTable::insertRows(m, 3, 2); },
        [](MdTable::Model &m) { return MdTable::deleteRows(m, 1, 1); },
        [](MdTable::Model &m) { return MdTable::deleteRows(m, 1, 2); },
        [](MdTable::Model &m) { return MdTable::duplicateRows(m, 2, 1); },
        [](MdTable::Model &m) { return MdTable::moveRows(m, 2, 1, 1); },
        [](MdTable::Model &m) { return MdTable::insertColumns(m, 0, 1); },
        [](MdTable::Model &m) { return MdTable::insertColumns(m, 3, 1); },
        [](MdTable::Model &m) { return MdTable::deleteColumns(m, 1, 1); },
        [](MdTable::Model &m) { return MdTable::moveColumns(m, 2, 1, 0); },
        [](MdTable::Model &m) { return MdTable::setAlignment(m, 0, 2, MdTable::Align::Right); },
        [](MdTable::Model &m) { return MdTable::sortRows(m, 0, true); },
        [](MdTable::Model &m) { return MdTable::mergeCells(m, 1, 0, 1); },
        [](MdTable::Model &m) { return MdTable::clearCells(m, 1, 2, 1, 2); },
        [](MdTable::Model &m) { return MdTable::pasteCells(m, 2, 2, {{u"x"_s, u"y"_s}, {u"z"_s}}); },
    };
    for (const QString &text : tables) {
        for (int i = 0; i < changes.size(); ++i) {
            const QString after = changed(text, changes[i]);
            const QString context = text + u" change "_s + QString::number(i) + u"\n"_s + after;
            QVERIFY2(!after.startsWith(u'('), qPrintable(context));
            // A table still, tidy, and read the same by the parser and the reader.
            QCOMPARE(tidy(after), after);
            const auto table = MdTable::locate(after, 0);
            QVERIFY2(table && table->unfit == MdTable::Unfit::None, qPrintable(context));
            QCOMPARE(parsedTables(after, md::Options::writer()).size(), 1);
        }
    }
    // A move to where the rows already are changes nothing.
    QCOMPARE(changed(tables[1], [](MdTable::Model &m) { return MdTable::moveRows(m, 1, 1, 2); }), tables[1]);
    // The header stays, and so does one column.
    QVERIFY(changed(tables[1], [](MdTable::Model &m) { return MdTable::deleteRows(m, 0, 1); }).startsWith(u'('));
    QVERIFY(changed(tables[1], [](MdTable::Model &m) { return MdTable::deleteColumns(m, 0, 3); }).startsWith(u'('));
    // Each change, exactly.
    QCOMPARE(changed(tables[1], [](MdTable::Model &m) { return MdTable::insertColumns(m, 1, 1); }),
             u"| a   |     |  b  |   c |\n| --- | --- | :-: | --: |\n| 1   |     |  2  |   3 |\n| 4   |     |  5  |   6 |"_s);
    QCOMPARE(changed(tables[1], [](MdTable::Model &m) { return MdTable::moveColumns(m, 0, 1, 3); }),
             u"|  b  |   c | a   |\n| :-: | --: | --- |\n|  2  |   3 | 1   |\n|  5  |   6 | 4   |"_s);
    QCOMPARE(changed(tables[1], [](MdTable::Model &m) { return MdTable::duplicateRows(m, 1, 1); }),
             u"| a   |  b  |   c |\n| --- | :-: | --: |\n| 1   |  2  |   3 |\n| 1   |  2  |   3 |\n| 4   |  5  |   6 |"_s);
    QCOMPARE(changed(tables[1], [](MdTable::Model &m) { return MdTable::setAlignment(m, 0, 1, MdTable::Align::Center); }),
             u"|  a  |  b  |   c |\n| :-: | :-: | --: |\n|  1  |  2  |   3 |\n|  4  |  5  |   6 |"_s);
}

void mergedCellsWidenNarrowAndRefuseCuts()
{
    const QString merged = u"| A   | B   | C   |\n| --- | --- | --- |\n| Maintenance || x   |\n| 1   | 2   | 3   |"_s;
    // A column inserted inside the merge widens it; at its edge, it does not.
    const QString widened = changed(merged, [](MdTable::Model &m) { return MdTable::insertColumns(m, 1, 1); });
    QVERIFY2(widened.contains(u"| Maintenance   ||| x   |"_s), qPrintable(widened));
    QCOMPARE(parsedTables(widened, md::Options::writer()).value(0).value(1).value(0).span, 3);
    const QString beside = changed(merged, [](MdTable::Model &m) { return MdTable::insertColumns(m, 2, 1); });
    QCOMPARE(parsedTables(beside, md::Options::writer()).value(0).value(1).value(0).span, 2);
    // Deleting a column it covers narrows it; deleting both takes it away.
    const QString narrowed = changed(merged, [](MdTable::Model &m) { return MdTable::deleteColumns(m, 0, 1); });
    QCOMPARE(parsedTables(narrowed, md::Options::writer()).value(0).value(1).value(0).text, u"Maintenance"_s);
    QCOMPARE(parsedTables(narrowed, md::Options::writer()).value(0).value(1).value(0).span, 1);
    const QString gone = changed(merged, [](MdTable::Model &m) { return MdTable::deleteColumns(m, 0, 2); });
    QCOMPARE(parsedTables(gone, md::Options::writer()).value(0).value(1).value(0).text, u"x"_s);
    // A move that would cut it is refused; one that takes it whole is not.
    QVERIFY(changed(merged, [](MdTable::Model &m) { return MdTable::moveColumns(m, 1, 1, 3); }).startsWith(u'('));
    QVERIFY(changed(merged, [](MdTable::Model &m) { return MdTable::moveColumns(m, 2, 1, 1); }).startsWith(u'('));
    const QString moved = changed(merged, [](MdTable::Model &m) { return MdTable::moveColumns(m, 0, 2, 3); });
    QCOMPARE(parsedTables(moved, md::Options::writer()).value(0).value(1).value(1).text, u"Maintenance"_s);
    // Rows move and sort with their merges.
    const QString sorted = changed(merged, [](MdTable::Model &m) { return MdTable::sortRows(m, 2, false); });
    QCOMPARE(parsedTables(sorted, md::Options::writer()).value(0).value(1).value(0).text, u"1"_s);
    QCOMPARE(parsedTables(sorted, md::Options::writer()).value(0).value(2).value(0).span, 2);
    // Merge and split, back and forth.
    const QString joined = changed(merged, [](MdTable::Model &m) { return MdTable::mergeCells(m, 2, 1, 2); });
    QVERIFY2(joined.contains(u"| 1   | 2 3         ||"_s), qPrintable(joined));
    QCOMPARE(changed(joined, [](MdTable::Model &m) { return MdTable::splitCell(m, 2, 2); }),
             u"| A   | B      | C   |\n| --- | ------ | --- |\n| Maintenance || x   |\n| 1   | 2 3    |     |"_s);
    QVERIFY(changed(merged, [](MdTable::Model &m) { return MdTable::splitCell(m, 2, 0); }).startsWith(u'('));
    // A paste over part of it is refused.
    QVERIFY(changed(merged, [](MdTable::Model &m) { return MdTable::pasteCells(m, 1, 1, {{u"p"_s}}); }).startsWith(u'('));
    // Without merged cells there is nothing to merge.
    MdTable::Dialect standard;
    standard.protectCode = standard.mergedCells = false;
    QVERIFY(changed(u"| a | b |\n|---|---|\n| 1 | 2 |"_s,
                    [&](MdTable::Model &m) { return MdTable::mergeCells(m, 1, 0, 1, standard); }, standard)
                .startsWith(u'('));
}

void sortingOrdersNumbersAsNumbersAndKeepsTies()
{
    const QString readings = u"| t | v |\n|---|---|\n| a | 10 |\n| b | 2.0 |\n| c |  |\n| d | 2 |\n| e | **pear** |\n| f | Apple |"_s;
    const auto order = [](const QString &text) {
        QString letters;
        for (const QList<ParsedCell> &row : parsedTables(text, md::Options::writer()).value(0).mid(1))
            letters += row.value(0).text;
        return letters;
    };
    QCOMPARE(order(changed(readings, [](MdTable::Model &m) { return MdTable::sortRows(m, 1, false); })), u"bdafec"_s);
    QCOMPARE(order(changed(readings, [](MdTable::Model &m) { return MdTable::sortRows(m, 1, true); })), u"abdefc"_s);
    QCOMPARE(MdTable::plainText(u"**OK** [log](log.md) `S1` E\\|W"_s), u"OK log S1 E|W"_s);
}

void movedRowsKeepTheirLineEndings()
{
    // The line breaks stay where they were; the rows move between them.
    const QString mixed = u"| a | b |\r\n|---|---|\r\n| 1 | 2 |\n| 3 | 4 |\r\n\r\nafter"_s;
    QCOMPARE(changed(mixed, [](MdTable::Model &m) { return MdTable::moveRows(m, 2, 1, 1); }),
             u"| a   | b   |\r\n| --- | --- |\r\n| 3   | 4   |\n| 1   | 2   |\r\n\r\nafter"_s);
    // New rows take the document's break; the table's last line keeps having none of its own.
    QCOMPARE(changed(u"| a | b |\r\n|---|---|\r\n| 1 | 2 |"_s,
                     [](MdTable::Model &m) { return MdTable::insertRows(m, 2, 1, u"\r\n"_s); }),
             u"| a   | b   |\r\n| --- | --- |\r\n| 1   | 2   |\r\n|     |     |"_s);
    QCOMPARE(changed(u"| a | b |\r\n|---|---|\r\n| 1 | 2 |\r\n| 3 | 4 |"_s,
                     [](MdTable::Model &m) { return MdTable::deleteRows(m, 2, 1); }),
             u"| a   | b   |\r\n| --- | --- |\r\n| 1   | 2   |"_s);
}

void pipesTypedInCellsAreEscapedByDialect()
{
    MdTable::Dialect standard;
    standard.protectCode = standard.mergedCells = false;
    QCOMPARE(MdTable::encodeCell(u"a|b"_s), u"a\\|b"_s);
    QCOMPARE(MdTable::encodeCell(u"|a|"_s), u"\\|a\\|"_s);
    QCOMPARE(MdTable::encodeCell(u"a\\|b"_s), u"a\\|b"_s);
    QCOMPARE(MdTable::encodeCell(u"a\\\\|b"_s), u"a\\\\\\|b"_s);
    // In code, the Writer dialect keeps a pipe as it is; GFM needs it escaped.
    QCOMPARE(MdTable::encodeCell(u"`a|b`"_s), u"`a|b`"_s);
    QCOMPARE(MdTable::encodeCell(u"`a|b`"_s, standard), u"`a\\|b`"_s);
    // One line, without the spaces around it.
    QCOMPARE(MdTable::encodeCell(u"  two\r\nlines\n "_s), u"two lines"_s);
    // Written into a table, each reads back as one cell with its text.
    for (const QString &value : {u"a|b"_s, u"|a|"_s, u"x\\"_s, u"`c|d`"_s}) {
        const QString text = changed(u"| a | b |\n|---|---|\n| 1 | 2 |"_s,
                                     [&](MdTable::Model &m) { return MdTable::pasteCells(m, 1, 0, {{value}}); });
        const auto parsed = parsedTables(text, md::Options::writer()).value(0);
        QCOMPARE(parsed.value(1).size(), 2);
    }
}

void tablesInQuotesKeepTheirPrefix()
{
    const QString quoted = u"> | a | b |\n> |---|---|\n> | 1 | 2 |"_s;
    QCOMPARE(changed(quoted, [](MdTable::Model &m) { return MdTable::insertRows(m, 2, 1); }),
             u"> | a   | b   |\n> | --- | --- |\n> | 1   | 2   |\n> |     |     |"_s);
    const QString listed = u"- item\n\n  | a | b |\n  |---|---|\n  | 1 | 2 |"_s;
    QCOMPARE(changed(listed, [](MdTable::Model &m) { return MdTable::insertColumns(m, 2, 1); }),
             u"- item\n\n  | a   | b   |     |\n  | --- | --- | --- |\n  | 1   | 2   |     |"_s);
    QCOMPARE(parsedTables(changed(listed, [](MdTable::Model &m) { return MdTable::insertRows(m, 1, 1); }),
                          md::Options::writer())
                 .value(0)
                 .size(),
             3);
}

void insertedTablesAreTidyAndEmpty()
{
    const MdTable::Layout layout = MdTable::write(MdTable::emptyTable(3, 3));
    QCOMPARE(layout.text, u"|     |     |     |\n| --- | --- | --- |\n|     |     |     |\n|     |     |     |"_s);
    QCOMPARE(tidy(layout.text), layout.text);
    // Every cell is its own, not a merge, in Writer too.
    const auto parsed = parsedTables(layout.text, md::Options::writer()).value(0);
    QCOMPARE(parsed.size(), 3);
    for (const QList<ParsedCell> &row : parsed) QCOMPARE(row.size(), 3);
    QCOMPARE(MdTable::write(MdTable::emptyTable(2, 1, u"> "_s, u"\r\n"_s)).text, u"> |     |     |\r\n> | --- | --- |"_s);
}

} // namespace TableTests

namespace TableTests {

void anEditOutsideARunIsAStepOfItsOwn()
{
    // A run of typing in a cell, and then typing just after it that is no
    // part of the run, as another window's: undo takes back only the last.
    SourceBuffer buffer(u"alpha"_s);
    qint64 now = 0;
    buffer.setClock([&] { return now; });
    QCOMPARE(buffer.replace({{{5, 0}, u"x"_s}}, EditKind::Typing, 71), SourceBuffer::EditOutcome::Applied);
    now += 50;
    QCOMPARE(buffer.replace({6, 0}, u"y"_s, EditKind::Typing), SourceBuffer::EditOutcome::Applied);
    buffer.undo();
    QCOMPARE(buffer.text(), u"alphax"_s);
    buffer.undo();
    QCOMPARE(buffer.text(), u"alpha"_s);
}

void cellEditsWithoutAViewKeepTablesTidy()
{
    // A link a move rewrites, in a cell: the column widens in every row.
    const QString text = u"Intro\n\n| Link      | B   |\n| --------- | --- |\n| [x](a.md) | end |\n\nSee [y](a.md)."_s;
    const int inCell = int(text.indexOf(u"a.md"));
    const int inProse = int(text.lastIndexOf(u"a.md"));
    const QList<SourceEdit> edits{{{inCell, 4}, u"longer-name.md"_s}, {{inProse, 4}, u"longer-name.md"_s}};
    const QList<SourceEdit> tidied = TableGrids::tidyCellEdits(text, edits, MdTable::Dialect{});
    QString result = text;
    for (auto edit = tidied.crbegin(); edit != tidied.crend(); ++edit)
        result.replace(edit->range.start, edit->range.length, edit->replacement);
    QCOMPARE(result, u"Intro\n\n| Link                | B   |\n| ------------------- | --- |\n| [x](longer-name.md) | end |\n\n"
                     "See [y](longer-name.md)."_s);
}

void focusLightsAWholeTable()
{
    const QString text = u"Before.\n\n| a | b |\n| --- | --- |\n| x | two |\n\nAfter."_s;
    const int start = int(text.indexOf(u"| a"));
    const int end = int(text.indexOf(u"two |")) + 5;
    const SourceRange lit = FocusMode::rangeAt(text, int(text.indexOf(u"two")), FocusMode::Unit::Paragraph);
    QCOMPARE(lit.start, start);
    QCOMPARE(lit.length, end - start);    // By sentence too: a table is one part.
    const SourceRange sentence = FocusMode::rangeAt(text, int(text.indexOf(u"two")), FocusMode::Unit::Sentence);
    QCOMPARE(sentence.start, start);
    QCOMPARE(sentence.length, end - start);
}

void shiftTabIntoARowOfAPipeAloneStopsThere()
{
    // A row written as a pipe alone has only the cells the header gives
    // it, none written: Shift+Tab into it goes to the first.
    const QString text = u"| a | b |\n|---|---|\n|\n| c | d |"_s;
    const int c = int(text.indexOf(u"c"));
    const auto moved = MarkdownEditing::moveToCell(text, c, c, true);
    QVERIFY(moved);
    const int pipe = int(text.indexOf(u"\n|\n")) + 2;
    QCOMPARE(moved->anchor, pipe);
    QCOMPARE(moved->cursor, pipe);
}

} // namespace TableTests
