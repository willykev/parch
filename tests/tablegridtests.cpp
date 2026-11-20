#include "tablegridtests.h"

#include "apppaths.h"
#include "documentfind.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "mdtable.h"
#include "sourcebuffer.h"
#include "tablegrids.h"
#include "testfiles.h"
#include "theme.h"
#include "vimmode.h"
#include "windowtesting.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QInputMethodQueryEvent>
#include <QMimeData>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QStyleHints>
#include <QWheelEvent>
#include <QtTest>
#include <memory>

using namespace Qt::StringLiterals;

namespace {

// One editor over one document with its tables shown as grids, as a
// window shows it.
struct GridEditor {
    QTemporaryDir root;
    QTemporaryDir themeFolder;
    std::unique_ptr<Theme> theme;
    std::unique_ptr<DocumentStore> store;
    QQmlEngine engine;
    QQuickWindow window;
    Document *document = nullptr;
    QQuickItem *pane = nullptr;
    QQuickItem *editor = nullptr;
    EditorBinding *binding = nullptr;
    QString error;

    explicit GridEditor(const QString &text, QSize size = QSize(640, 480))
    {
        writeTheme(themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
        theme = std::make_unique<Theme>(themeFolder.path());
        store = std::make_unique<DocumentStore>(AppPaths::under(root.path()));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), theme.get());
        writeFile(root.filePath(QStringLiteral("doc.md")), text.toUtf8());
        document = store->open(root.filePath(QStringLiteral("doc.md")));
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/EditorPane.qml")));
        pane = qobject_cast<QQuickItem *>(component.create());
        if (!pane) {
            error = component.errorString();
            return;
        }
        pane->setParent(&window);
        pane->setParentItem(window.contentItem());
        pane->setSize(size);
        window.resize(size);
        pane->setProperty("tableGrids", true);
        pane->setProperty("document", QVariant::fromValue(document));
        editor = pane->findChild<QQuickItem *>(QStringLiteral("editor"));
        binding = pane->findChild<EditorBinding *>();
        window.show();
        if (editor) editor->forceActiveFocus();
        QCoreApplication::processEvents();
    }
    bool ok() const { return document && pane && editor && binding; }
    // Waits for the parse that finds the tables and their grids.
    bool grids(int count)
    {
        QElapsedTimer timer;
        timer.start();
        while (binding->tables()->grids().size() != count && timer.elapsed() < 5000) QTest::qWait(10);
        return binding->tables()->grids().size() == count;
    }
    TableGrid *grid(int index = 0) const { return binding->tables()->grids().value(index); }
    QString text() const { return document->buffer()->text(); }
    void select(int anchor, int cursor) { binding->select(anchor, cursor); }
    int cursor() const { return binding->cursorSource(); }
    int anchor() const { return binding->anchorSource(); }
    // The cell the cursor is in, as its text, or "-" outside the grids.
    QString cell() const
    {
        TableGrid *grid = binding->tables()->gridAt(cursor());
        if (!grid) return QStringLiteral("-");
        const TableGrid::Place place = grid->placeOf(cursor());
        if (!place.valid()) return QStringLiteral("(syntax)");
        const MdTable::Cell &cell = grid->cellAt(place);
        return text().mid(cell.start, cell.end - cell.start);
    }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { sendKey(&window, key, modifiers); }
    void type(const QString &typed) { typeText(&window, typed); }
    // A second view of the same document, in a window of its own.
    struct Second {
        QQuickWindow window;
        QQuickItem *pane = nullptr;
        EditorBinding *binding = nullptr;
    };
    std::unique_ptr<Second> secondView()
    {
        auto second = std::make_unique<Second>();
        QQmlComponent component(&engine, QUrl(QStringLiteral("qrc:/EditorPane.qml")));
        second->pane = qobject_cast<QQuickItem *>(component.create());
        if (!second->pane) return nullptr;
        second->pane->setParent(&second->window);
        second->pane->setParentItem(second->window.contentItem());
        second->pane->setSize(QSizeF(640, 480));
        second->window.resize(640, 480);
        second->pane->setProperty("tableGrids", true);
        second->pane->setProperty("document", QVariant::fromValue(document));
        second->binding = second->pane->findChild<EditorBinding *>();
        second->window.show();
        QCoreApplication::processEvents();
        return second;
    }
};

const QString weather = u"Before the table.\n"
                        "\n"
                        "| Time  | Shelter | Air |\n"
                        "| ----- | ------- | --- |\n"
                        "| 06:00 | North   | 7.8 |\n"
                        "| 10:00 | East    | 12  |\n"
                        "\n"
                        "After the table."_s;

} // namespace

namespace TableGridTests {

void caretRestsInAHiddenRowAndNoKeyEjectsIt()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    // The header line is as tall as the grid; the rest of the table is hidden.
    QTextDocument *doc = view.binding->textDocument();
    const QTextBlock header = doc->findBlock(view.binding->sourceToEditor(grid->start()));
    QCOMPARE(header.blockFormat().lineHeight(), std::ceil(grid->height()));
    QVERIFY(header.blockFormat().nonBreakableLines());
    int hidden = 0;
    for (QTextBlock block = header.next(); block.isValid() && block.position() <= view.binding->sourceToEditor(grid->end());
         block = block.next())
        hidden += block.isVisible() ? 0 : 1;
    QCOMPARE(hidden, 3);
    QCOMPARE(grid->rowCount(), 3);
    QCOMPARE(grid->columnCount(), 3);
    QVERIFY(grid->height() > 3 * 24);

    // From cell to cell with the arrows, never into the pipes.
    const int north = int(view.text().indexOf(u"North"));
    view.select(north, north);
    QCOMPARE(view.cell(), u"North"_s);
    QVERIFY(view.binding->tables()->caretInGrid());
    for (int i = 0; i < 5; ++i) view.key(Qt::Key_Right);
    QCOMPARE(view.cursor(), north + 5);
    view.key(Qt::Key_Right);
    QCOMPARE(view.cell(), u"7.8"_s);
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"7.8")));
    view.key(Qt::Key_Left);
    QCOMPARE(view.cell(), u"North"_s);
    QCOMPARE(view.cursor(), north + 5);
    view.key(Qt::Key_Down);
    QCOMPARE(view.cell(), u"East"_s);
    view.key(Qt::Key_Up);
    view.key(Qt::Key_Up);
    QCOMPARE(view.cell(), u"Shelter"_s);
    // Up from the header leaves the table for the text above it.
    view.key(Qt::Key_Up);
    QCOMPARE(view.cell(), u"-"_s);
    QVERIFY(view.cursor() < grid->start());
    // Down from the text above enters the header again.
    view.key(Qt::Key_Down);
    QVERIFY2(view.cell() != u"-"_s && view.cell() != u"(syntax)"_s, qPrintable(view.cell()));
    view.key(Qt::Key_End);
    view.key(Qt::Key_Home);
    QVERIFY(view.binding->tables()->gridAt(view.cursor()));
    // Tab and Shift+Tab select the next and previous cell's text.
    const int time = int(view.text().indexOf(u"Time"));
    view.select(time, time);
    view.key(Qt::Key_Tab);
    QCOMPARE(view.text().mid(qMin(view.anchor(), view.cursor()), qAbs(view.cursor() - view.anchor())), u"Shelter"_s);
    view.key(Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(view.text().mid(qMin(view.anchor(), view.cursor()), qAbs(view.cursor() - view.anchor())), u"Time"_s);
    // The caret is drawn where the grid has it, in the editor's coordinates.
    view.select(north, north);
    QVERIFY(grid->hasCaret());
    QVERIFY(grid->caretRect().height() > 0);
    const QRectF caret = *view.binding->tables()->rectangleFor(north);
    QVERIFY(caret.top() >= grid->y() && caret.bottom() <= grid->y() + grid->height());
    // A position set in the pipes from outside is moved into the cell's text.
    const int padding = int(view.text().indexOf(u"| North")) + 1;
    view.select(padding, padding);
    QCOMPARE(view.cell(), u"North"_s);
    QCOMPARE(view.cursor(), north);
    // Below the table the caret is the text's again.
    const int after = int(view.text().indexOf(u"After"));
    view.select(after, after);
    QCOMPARE(view.cell(), u"-"_s);
    QVERIFY(!view.binding->tables()->caretInGrid());
}

void typingInACellEditsTheSourceAndKeepsItTidy()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const QString before = view.text();
    const int east = int(view.text().indexOf(u"East"));
    view.select(east + 4, east + 4);
    view.type(u" Gate"_s);
    QCOMPARE(view.text(), u"Before the table.\n"
                          "\n"
                          "| Time  | Shelter   | Air |\n"
                          "| ----- | --------- | --- |\n"
                          "| 06:00 | North     | 7.8 |\n"
                          "| 10:00 | East Gate | 12  |\n"
                          "\n"
                          "After the table."_s);
    QCOMPARE(view.cell(), u"East Gate"_s);
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"East Gate")) + 9);
    QVERIFY(view.binding->tables()->gridAt(view.cursor()));
    // The typing is one step to undo, the table's widening with it.
    view.binding->undo();
    QCOMPARE(view.text(), before);
    view.binding->redo();
    QCOMPARE(view.cell(), u"East Gate"_s);
    // Deleting shrinks the column again.
    for (int i = 0; i < 5; ++i) view.key(Qt::Key_Backspace);
    QCOMPARE(view.text(), before);
    // A pipe typed in a cell is written escaped, and the table stays a grid.
    view.type(u"|W"_s);
    QVERIFY2(view.text().contains(u"| East\\|W |"_s), qPrintable(view.text()));
    QCOMPARE(view.binding->tables()->grids().size(), 1);
    // Backspace at the start of a cell, and Delete past its end, leave the table as it is.
    const QString now = view.text();
    const int north = int(now.indexOf(u"North"));
    view.select(north, north);
    view.key(Qt::Key_Backspace);
    view.select(north + 5, north + 5);
    view.key(Qt::Key_Delete);
    QCOMPARE(view.text(), now);
    // Every edit undoes to the exact text before it.
    while (view.document->buffer()->canUndo()) view.binding->undo();
    QCOMPARE(view.text(), before);
}

void inputMethodComposesAtTheGridCaret()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int north = int(view.text().indexOf(u"North"));
    view.select(north + 5, north + 5);
    sendPreedit(&view.window, u"ka"_s);
    // The input method is told the caret is where the grid draws it.
    QInputMethodQueryEvent query(Qt::ImCursorRectangle);
    QCoreApplication::sendEvent(view.editor, &query);
    const QRectF reported = query.value(Qt::ImCursorRectangle).toRectF();
    const QRectF drawn = *view.binding->tables()->rectangleFor(view.cursor());
    QCOMPARE(reported.topLeft(), drawn.topLeft());
    TableGrid *grid = view.grid();
    QVERIFY(reported.top() >= grid->y() && reported.top() < grid->y() + grid->height());
    // The commit is written into the cell, the table tidy around it.
    QInputMethodEvent commit;
    commit.setCommitString(u"北"_s);
    QCoreApplication::sendEvent(view.editor, &commit);
    QVERIFY2(view.text().contains(u"| North北 |"_s), qPrintable(view.text()));
    QCOMPARE(view.cell(), u"North北"_s);
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"North北")) + 6);
}

void vimLinesMoveFromCellToCell()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.pane->setProperty("vimKeys", true);
    QVERIFY(view.grids(1));
    const int north = int(view.text().indexOf(u"North"));
    view.select(north, north);
    sendKey(&view.window, u'j');
    QCOMPARE(view.cell(), u"East"_s);
    sendKey(&view.window, u'k');
    QCOMPARE(view.cell(), u"North"_s);
    // Over the delimiter row to the header.
    sendKey(&view.window, u'k');
    QCOMPARE(view.cell(), u"Shelter"_s);
    sendKey(&view.window, u'j');
    QCOMPARE(view.cell(), u"North"_s);
    // Across a row, cell by cell.
    sendKey(&view.window, u'w');
    QVERIFY2(view.cell() != u"(syntax)"_s, qPrintable(view.cell()));
}

void draggingFromProseOverATableSelectsItWhole()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    const int before = int(view.text().indexOf(u"table"));
    // A selection from the text above that reaches into a cell takes the table to its end.
    view.select(before, int(view.text().indexOf(u"North")) + 2);
    QCOMPARE(view.anchor(), before);
    QCOMPARE(view.cursor(), grid->end());
    // From below, to its start.
    const int after = int(view.text().indexOf(u"After")) + 3;
    view.select(after, int(view.text().indexOf(u"East")));
    QCOMPARE(view.anchor(), after);
    QCOMPARE(view.cursor(), grid->start());
}

void clicksBelowATableLandInProseAfterItMoves()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Lines written above the table move it down.
    view.select(0, 0);
    const qreal top = view.grid()->y();
    view.type(u"One more line."_s);
    view.key(Qt::Key_Return);
    view.key(Qt::Key_Return);
    view.type(u"And another."_s);
    view.key(Qt::Key_Return);
    view.key(Qt::Key_Return);
    QVERIFY2(view.text().startsWith(u"One more line.\n\nAnd another.\n\nBefore"_s), qPrintable(view.text()));
    QTRY_VERIFY(view.grid()->y() >= top + 4 * 20);
    const int after = int(view.text().indexOf(u"After")) + 2;
    QRectF line;
    QMetaObject::invokeMethod(view.editor, "positionToRectangle", Q_RETURN_ARG(QRectF, line),
                              Q_ARG(int, view.binding->sourceToEditor(after)));
    const QPointF point = view.editor->mapToScene(line.center());
    QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier, point.toPoint());
    QTRY_COMPARE(view.cursor(), after);
    // A click in a cell puts the caret in its text.
    TableGrid *grid = view.grid();
    const int east = int(view.text().indexOf(u"East"));
    const QRectF cell = *view.binding->tables()->rectangleFor(east);
    QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier,
                      view.editor->mapToScene(cell.center() + QPointF(2, 0)).toPoint());
    QTRY_COMPARE(view.cell(), u"East"_s);
    QVERIFY(view.cursor() >= east && view.cursor() <= east + 4);
    QVERIFY(grid->hasCaret());
}

void tableItemsPassTheWindowChecks()
{

    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    QQuickItem *gridItem = findItem(view.window.contentItem(), u"tableGrid"_s);
    QVERIFY(gridItem);
    QVERIFY(gridItem->isVisible());
    QCOMPARE(gridItem->height(), view.grid()->height());
    // The grid draws itself in the theme's colours, which the QML gives it.
    QCOMPARE(gridItem->property("backgroundColor").value<QColor>(), view.theme->property("background").value<QColor>());
    QStringList unnamed = unnamedAccessibleItems(&view.window);
    QVERIFY2(unnamed.isEmpty(), qPrintable(unnamed.join(u", ")));
    // Turned off, the table shows as its Markdown again, every line of it.
    view.pane->setProperty("tableGrids", false);
    QVERIFY(view.grids(0));
    QTextDocument *doc = view.binding->textDocument();
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) QVERIFY(block.isVisible());
}

void editsInsideACellBecomeCellEdits()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Bold on a word in a cell: the markers go round it, the table tidy.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east, east + 4);
    QVERIFY(view.binding->format(u"bold"_s));
    QVERIFY2(view.text().contains(u"| 10:00 | **East** | 12  |"_s), qPrintable(view.text()));
    QVERIFY2(view.text().contains(u"| Time  | Shelter  | Air |"_s), qPrintable(view.text()));
    QCOMPARE(view.text().mid(view.anchor(), view.cursor() - view.anchor()), u"East"_s);
    // One step back to the table as it was.
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    // Several edits in several cells of one table, as one step, the selection placed in the text it ends in.
    const QString text = view.text();
    const int north = int(text.indexOf(u"North")), gate = int(text.indexOf(u"12"));
    const auto adapted = view.binding->tables()->adapt(
        QList<SourceEdit>{{{north, 5}, u"N"_s}, {{gate, 2}, u"twelve"_s}}, north + 1, gate - 4 + 6);
    QCOMPARE(adapted.kind, TableGrids::Adapted::Changed);
    QCOMPARE(adapted.edits.size(), 1);
    QString after = text;
    after.replace(adapted.edits[0].range.start, adapted.edits[0].range.length, adapted.edits[0].replacement);
    QVERIFY2(after.contains(u"| 06:00 | N       | 7.8    |\n| 10:00 | East    | twelve |"_s), qPrintable(after));
    QCOMPARE(after.mid(adapted.anchor - 1, 1), u"N"_s);
    QCOMPARE(after.mid(adapted.cursor - 6, 6), u"twelve"_s);
}

void editsAcrossTableSyntaxAreRefusedWithAReason()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    // A heading's marks would go before the row's pipe: refused, nothing written.
    const int north = int(view.text().indexOf(u"North"));
    view.select(north, north);
    QVERIFY(!view.binding->format(u"heading1"_s));
    QCOMPARE(view.text(), weather);
    QCOMPARE(refused.count(), 1);
    QVERIFY(!refused.first().first().toString().isEmpty());
    // An edit from one cell into the next is refused too.
    const auto across = view.binding->tables()->adapt({north, 9}, u"x"_s);
    QCOMPARE(across.kind, TableGrids::Adapted::Refused);
    // So is joining the table's last line with the text after it, from either side.
    const int end = view.grid()->end();
    QCOMPARE(view.binding->tables()->adapt({end, 1}, QString()).kind, TableGrids::Adapted::Refused);
    QCOMPARE(view.binding->tables()->adapt({view.grid()->start() - 1, 1}, QString()).kind,
             TableGrids::Adapted::Refused);
}

void wholeTableEditsPassThrough()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    // A selection from the text above across the whole table deletes it all.
    const int before = int(view.text().indexOf(u"table."));
    view.select(before, int(view.text().indexOf(u"North")));
    QCOMPARE(view.cursor(), grid->end());
    view.key(Qt::Key_Delete);
    QCOMPARE(view.text(), u"Before the \n\nAfter the table."_s);
    QVERIFY(view.grids(0));
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    QVERIFY(view.grids(1));
}

void everyWayOfEditingKeepsTheTableTidy()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const auto tidy = [&] {
        const auto table = MdTable::locate(view.text(), view.grid()->start());
        if (!table) return false;
        return MdTable::write(MdTable::model(view.text(), *table)).text
               == view.text().mid(table->start, table->end - table->start);
    };
    // Paste: lines become spaces in the cell.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east + 4, east + 4);
    QGuiApplication::clipboard()->setText(u" Gate,\nopen"_s);
    view.binding->paste();
    QCOMPARE(view.cell(), u"East Gate, open"_s);
    QVERIFY(tidy());
    // The editor's own insert and remove, as assistive technology and QML use them.
    const int open = int(view.text().indexOf(u"open"));
    view.binding->replaceEditorRange(view.binding->sourceToEditor(open), view.binding->sourceToEditor(open + 4), u"shut"_s);
    QCOMPARE(view.cell(), u"East Gate, shut"_s);
    QVERIFY(tidy());
    // An input method's commit.
    view.select(int(view.text().indexOf(u"North")) + 5, int(view.text().indexOf(u"North")) + 5);
    QInputMethodEvent commit;
    commit.setCommitString(u"ern"_s);
    QCoreApplication::sendEvent(view.editor, &commit);
    QCOMPARE(view.cell(), u"Northern"_s);
    QVERIFY(tidy());
    // Vim's x, in a cell, deletes in it.
    view.pane->setProperty("vimKeys", true);
    view.select(int(view.text().indexOf(u"Northern")), int(view.text().indexOf(u"Northern")));
    sendKey(&view.window, u'x');
    QCOMPARE(view.cell(), u"orthern"_s);
    QVERIFY(tidy());
}

void replaceAndReplaceAllKeepTablesTidy()
{
    GridEditor view(weather + u"\n\nEast is east."_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    DocumentFind find;
    find.setDocument(view.document);
    find.setBinding(view.binding);
    find.setActive(true);
    find.setCaseSensitive(true);
    find.setPattern(u"East"_s);
    QTRY_COMPARE(find.count(), 2);
    view.select(int(view.text().indexOf(u"East")), int(view.text().indexOf(u"East")) + 4);
    find.setSelection(view.anchor(), view.cursor());
    QCOMPARE(find.current(), 0);
    find.replace(u"Eastern Gate"_s);
    QVERIFY2(view.text().contains(u"| 10:00 | Eastern Gate | 12  |"_s), qPrintable(view.text()));
    QVERIFY2(view.text().contains(u"| Time  | Shelter      | Air |"_s), qPrintable(view.text()));
    view.binding->undo();
    QVERIFY(view.text().startsWith(weather));
    // Replace all: in cells and prose, each table tidy, one step.
    QSignalSpy replaced(&find, &DocumentFind::replacedAll);
    find.setPattern(u"[0-9]+:00"_s);
    find.setRegex(true);
    QTRY_COMPARE(find.count(), 2);
    find.replaceAll(u"noon"_s);
    QTRY_COMPARE(replaced.count(), 1);
    QCOMPARE(replaced.first().first().toInt(), 2);
    QVERIFY2(view.text().contains(u"| Time | Shelter | Air |\n| ---- | ------- | --- |\n| noon | North   | 7.8 |"_s),
             qPrintable(view.text()));
    view.binding->undo();
    QVERIFY(view.text().startsWith(weather));
}

void findSkipsTableSyntax()
{
    GridEditor view(weather + u"\n\nA line -- with a dash | and a pipe."_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    DocumentFind find;
    find.setDocument(view.document);
    find.setBinding(view.binding);
    find.setActive(true);
    // The pipes and dashes of a grid are not its text: only the prose's are found.
    find.setPattern(u"|"_s);
    QTRY_COMPARE(find.count(), 1);
    find.setPattern(u"--"_s);
    QTRY_COMPARE(find.count(), 1);
    // A cell's text is.
    find.setPattern(u"North"_s);
    QTRY_COMPARE(find.count(), 1);
    // A table shown as Markdown finds all of it.
    view.pane->setProperty("tableGrids", false);
    find.setPattern(u"|"_s);
    QTRY_VERIFY(find.count() > 1);
}

void aSecondViewKeepsItsCaretWhenATableIsRetidied()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    auto second = view.secondView();
    QVERIFY(second && second->binding);
    QTRY_COMPARE(second->binding->tables()->grids().size(), 1);
    const int seven = int(view.text().indexOf(u"7.8"));
    second->binding->select(seven + 2, seven + 2);
    // Writing in the first view widens a column before the second's caret.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east + 4, east + 4);
    view.type(u" Gate"_s);
    QVERIFY(view.text().contains(u"East Gate"_s));
    // The second view's caret is still after "7." in its cell.
    const int now = int(view.text().indexOf(u"7.8"));
    QCOMPARE(second->binding->cursorSource(), now + 2);
    QCOMPARE(second->binding->anchorSource(), now + 2);
    QCOMPARE(second->binding->tables()->grids().first()->end(), view.grid()->end());
}

void aRetidyChangesOnlyTheLinesItMustAndStylesRowsShownAgain()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    QTextDocument *shown = view.binding->textDocument();
    const int blocks = shown->blockCount();
    // A cursor held in a hidden row, as another part of the view holds one:
    // after "No" in North.
    const int north = int(view.text().indexOf(u"North"));
    QTextCursor held(shown);
    held.setPosition(view.binding->sourceToEditor(north + 2));
    QVERIFY(!held.block().isVisible());
    // Writing in a cell widens its column, and every row is padded anew.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east + 4, east + 4);
    view.type(u" Gate"_s);
    QVERIFY2(view.text().contains(u"| North     | 7.8 |"_s), qPrintable(view.text()));
    // The styling of the change is done before the table is shown again.
    QObject *styling = nullptr;
    for (QObject *child : view.pane->findChildren<QObject *>())
        if (child->inherits("MarkdownStyling")) styling = child;
    QVERIFY(styling);
    QTest::qWait(100);
    QTRY_VERIFY(!styling->property("busy").toBool());
    QTest::qWait(100);
    // The rows are the same paragraphs as before, each changed in place:
    // the held cursor is still after "No", and the view shows the source.
    QCOMPARE(shown->blockCount(), blocks);
    QCOMPARE(view.binding->editorToSource(held.position()), int(view.text().indexOf(u"North")) + 2);
    QCOMPARE(shown->toPlainText(), view.text());
    // Shown as Markdown again, its rows are styled as any text is.
    QVERIFY(view.binding->tables()->runCommand(u"showTableMarkdown"_s));
    const QTextBlock row = shown->findBlock(view.binding->sourceToEditor(int(view.text().indexOf(u"North"))));
    QVERIFY(row.isVisible());
    QTRY_VERIFY(std::any_of(row.layout()->formats().cbegin(), row.layout()->formats().cend(),
                            [](const QTextLayout::FormatRange &range) { return range.start == 0; }));
}

namespace {

// The cells of the table as a grid shows them, row by row, "|" between cells.
QString cells(GridEditor &view)
{
    TableGrid *grid = view.grid();
    if (!grid) return u"(no grid)"_s;
    QStringList rows;
    for (int r = 0; r < grid->rowCount(); ++r) {
        QStringList row;
        for (int i = 0; i < grid->cellCount(r); ++i) {
            const MdTable::Cell &cell = grid->cellAt({r, i});
            row.append(view.text().mid(cell.start, cell.end - cell.start));
        }
        rows.append(row.join(u'|'));
    }
    return rows.join(u" / "_s);
}

QString selected(GridEditor &view)
{
    return view.text().mid(qMin(view.anchor(), view.cursor()), qAbs(view.cursor() - view.anchor()));
}

} // namespace

void arrowsWalkThroughCellsAndOutOfTheTable()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Down from the last row leaves the table below it.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east, east);
    view.key(Qt::Key_Down);
    QCOMPARE(view.cell(), u"-"_s);
    QCOMPARE(view.cursor(), view.grid()->end() + 1);
    // Up from below enters the last row.
    view.key(Qt::Key_Up);
    QVERIFY2(view.cell() == u"10:00"_s || view.cell() == u"East"_s || view.cell() == u"12"_s, qPrintable(view.cell()));
    // A word at a time, and on into the next cell.
    const QString text = u"| A | B |\n|---|---|\n| one two | x |"_s;
    GridEditor words(text);
    QVERIFY(words.grids(1));
    const int one = int(words.text().indexOf(u"one"));
    words.select(one, one);
    words.key(Qt::Key_Right, Qt::ControlModifier);
    QCOMPARE(words.cursor(), one + 3);
    words.key(Qt::Key_Right, Qt::ControlModifier);
    QCOMPARE(words.cursor(), one + 7);
    words.key(Qt::Key_Right, Qt::ControlModifier);
    QCOMPARE(words.cell(), u"x"_s);
    words.key(Qt::Key_Left, Qt::ControlModifier);
    QCOMPARE(words.cell(), u"one two"_s);
    // A page down goes to the last row.
    words.select(int(words.text().indexOf(u"A")), int(words.text().indexOf(u"A")));
    words.key(Qt::Key_PageDown);
    QCOMPARE(words.cell(), u"one two"_s);
}

void tabAndEnterMoveAddAndLeave()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Tab in the last cell adds a row and goes to its first cell.
    const int twelve = int(view.text().indexOf(u"12"));
    view.select(twelve, twelve);
    view.key(Qt::Key_Tab);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North|7.8 / 10:00|East|12 / ||"_s);
    QCOMPARE(view.grid()->placeOf(view.cursor()), (TableGrid::Place{3, 0}));
    view.type(u"14:00"_s);
    // Enter goes down a column, adding a row from the last.
    view.key(Qt::Key_Return);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North|7.8 / 10:00|East|12 / 14:00|| / ||"_s);
    QCOMPARE(view.grid()->placeOf(view.cursor()), (TableGrid::Place{4, 0}));
    // Enter on an empty last row takes it away and goes on below the table.
    view.key(Qt::Key_Return);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North|7.8 / 10:00|East|12 / 14:00||"_s);
    QCOMPARE(view.cell(), u"-"_s);
    QCOMPARE(view.cursor(), view.grid()->end() + 1);
    // Enter above the last row selects the text of the cell below.
    const int north = int(view.text().indexOf(u"North"));
    view.select(north, north);
    view.key(Qt::Key_Return);
    QCOMPARE(selected(view), u"East"_s);
    // Shift+Enter breaks the line in a cell, as HTML has it.
    view.select(north + 5, north + 5);
    view.key(Qt::Key_Return, Qt::ShiftModifier);
    view.type(u"side"_s);
    QCOMPARE(view.cell(), u"North<br>side"_s);
    // Shift+Tab at the first cell stays.
    const int time = int(view.text().indexOf(u"Time"));
    view.select(time, time);
    const QString before = view.text();
    view.key(Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(view.text(), before);
    QCOMPARE(view.cell(), u"Time"_s);
}

void selectionsAreACellABlockOrTheWholeTable()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Shift+Right past a cell's text makes a block of cells.
    const int north = int(view.text().indexOf(u"North"));
    view.select(north, north + 5);
    view.key(Qt::Key_Right, Qt::ShiftModifier);
    view.key(Qt::Key_Down, Qt::ShiftModifier);
    int r0 = 0, r1 = 0, c0 = 0, c1 = 0;
    QVERIFY(view.binding->tables()->block(view.grid(), &r0, &r1, &c0, &c1));
    QCOMPARE(QList<int>({r0, r1, c0, c1}), QList<int>({1, 2, 1, 2}));
    const QList<TableGrid::Drawn> drawn = view.grid()->drawn(0, view.grid()->height());
    int lit = 0;
    for (const TableGrid::Drawn &cell : drawn) lit += cell.selected;
    QCOMPARE(lit, 4);
    // Delete empties the block's cells.
    view.key(Qt::Key_Delete);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|| / 10:00||"_s);
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    // Typing over a block empties it and writes in its first cell.
    view.select(north, int(view.text().indexOf(u"12")) + 1);
    view.type(u"x"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|x| / 10:00||"_s);
    view.binding->undo();
    // Ctrl+A: the cell's text, then the table, then everything.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east + 1, east + 1);
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(selected(view), u"East"_s);
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(view.anchor(), view.grid()->start());
    QCOMPARE(view.cursor(), view.grid()->end());
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(selected(view), view.text());
    // Escape collapses a block.
    view.select(north, int(view.text().indexOf(u"7.8")));
    view.key(Qt::Key_Escape);
    QCOMPARE(view.anchor(), view.cursor());
}

void theLineAfterATableStaysBlank()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    // Writing on the blank line after the table writes a line lower.
    const int blank = grid->end() + 1;
    view.select(blank, blank);
    view.type(u"Note"_s);
    QVERIFY2(view.text().contains(u"| 12  |\n\nNote\nAfter"_s), qPrintable(view.text()));
    QCOMPARE(view.binding->tables()->grids().size(), 1);
    // Nothing pulls the text below up against the table.
    view.binding->undo();
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    const int after = int(view.text().indexOf(u"After"));
    view.select(after, after);
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    view.key(Qt::Key_Backspace);
    QCOMPARE(view.text(), weather);
    QCOMPARE(refused.count(), 1);
}

void tablesAtTheEndsOfTheTextHaveACaretBesideThem()
{
    GridEditor view(u"| A | B |\n|---|---|\n| 1 | 2 |"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Up from the first row of a table that starts the text makes a line to write on.
    view.select(2, 2);
    QCOMPARE(view.cell(), u"A"_s);
    view.key(Qt::Key_Up);
    QCOMPARE(view.cursor(), 0);
    QCOMPARE(view.cell(), u"-"_s);
    view.type(u"Title"_s);
    QVERIFY2(view.text().startsWith(u"Title\n| A | B |"_s), qPrintable(view.text()));
    // Down from the last row of a table that ends it makes a line below, after a blank one.
    const int two = int(view.text().indexOf(u"2 |"));
    view.select(two, two);
    view.key(Qt::Key_Down);
    QCOMPARE(view.cell(), u"-"_s);
    QCOMPARE(view.cursor(), int(view.text().size()));
    view.type(u"End"_s);
    QVERIFY2(view.text().endsWith(u"| 1 | 2 |\n\nEnd"_s), qPrintable(view.text()));
}

void backspaceAfterATableSelectsThenDeletesIt()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int blank = view.grid()->end() + 1;
    view.select(blank, blank);
    view.key(Qt::Key_Backspace);
    QCOMPARE(view.anchor(), view.grid()->start());
    QCOMPARE(view.cursor(), view.grid()->end() + 1);
    view.key(Qt::Key_Backspace);
    QCOMPARE(view.text(), u"Before the table.\n\n\nAfter the table."_s);
    QVERIFY(view.grids(0));
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    // Delete at the end of the line before a table selects it too.
    QVERIFY(view.grids(1));
    view.select(view.grid()->start() - 1, view.grid()->start() - 1);
    view.key(Qt::Key_Delete);
    QCOMPARE(view.anchor(), view.grid()->start());
}

void vimMotionsFlowAcrossCells()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.pane->setProperty("vimKeys", true);
    QVERIFY(view.grids(1));
    const int north = int(view.text().indexOf(u"North"));
    view.select(north + 4, north + 4);
    // l past a cell's last character, and on into the next cell.
    sendKey(&view.window, u'l');
    sendKey(&view.window, u'l');
    QCOMPARE(view.cell(), u"7.8"_s);
    // h back, out of the cell and into the one before.
    sendKey(&view.window, u'h');
    QCOMPARE(view.cell(), u"North"_s);
    // 0 and $ go to the first and last cell of the row.
    sendKey(&view.window, u'0');
    QCOMPARE(view.cell(), u"06:00"_s);
    sendKey(&view.window, u'$');
    QCOMPARE(view.cell(), u"7.8"_s);
    // w goes word by word, cell by cell.
    sendKey(&view.window, u'0');
    sendKey(&view.window, u'w');
    QVERIFY2(view.cell() != u"(syntax)"_s && view.cell() != u"-"_s, qPrintable(view.cell()));
}

void vimOperatorsActOnCellsAndRows()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.pane->setProperty("vimKeys", true);
    QVERIFY(view.grids(1));
    const int north = int(view.text().indexOf(u"North"));
    view.select(north, north);
    // dd takes the row away, and u brings it back.
    typeText(&view.window, u"dd"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 10:00|East|12"_s);
    sendKey(&view.window, u'u');
    QCOMPARE(view.text(), weather);
    // yy and p put a copy of the row below it.
    view.select(north, north);
    typeText(&view.window, u"yyp"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North|7.8 / 06:00|North|7.8 / 10:00|East|12"_s);
    sendKey(&view.window, u'u');
    QCOMPARE(view.text(), weather);
    // o opens an empty row below and writes in it; Escape keeps what was written.
    view.select(north, north);
    sendKey(&view.window, u'o');
    typeText(&view.window, u"08:00"_s);
    view.key(Qt::Key_Escape);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North|7.8 / 08:00|| / 10:00|East|12"_s);
    sendKey(&view.window, u'u');
    QCOMPARE(view.text(), weather);
    // cw changes a word in a cell; D deletes to the end of the cell, no further.
    view.select(north, north);
    typeText(&view.window, u"cwSouth"_s);
    view.key(Qt::Key_Escape);
    QCOMPARE(view.cell(), u"South"_s);
    view.select(int(view.text().indexOf(u"South")) + 2, int(view.text().indexOf(u"South")) + 2);
    sendKey(&view.window, u'D');
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|So|7.8 / 10:00|East|12"_s);
    // x takes a character and . takes another; r replaces one.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east, east);
    sendKey(&view.window, u'x');
    QCOMPARE(view.cell(), u"ast"_s);
    sendKey(&view.window, u'.');
    QCOMPARE(view.cell(), u"st"_s);
    typeText(&view.window, u"rS"_s);
    QCOMPARE(view.cell(), u"St"_s);
    // x empties the cell and stops there, the table whole.
    typeText(&view.window, u"xx"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|So|7.8 / 10:00||12"_s);
    const QString emptied = view.text();
    sendKey(&view.window, u'x');
    QCOMPARE(view.text(), emptied);
    // J would join the row to the next: nothing changes.
    const QString now = view.text();
    sendKey(&view.window, u'J');
    QCOMPARE(view.text(), now);
}

void everyTableCommandRunsFromMenuPaletteAndHandle()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrids *tables = view.binding->tables();
    const auto at = [&](const QString &text) {
        const int position = int(view.text().indexOf(text));
        view.select(position, position);
    };
    const auto runs = [&](const QString &command) {
        QVERIFY2(tables->can(command), qPrintable(command));
        QVERIFY2(tables->runCommand(command), qPrintable(command));
    };
    // Every command is one the registry and Format → Table know.
    QCOMPARE(TableGrids::commands().size(), 22);
    at(u"North"_s);
    runs(u"insertTableRowBelow"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North|7.8 / || / 10:00|East|12"_s);
    QCOMPARE(view.grid()->placeOf(view.cursor()), (TableGrid::Place{2, 1}));
    view.binding->undo();
    at(u"North"_s);
    runs(u"insertTableRowAbove"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / || / 06:00|North|7.8 / 10:00|East|12"_s);
    view.binding->undo();
    at(u"North"_s);
    runs(u"duplicateTableRow"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North|7.8 / 06:00|North|7.8 / 10:00|East|12"_s);
    view.binding->undo();
    at(u"North"_s);
    runs(u"moveTableRowDown"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 10:00|East|12 / 06:00|North|7.8"_s);
    QCOMPARE(view.cell(), u"North"_s);
    runs(u"moveTableRowUp"_s);
    QCOMPARE(view.text(), weather);
    QVERIFY(!tables->can(u"moveTableRowUp"_s));
    runs(u"deleteTableRow"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 10:00|East|12"_s);
    view.binding->undo();
    at(u"North"_s);
    runs(u"insertTableColumnLeft"_s);
    QCOMPARE(cells(view), u"Time||Shelter|Air / 06:00||North|7.8 / 10:00||East|12"_s);
    view.binding->undo();
    at(u"North"_s);
    runs(u"moveTableColumnRight"_s);
    QCOMPARE(cells(view), u"Time|Air|Shelter / 06:00|7.8|North / 10:00|12|East"_s);
    QCOMPARE(view.cell(), u"North"_s);
    runs(u"moveTableColumnLeft"_s);
    QCOMPARE(view.text(), weather);
    runs(u"alignTableColumnRight"_s);
    QVERIFY2(view.text().contains(u"| ----- | ------: | --- |"_s), qPrintable(view.text()));
    runs(u"alignTableColumnDefault"_s);
    QCOMPARE(view.text(), weather);
    at(u"7.8"_s);
    runs(u"sortTableDescending"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 10:00|East|12 / 06:00|North|7.8"_s);
    view.binding->undo();
    at(u"North"_s);
    runs(u"deleteTableColumn"_s);
    QCOMPARE(cells(view), u"Time|Air / 06:00|7.8 / 10:00|12"_s);
    view.binding->undo();
    // Merge and split, in the Writer dialect.
    at(u"North"_s);
    runs(u"mergeTableCells"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North 7.8 / 10:00|East|12"_s);
    QVERIFY(view.text().contains(u"||"_s));
    runs(u"splitTableCell"_s);
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North 7.8| / 10:00|East|12"_s);
    while (view.document->buffer()->canUndo()) view.binding->undo();
    QCOMPARE(view.text(), weather);
    // The table goes whole, its line with it.
    QVERIFY(view.grids(1));
    at(u"North"_s);
    runs(u"deleteTable"_s);
    QCOMPARE(view.text(), u"Before the table.\n\n\nAfter the table."_s);
    view.binding->undo();
    // Outside a table, none of them runs.
    QVERIFY(view.grids(1));
    at(u"After"_s);
    for (const QString &command : TableGrids::commands()) QVERIFY2(!tables->can(command), qPrintable(command));
    // From a row's handle: the row selected whole, its menu offered.
    at(u"North"_s);
    tables->selectRow(view.grid(), 1, false);
    int r0 = 0, r1 = 0, c0 = 0, c1 = 0;
    QVERIFY(tables->block(view.grid(), &r0, &r1, &c0, &c1));
    QCOMPARE(QList<int>({r0, r1, c0, c1}), QList<int>({1, 1, 0, 2}));
    tables->selectColumn(view.grid(), 2, true);
    QVERIFY(tables->block(view.grid(), &r0, &r1, &c0, &c1));
    QCOMPARE(QList<int>({r0, r1, c0, c1}), QList<int>({0, 2, 0, 2}));
    QQuickItem *rowHandle = findItem(view.window.contentItem(), u"rowHandle"_s);
    QVERIFY(rowHandle && rowHandle->isVisible());
}

void handlesStayPutAsThePointerReachesThem()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // The caret in the text before the table: only the pointer shows handles.
    view.select(0, 0);
    TableGrid *grid = view.grid();
    QQuickItem *drawn = findItem(view.window.contentItem(), u"tableGrid"_s);
    QQuickItem *columnHandle = findItem(view.window.contentItem(), u"columnHandle"_s);
    QQuickItem *addColumn = findItem(view.window.contentItem(), u"addColumnButton"_s);
    QVERIFY(drawn && columnHandle && addColumn);
    QVERIFY(!columnHandle->isVisible());
    const auto glide = [&](QPointF from, QPointF to) {
        for (int step = 1; step <= 8; ++step)
            QTest::mouseMove(&view.window, (from + (to - from) * step / 8.0).toPoint());
    };
    // Over the second column's first body cell, then up onto its handle.
    const QRectF shelter = grid->columnRect(1);
    const QPointF cell = drawn->mapToScene(QPointF(shelter.center().x(), grid->rowRect(1).center().y()));
    QTest::mouseMove(&view.window, cell.toPoint());
    QTRY_VERIFY(columnHandle->isVisible());
    const qreal above = columnHandle->x();
    const QPointF handle = columnHandle->mapToScene(QPointF(columnHandle->width() / 2, columnHandle->height() / 2));
    glide(cell, handle);
    QTest::qWait(50);
    QVERIFY(columnHandle->isVisible());
    QCOMPARE(columnHandle->x(), above);
    // A click there selects the column, and its menu opens.
    QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier, handle.toPoint());
    int r0 = 0, r1 = 0, c0 = 0, c1 = 0;
    QTRY_VERIFY(view.binding->tables()->block(grid, &r0, &r1, &c0, &c1));
    QCOMPARE(QList<int>({r0, r1, c0, c1}), QList<int>({0, 2, 1, 1}));
    QTRY_VERIFY(findItem(view.window.contentItem(), u"insertColumnLeftEntry"_s)
                && findItem(view.window.contentItem(), u"insertColumnLeftEntry"_s)->isVisible());
    view.key(Qt::Key_Escape);
    view.select(0, 0);
    // The + on the right edge stays while the pointer goes onto it.
    const QPointF right = drawn->mapToScene(QPointF(grid->columnRect(2).center().x(), grid->rowRect(1).center().y()));
    QTest::mouseMove(&view.window, right.toPoint());
    QTRY_VERIFY(addColumn->isVisible());
    const QPointF plus = addColumn->mapToScene(QPointF(addColumn->width() / 2, addColumn->height() / 2));
    glide(right, plus);
    QTest::qWait(50);
    QVERIFY(addColumn->isVisible());
    QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier, plus.toPoint());
    QTRY_COMPARE(grid->columnCount(), 4);
}

void handlesMoveRowsAndColumnsByDragging()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int east = int(view.text().indexOf(u"East"));
    view.select(east, east);
    QQuickItem *handle = findItem(view.window.contentItem(), u"rowHandle"_s);
    QTRY_VERIFY(handle && handle->isVisible());
    // Drag the last row's handle above the first row.
    const QPointF from = handle->mapToScene(QPointF(handle->width() / 2, handle->height() / 2));
    TableGrid *grid = view.grid();
    QQuickItem *slot = handle->parentItem();
    const QRectF first = grid->rowRect(1);
    const QPointF to = slot->mapToScene(QPointF(-handle->width() / 2, first.y() + 2));
    QTest::mousePress(&view.window, Qt::LeftButton, Qt::NoModifier, from.toPoint());
    for (int step = 1; step <= 6; ++step)
        QTest::mouseMove(&view.window, (from + (to - from) * step / 6.0).toPoint());
    QQuickItem *line = findItem(view.window.contentItem(), u"rowDropLine"_s);
    QVERIFY(line && line->isVisible());
    QTest::mouseRelease(&view.window, Qt::LeftButton, Qt::NoModifier, to.toPoint());
    QCOMPARE(cells(view), u"Time|Shelter|Air / 10:00|East|12 / 06:00|North|7.8"_s);
    QVERIFY(!line->isVisible());
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    // A column's handle, dragged to the right edge, moves it last.
    const int north = int(view.text().indexOf(u"North"));
    view.select(north, north);
    QQuickItem *columnHandle = findItem(view.window.contentItem(), u"columnHandle"_s);
    QTRY_VERIFY(columnHandle && columnHandle->isVisible());
    const QPointF grab = columnHandle->mapToScene(QPointF(columnHandle->width() / 2, columnHandle->height() / 2));
    const QPointF drop = slot->mapToScene(QPointF(view.grid()->width() - 3, -columnHandle->height()));
    QTest::mousePress(&view.window, Qt::LeftButton, Qt::NoModifier, grab.toPoint());
    for (int step = 1; step <= 6; ++step)
        QTest::mouseMove(&view.window, (grab + (drop - grab) * step / 6.0).toPoint());
    QTest::mouseRelease(&view.window, Qt::LeftButton, Qt::NoModifier, drop.toPoint());
    QCOMPARE(cells(view), u"Time|Air|Shelter / 06:00|7.8|North / 10:00|12|East"_s);
    // The plus buttons add a column and a row.
    view.binding->undo();
    view.select(north, north);
    QQuickItem *addColumn = findItem(view.window.contentItem(), u"addColumnButton"_s);
    QTRY_VERIFY(addColumn && addColumn->isVisible());
    QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier, centreOf(addColumn));
    QCOMPARE(cells(view), u"Time|Shelter|Air| / 06:00|North|7.8| / 10:00|East|12|"_s);
    QQuickItem *addRow = findItem(view.window.contentItem(), u"addRowButton"_s);
    QTRY_VERIFY(addRow && addRow->isVisible());
    QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier, centreOf(addRow));
    QCOMPARE(cells(view), u"Time|Shelter|Air| / 06:00|North|7.8| / 10:00|East|12| / |||"_s);
}

void theSizePickerInsertsATidyEmptyTable()
{
    GridEditor view(u"A paragraph.\n\nAnother one."_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.select(5, 5);
    QMetaObject::invokeMethod(view.pane, "chooseTable");
    QObject *picker = view.pane->findChild<QObject *>(u"tablePicker"_s);
    QVERIFY(picker);
    QTRY_VERIFY(picker->property("opened").toBool());
    QCOMPARE(picker->property("columns").toInt(), 3);
    QCOMPARE(picker->property("rows").toInt(), 3);
    // The arrows grow it to four columns and five rows; Enter puts it in.
    view.key(Qt::Key_Right);
    view.key(Qt::Key_Down);
    view.key(Qt::Key_Down);
    QCOMPARE(picker->property("columns").toInt(), 4);
    QCOMPARE(picker->property("rows").toInt(), 5);
    view.key(Qt::Key_Return);
    QTRY_VERIFY(!picker->property("visible").toBool());
    QCOMPARE(view.text(), u"A paragraph.\n\n|     |     |     |     |\n| --- | --- | --- | --- |\n|     |     |     |     |\n"
                          "|     |     |     |     |\n|     |     |     |     |\n|     |     |     |     |\n\nAnother one."_s);
    // A grid at once, the caret in its first cell, ready to write the header.
    QCOMPARE(view.binding->tables()->grids().size(), 1);
    QCOMPARE(view.grid()->placeOf(view.cursor()), (TableGrid::Place{0, 0}));
    view.type(u"Time"_s);
    QCOMPARE(view.cell(), u"Time"_s);
    view.key(Qt::Key_Tab);
    view.type(u"Shelter"_s);
    QVERIFY2(view.text().contains(u"| Time | Shelter |     |     |"_s), qPrintable(view.text()));
    // Escape leaves the picker without a table; and there is none to put in a cell.
    QMetaObject::invokeMethod(view.pane, "chooseTable");
    QTRY_VERIFY(picker->property("opened").toBool());
    const QString before = view.text();
    view.key(Qt::Key_Escape);
    QTRY_VERIFY(!picker->property("visible").toBool());
    QCOMPARE(view.text(), before);
}

void tablesCanBeShownAsMarkdownPerView()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    auto second = view.secondView();
    QVERIFY(second && second->binding);
    QTRY_COMPARE(second->binding->tables()->grids().size(), 1);
    const int north = int(view.text().indexOf(u"North"));
    view.select(north + 2, north + 2);
    QVERIFY(view.binding->tables()->runCommand(u"showTableMarkdown"_s));
    // This view shows the table's Markdown, every line of it; the other its grid.
    QCOMPARE(view.binding->tables()->grids().size(), 0);
    QCOMPARE(view.text(), weather);
    QCOMPARE(view.cursor(), north + 2);
    for (QTextBlock block = view.binding->textDocument()->begin(); block.isValid(); block = block.next())
        QVERIFY(block.isVisible());
    QCOMPARE(second->binding->tables()->grids().size(), 1);
    // A parse of the text later keeps it so, and an edit in it is the writer's own.
    view.type(u"x"_s);
    QTest::qWait(300);
    QCOMPARE(view.binding->tables()->grids().size(), 0);
    QVERIFY2(view.text().contains(u"| Noxrth   |"_s), qPrintable(view.text()));
    QVERIFY(view.binding->tables()->can(u"showTableGrid"_s));
    QVERIFY(view.binding->tables()->runCommand(u"showTableGrid"_s));
    QCOMPARE(view.binding->tables()->grids().size(), 1);
}

void readOnlyTablesTakeNoEdits()
{
    GridEditor view{QString()};
    QVERIFY2(view.ok(), qPrintable(view.error));
    writeFile(view.root.filePath(u"bytes.md"_s), QByteArray("caf\xe9\n\n| a | b |\n|---|---|\n| 1 | 2 |\n"));
    Document *readOnly = view.store->open(view.root.filePath(u"bytes.md"_s));
    QCOMPARE(readOnly->state(), Document::State::ReadOnly);
    view.pane->setProperty("document", QVariant::fromValue(readOnly));
    QTRY_VERIFY(view.binding->document() == readOnly);
    QTRY_COMPARE(view.binding->tables()->grids().size(), 1);
    const QString before = readOnly->buffer()->text();
    const int one = int(before.indexOf(u"1 |"));
    view.binding->select(one, one);
    view.type(u"x"_s);
    view.key(Qt::Key_Tab);
    QCOMPARE(readOnly->buffer()->text(), before);
    for (const QString &command : TableGrids::commands())
        if (command != u"showTableMarkdown"_s) QVERIFY2(!view.binding->tables()->can(command), qPrintable(command));
    // The handles and buttons that would change it are not there.
    QQuickItem *handle = findItem(view.window.contentItem(), u"rowHandle"_s);
    QVERIFY(!handle || !handle->isVisible());
}

void copyingCellsGivesTabsAndAnHtmlTable()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // A block from North to 12: two rows, two columns.
    view.select(int(view.text().indexOf(u"North")), int(view.text().indexOf(u"12")) + 1);
    view.binding->copy();
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    QVERIFY(mime);
    QCOMPARE(mime->text(), u"North\t7.8\nEast\t12\n"_s);
    QVERIFY2(mime->html().contains(u"<tr><td>North</td><td>7.8</td></tr>"_s), qPrintable(mime->html()));
    // Cut copies, then empties the cells; one step back brings them.
    view.binding->cut();
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|| / 10:00||"_s);
    QCOMPARE(QGuiApplication::clipboard()->text(), u"North\t7.8\nEast\t12\n"_s);
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    // A selection in one cell copies its text as text does.
    const int north = int(view.text().indexOf(u"North"));
    view.select(north, north + 3);
    view.binding->copy();
    QCOMPARE(QGuiApplication::clipboard()->text(), u"Nor"_s);
}

void pastingCellsFillsOverwritesAndGrows()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Two rows of three from a spreadsheet, at East: they overwrite and grow the table.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east, east);
    auto *sheet = new QMimeData;
    sheet->setText(u"South\t17.1\t55\nWest | gate\t13.4\t70\n"_s);
    sheet->setHtml(u"<table><tr><td>ignored</td></tr></table>"_s);
    QGuiApplication::clipboard()->setMimeData(sheet);
    view.binding->paste();
    QCOMPARE(cells(view), u"Time|Shelter|Air| / 06:00|North|7.8| / 10:00|South|17.1|55 / |West \\| gate|13.4|70"_s);
    // One step back.
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    // An HTML table alone, as a web page copies one.
    auto *page = new QMimeData;
    page->setHtml(u"<meta charset='utf-8'><table><tr><td>A&amp;B</td><td><b>bold</b></td></tr></table>"_s);
    QGuiApplication::clipboard()->setMimeData(page);
    view.select(east, east);
    view.binding->paste();
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|North|7.8 / 10:00|A&B|bold"_s);
    view.binding->undo();
    // One value over a block fills every cell of it.
    view.select(int(view.text().indexOf(u"North")), int(view.text().indexOf(u"12")) + 1);
    auto *one = new QMimeData;
    one->setText(u"n/a\n"_s);
    one->setHtml(u"<table><tr><td>n/a</td></tr></table>"_s);
    QGuiApplication::clipboard()->setMimeData(one);
    view.binding->paste();
    QCOMPARE(cells(view), u"Time|Shelter|Air / 06:00|n/a|n/a / 10:00|n/a|n/a"_s);
    view.binding->undo();
    // Plain text with no tabs goes into the cell as text, its lines as spaces.
    view.select(east + 4, east + 4);
    QGuiApplication::clipboard()->setText(u" gate\nopen"_s);
    view.binding->paste();
    QCOMPARE(view.cell(), u"East gate open"_s);
}

void wideTablesScrollSidewaysAndTheTextScrollsOver()
{
    // A table wider than a narrow window, its words too long to wrap
    // within it, and text after it to scroll to.
    QString text = u"| Shelter | Thermometer | Hygrometer | Anemometer | Barometer | Pluviometer |\n"
                   "| --- | --- | --- | --- | --- | --- |\n"_s;
    for (int i = 0; i < 4; ++i)
        text += u"| Northernmost | Calibrated%1 | Recalibrated | Uncalibrated | Waterproofed | Overflowing |\n"_s.arg(i);
    for (int i = 0; i < 60; ++i) text += u"\nA paragraph after the table."_s;
    GridEditor view(text, QSize(360, 300));
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    QTRY_VERIFY(grid->width() > grid->viewWidth());
    auto *flickable = view.pane->findChild<QQuickItem *>(u"flickable"_s);
    QVERIFY(flickable);
    QQuickItem *drawn = findItem(view.window.contentItem(), u"tableGrid"_s);
    QVERIFY(drawn);
    // The table at the top of the view.
    view.select(0, 0);
    flickable->setProperty("contentY", 0);
    QTRY_VERIFY(drawn->mapToScene(QPointF(0, 0)).y() >= 0);
    const QPointF at = drawn->mapToScene(QPointF(drawn->width() / 2, drawn->height() / 2));
    // A turn of the wheel, which reports a few pixels too, as Wayland's does.
    const auto wheel = [&](QPoint pixels, QPoint angle, Qt::KeyboardModifiers modifiers) {
        QWheelEvent event(at, view.window.mapToGlobal(at), pixels, angle, Qt::NoButton, modifiers,
                          Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(&view.window, &event);
    };
    // A swipe on the touchpad: the fingers down, moves, the fingers up.
    quint64 time = 1000;
    const auto send = [&](Qt::ScrollPhase phase, QPoint pixels, Qt::MouseEventSource source) {
        QWheelEvent event(at, view.window.mapToGlobal(at), pixels, pixels * 12, Qt::NoButton, Qt::NoModifier, phase,
                          false, source);
        event.setTimestamp(time += 10);
        QCoreApplication::sendEvent(&view.window, &event);
    };
    const auto swipe = [&](QPoint pixels, int moves) {
        send(Qt::ScrollBegin, {}, Qt::MouseEventNotSynthesized);
        for (int i = 0; i < moves; ++i) send(Qt::ScrollUpdate, pixels, Qt::MouseEventSynthesizedBySystem);
        send(Qt::ScrollEnd, {}, Qt::MouseEventSynthesizedBySystem);
    };
    // Sideways on the touchpad moves the table as far as the fingers go,
    // and leaves the text.
    swipe({-10, 0}, 3);
    QCOMPARE(grid->scrollX(), 30.0);
    QCOMPARE(flickable->property("contentY").toReal(), 0.0);
    // Shift and a click of the wheel move it as far as the text scrolls
    // for one, whatever few pixels come with the click.
    const qreal click = QGuiApplication::styleHints()->wheelScrollLines() * view.binding->tables()->lineHeight();
    wheel({0, -15}, {0, -120}, Qt::ShiftModifier);
    QCOMPARE(grid->scrollX(), 30.0 + click);
    wheel({0, 15}, {0, 120}, Qt::ShiftModifier);
    QCOMPARE(grid->scrollX(), 30.0);
    // A swipe mostly downward, even a little sideways, scrolls the text and
    // leaves the table.
    const qreal across = grid->scrollX();
    swipe({-2, -20}, 5);
    QCOMPARE(grid->scrollX(), across);
    QTRY_VERIFY(flickable->property("contentY").toReal() > 0);
}

void handTypedTablesWaitUntilTheCaretLeaves()
{
    GridEditor view(u"Readings.\n\n\n\nAfter."_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(0));
    view.select(11, 11);
    view.type(u"| a | b |"_s);
    view.key(Qt::Key_Return);
    view.type(u"|---|---|"_s);
    view.key(Qt::Key_Return);
    view.type(u"| c | d |"_s);
    // While the caret is in it, the table being written stays as it is typed.
    QTest::qWait(300);
    QCOMPARE(view.binding->tables()->grids().size(), 0);
    QVERIFY(view.text().contains(u"| a | b |\n|---|---|\n| c | d |"_s));
    // Once the caret leaves it, it is a grid; its text is not touched.
    view.select(int(view.text().indexOf(u"After")), int(view.text().indexOf(u"After")));
    QVERIFY(view.grids(1));
    QVERIFY(view.text().contains(u"| a | b |\n|---|---|\n| c | d |"_s));
}

void everyGeometryQuestionHasAnAnswerInATable()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    // Every position of the table, pipes and hidden rows too, is somewhere in its grid.
    for (int position = grid->start(); position <= grid->end(); ++position) {
        const QRectF rect = view.binding->sourceRectangle(position);
        QVERIFY2(rect.top() >= grid->y() - 0.5 && rect.bottom() <= grid->y() + grid->height() + 0.5,
                 qPrintable(QString::number(position)));
    }
    // Later rows are lower down, so that a view scrolled through the table moves steadily.
    const qreal first = view.binding->sourceRectangle(int(view.text().indexOf(u"North"))).top();
    const qreal second = view.binding->sourceRectangle(int(view.text().indexOf(u"East"))).top();
    QVERIFY(second > first);
    QVERIFY(view.binding->sourceRectangle(grid->end()).top() >= second);
    // The text after the table is below it.
    QVERIFY(view.binding->sourceRectangle(int(view.text().indexOf(u"After"))).top() >= grid->y() + grid->height());
}

void fontAndWidthChangesKeepTheGrid()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const qreal before = view.grid()->height();
    // A larger font makes a taller grid, and the line it stands in grows with it.
    view.pane->setProperty("fontPixelSize", 26);
    QTRY_VERIFY(view.grid()->height() > before);
    QTextDocument *doc = view.binding->textDocument();
    const QTextBlock header = doc->findBlock(view.binding->sourceToEditor(view.grid()->start()));
    QTRY_COMPARE(header.blockFormat().lineHeight(), std::ceil(view.grid()->height()));
    // A narrow view wraps the long cells rather than drawing past it.
    GridEditor narrow(u"| A long heading that takes room | B |\n|---|---|\n| some words in a cell that needs room | x |"_s,
                      QSize(320, 480));
    QVERIFY(narrow.grids(1));
    QVERIFY2(narrow.grid()->width() <= narrow.binding->tables()->availableWidth() + 1,
             qPrintable(QString::number(narrow.grid()->width())));
    QVERIFY(narrow.grid()->lineCount({1, 0}) > 1);
}

void tablesTheGridCannotHoldStayMarkdown()
{
    GridEditor view(u"| a | b |\n|---|---|\n| 1 | 2 | 3 |\n\nText.\n\n| c | d |\n|---|---|\n| 4 | 5 |"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    // The first has a cell past its header: it shows as Markdown; the second is a grid.
    QVERIFY(view.grids(1));
    QCOMPARE(view.grid()->start(), int(view.text().indexOf(u"| c |")));
    for (QTextBlock block = view.binding->textDocument()->begin(); block.position() < view.binding->sourceToEditor(view.grid()->start());
         block = block.next())
        QVERIFY(block.isVisible());
    // Typed in, it takes edits as text does.
    view.select(3, 3);
    view.type(u"z"_s);
    QVERIFY(view.text().startsWith(u"| az | b |"_s));
}

} // namespace TableGridTests
