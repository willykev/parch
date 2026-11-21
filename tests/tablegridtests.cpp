#include "tablegridtests.h"

#include "apppaths.h"
#include "commandfilter.h"
#include "documentparse.h"
#include "mdtree.h"
#include "documentsettings.h"
#include "attachments.h"
#include "documentfind.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "mdtable.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "tablegrids.h"
#include "testfiles.h"
#include "theme.h"
#include "vimmode.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QImage>
#include <QInputMethodQueryEvent>
#include <QMimeData>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QStyleHints>
#include <QThreadPool>
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
    QCOMPARE(view.binding->tables()->adapt({end + 1, 1}, QString()).kind, TableGrids::Adapted::Refused);
    // The blank line above it may go: the paragraph there stays one, the table a table.
    QCOMPARE(view.binding->tables()->adapt({view.grid()->start() - 1, 1}, QString()).kind,
             TableGrids::Adapted::Pass);
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
    // Ctrl+A: the cell's text, then every cell, then everything.
    const int east = int(view.text().indexOf(u"East"));
    view.select(east + 1, east + 1);
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(selected(view), u"East"_s);
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(view.anchor(), int(view.text().indexOf(u"Time")));
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"12")) + 2);
    // Every cell is a block: copied as cells, emptied by Delete, the table kept.
    view.binding->copy();
    QCOMPARE(QGuiApplication::clipboard()->text(), u"Time\tShelter\tAir\n06:00\tNorth\t7.8\n10:00\tEast\t12\n"_s);
    QVERIFY(QGuiApplication::clipboard()->mimeData()->hasHtml());
    view.key(Qt::Key_Delete);
    QCOMPARE(view.binding->tables()->grids().size(), 1);
    QCOMPARE(cells(view), u"|| / || / ||"_s);
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    view.select(east + 1, east + 1);
    view.key(Qt::Key_A, Qt::ControlModifier);
    view.key(Qt::Key_A, Qt::ControlModifier);
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
    QVERIFY2(view.text().contains(u"| 12  |\n\nNote\n\nAfter"_s), qPrintable(view.text()));
    QCOMPARE(view.binding->tables()->grids().size(), 1);
    // Nothing pulls the text below up against the table.
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
    // In a cell no table goes in: the table's own menu opens instead.
    QMetaObject::invokeMethod(view.pane, "chooseTable");
    QObject *tableMenu = view.pane->findChild<QObject *>(u"tableMenu"_s);
    QVERIFY(tableMenu);
    QTRY_VERIFY(tableMenu->property("opened").toBool());
    QVERIFY(!picker->property("opened").toBool());
    QMetaObject::invokeMethod(tableMenu, "close");
    QTRY_VERIFY(!tableMenu->property("visible").toBool());
    // Escape leaves the picker without a table.
    const QString before = view.text();
    const int another = int(before.indexOf(u"Another"));
    view.select(another, another);
    QMetaObject::invokeMethod(view.pane, "chooseTable");
    QTRY_VERIFY(picker->property("opened").toBool());
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

void guideDescribesTheActionsInEachTableMenu()
{
    GridEditor view(u"| A | B | C |\n|---|---|---|\n| one | centre | three |\n| four | five | six |\n"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    activate(&view.window);
    const int position = int(view.text().indexOf(u"centre")) + 2;
    view.select(position, position);
    QObject *menu = view.pane->findChild<QObject *>(u"tableMenu"_s);
    QVERIFY(menu);
    const QStringList rowActions{u"insertRowAboveEntry"_s, u"insertRowBelowEntry"_s, u"duplicateRowEntry"_s, u"deleteRowEntry"_s};
    const QStringList columnActions{u"insertColumnLeftEntry"_s, u"insertColumnRightEntry"_s, u"deleteColumnEntry"_s};
    const QStringList rowHandleOnly{u"moveRowUpEntry"_s, u"moveRowDownEntry"_s};
    const QStringList columnHandleOnly{u"moveColumnLeftEntry"_s, u"moveColumnRightEntry"_s, u"alignDefaultEntry"_s, u"alignLeftEntry"_s,
                                      u"alignCenterEntry"_s, u"alignRightEntry"_s, u"sortAscendingEntry"_s, u"sortDescendingEntry"_s};
    const QStringList cellOnly{u"mergeCellsEntry"_s, u"splitCellEntry"_s, u"showMarkdownEntry"_s, u"deleteTableEntry"_s};
    const auto commands = [&](const QStringList &visible) {
        for (const auto &id : rowActions + columnActions + rowHandleOnly + columnHandleOnly + cellOnly) {
            auto *entry = menu->findChild<QQuickItem *>(id);
            QVERIFY2(entry, qPrintable(id));
            QVERIFY2(entry->isVisible() == visible.contains(id), qPrintable(id));
        }
    };
    const QPoint cell = view.editor->mapToScene(view.binding->sourceRectangle(position).center()).toPoint();
    QTest::mouseClick(&view.window, Qt::RightButton, Qt::NoModifier, cell);
    QTRY_VERIFY(menu->property("opened").toBool());
    QCOMPARE(menu->property("kind").toString(), u"cell"_s);
    commands(rowActions + columnActions + cellOnly);
    QStringList cellLabels;
    for (const auto &id : cellOnly) cellLabels.append(menu->findChild<QQuickItem *>(id)->property("text").toString());
    QMetaObject::invokeMethod(menu, "close");
    QTRY_VERIFY(!menu->property("visible").toBool());
    for (const auto &kind : {u"row"_s, u"column"_s}) {
        view.select(position, position);
        QQuickItem *handle = findItem(view.window.contentItem(), kind + u"Handle");
        QTRY_VERIFY(handle && handle->isVisible());
        QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier, centreOf(handle));
        QTRY_VERIFY(menu->property("opened").toBool());
        QCOMPARE(menu->property("kind").toString(), kind);
        commands(kind == u"row" ? rowActions + rowHandleOnly : columnActions + columnHandleOnly);
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());
    }

    const QString guide = QString::fromUtf8(readFile(QStringLiteral(PARCH_SOURCE_DIR "/docs/guide.md")));
    const QString section = guide.section(u"## Write in tables\n"_s, 1, 1).section(u"\n## "_s, 0, 0);
    const QString paragraph = section.section(u"A right click in a cell"_s, 1, 1).section(u"\n- "_s, 0, 0).simplified();
    QVERIFY2(!paragraph.contains(u"offers all of these"), qPrintable(paragraph));
    QVERIFY2(paragraph.contains(u"row insertion, duplication and deletion"), qPrintable(paragraph));
    QVERIFY2(paragraph.contains(u"column insertion and deletion"), qPrintable(paragraph));
    for (const auto &label : cellLabels) QVERIFY2(paragraph.contains(u"**%1**"_s.arg(label)), qPrintable(label));
    QVERIFY2(paragraph.contains(u"row handle's menu to move a row"), qPrintable(paragraph));
    QVERIFY2(paragraph.contains(u"column handle's menu to move, align or sort a column"), qPrintable(paragraph));
    QVERIFY(paragraph.contains(u"**Table**"));
}

void showMarkdownFromTheCellMenuKeepsTheCaret_data()
{
    QTest::addColumn<QString>("route");
    QTest::addColumn<bool>("split");
    QTest::addColumn<bool>("deep");
    QTest::newRow("right-click") << u"right-click"_s << false << false;
    QTest::newRow("Menu") << u"Menu"_s << false << false;
    QTest::newRow("palette") << u"palette"_s << false << false;
    QTest::newRow("right-click-split-crlf") << u"right-click"_s << true << false;
    QTest::newRow("right-click-deep-in-table") << u"right-click"_s << false << true;
}

void showMarkdownFromTheCellMenuKeepsTheCaret()
{
    QFETCH(QString, route);
    QFETCH(bool, split);
    QFETCH(bool, deep);
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    shell.settings()->setTableGrids(true);
    const int flash = QGuiApplication::styleHints()->cursorFlashTime();
    QGuiApplication::styleHints()->setCursorFlashTime(0);
    const auto blink = qScopeGuard([flash] { QGuiApplication::styleHints()->setCursorFlashTime(flash); });
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, split ? 900 : 500);
    const QString path = shell.root.filePath(u"grid.md"_s);
    const QString prose = u"Preceding text, outside the table.\n\n"_s.repeated(12);
    const QString words = u"many words for a cell to wrap in the narrower table. "_s.repeated(5);
    QString source = prose + u"| First | Second |\n|---|---|\n"_s;
    if (deep) source += u"| %1 | right |\n| left | %1 |\n"_s.arg(words.trimmed()).repeated(10);
    source += u"| %1target | right |\n\nAfter the table.\n"_s.arg(words);
    if (split) source.replace(u"\n"_s, u"\r\n"_s);
    writeFile(path, source.toUtf8());
    shell.backend->openFiles({path});
    Document *document = shell.document();
    EditorBinding *binding = shell.binding();
    if (split) {
        shell.context()->setLibraryOpen(false);
        shell.context()->tabs()->setActiveMode(TabModel::Split);
        QTRY_VERIFY(window->property("splitBoth").toBool());
    }
    QTRY_COMPARE(binding->tables()->grids().size(), 1);
    QVERIFY(binding->tables()->grids().first()->lineCount({1, 0}) > 1);
    // Another projection, without a second native window taking the popup's focus.
    QQmlComponent component(&shell.engine, QUrl(u"qrc:/EditorPane.qml"_s));
    const std::unique_ptr<QObject> second(component.createWithInitialProperties(
        {{u"width"_s, 640}, {u"height"_s, 480}, {u"tableGrids"_s, true}, {u"document"_s, QVariant::fromValue(document)}}));
    QVERIFY2(second, qPrintable(component.errorString()));
    auto *otherBinding = second->findChild<EditorBinding *>();
    QVERIFY(otherBinding);
    QTRY_COMPARE(otherBinding->tables()->grids().size(), 1);

    activate(window);
    QQuickItem *editor = shell.item(u"editor"_s);
    QTRY_COMPARE(window->activeFocusItem(), editor);
    const int position = int(source.indexOf(u"target")) + 2;
    binding->select(position, position);
    QCOMPARE(binding->cursorSource(), position);
    const Revision revision = document->buffer()->revision();
    if (route == u"palette") {
        sendKey(window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QObject *palette = shell.object(u"commandPalette"_s);
        QTRY_VERIFY(palette && palette->property("opened").toBool());
        auto *field = qobject_cast<QQuickItem *>(shell.object(u"paletteField"_s));
        QVERIFY(field);
        field->setProperty("text", u"Show table as Markdown"_s);
        auto *matches = qobject_cast<CommandFilter *>(shell.object(u"paletteMatches"_s));
        QVERIFY(matches);
        QTRY_COMPARE(matches->idAt(0), u"showTableMarkdown"_s);
        QTRY_COMPARE(window->activeFocusItem(), field);
        sendKey(window, Qt::Key_Return);
        QTRY_VERIFY(!palette->property("visible").toBool());
    } else {
        if (route == u"Menu") sendKey(window, Qt::Key_Menu);
        else {
            const QRectF point = binding->sourceRectangle(position);
            const QPointF click = editor->mapToScene({point.x() + 0.25, point.center().y()});
            QTest::mouseClick(window, Qt::RightButton, Qt::NoModifier, click.toPoint());
        }
        QObject *menu = shell.object(u"tableMenu"_s);
        QTRY_VERIFY(menu && menu->property("opened").toBool());
        auto *entry = menu->findChild<QQuickItem *>(u"showMarkdownEntry"_s);
        QVERIFY(entry && entry->isVisible() && entry->isEnabled());
        QCOMPARE(binding->cursorSource(), position);
        QSignalSpy triggered(entry, SIGNAL(triggered()));
        QTest::mouseClick(entry->window(), Qt::LeftButton, Qt::NoModifier, centreOf(entry));
        QCOMPARE(triggered.size(), 1);
        QTRY_VERIFY(!menu->property("visible").toBool());
    }
    QTRY_COMPARE(binding->tables()->grids().size(), 0);
    QCOMPARE(otherBinding->tables()->grids().size(), 1);
    QCOMPARE(document->buffer()->revision(), revision);
    QVERIFY(!document->buffer()->canUndo());
    QCOMPARE(document->buffer()->text(), source);
    QCOMPARE(binding->cursorSource(), position);
    QTRY_COMPARE(window->activeFocusItem(), editor);
    QTRY_VERIFY(caretDrawnAt(binding, editor, position));
    auto *flickable = shell.item(u"flickable"_s);
    QVERIFY(flickable);
    QTRY_VERIFY(flickable->mapRectToItem(editor, flickable->boundingRect()).contains(binding->sourceRectangle(position).center()));
    typeText(window, u"x"_s);
    QString changed = source;
    changed.insert(position, u'x');
    QCOMPARE(document->buffer()->text(), changed);
    shell.backend->store()->save(document);
    QTRY_COMPARE(document->state(), Document::State::Clean);
    QCOMPARE(readFile(path), changed.toUtf8());
}

void externallyReloadedTablesProjectInANewView_data()
{
    QTest::addColumn<bool>("inside");
    QTest::addColumn<bool>("parsedBeforeActivation");
    QTest::addColumn<bool>("earlierUndo");
    QTest::addColumn<bool>("startsWithTable");
    QTest::newRow("reload-parses-in-reopened-view") << true << false << true << false;
    QTest::newRow("reopened-before-parse-caret-in-prose") << false << false << false << false;
    QTest::newRow("already-parsed-with-undo") << true << true << true << false;
    QTest::newRow("new-view-on-leading-table") << true << true << false << true;
}

void externallyReloadedTablesProjectInANewView()
{
    QFETCH(bool, inside);
    QFETCH(bool, parsedBeforeActivation);
    QFETCH(bool, earlierUndo);
    QFETCH(bool, startsWithTable);
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    shell.settings()->setTableGrids(true);
    QQuickWindow *window = shell.window.get();
    activate(window);
    const QString path = shell.root.filePath(u"grid.md"_s);
    const QString secondFile = shell.root.filePath(u"second.md"_s);
    // The caret follows the surviving suffix through an otherwise full rewrite.
    const QString suffix = u"\n| Target | cello |\n\nAfter the rewrite.\n"_s;
    const QString old = u"Older prose at the start of the file. "_s.repeated(12) + u"\n\n"_s + weather
                        + u"\n\nAnother paragraph before the row."_s + suffix;
    const QString fresh = (startsWithTable ? QString() : u"Different introduction.\n\n"_s)
                          + u"| New | Columns |\n|---|---|"_s + suffix;
    writeFile(path, old.toUtf8());
    writeFile(secondFile, "Another open file.\n");
    shell.backend->openFiles({path});
    Document *document = shell.document();
    EditorBinding *binding = shell.binding();
    QVERIFY(document && binding);
    QTRY_COMPARE(binding->tables()->grids().size(), 1);
    if (earlierUndo) {
        document->buffer()->replace({0, 0}, u"An earlier edit.\n\n"_s);
        shell.backend->store()->save(document);
        QTRY_COMPARE(document->state(), Document::State::Clean);
        QVERIFY(document->buffer()->canUndo());
    }
    const int position = inside ? int(fresh.indexOf(u"Target")) + 3 : int(fresh.indexOf(u"After"));
    const int oldPosition = position + document->buffer()->length() - int(fresh.size());
    binding->select(oldPosition, oldPosition);
    QCOMPARE(binding->cursorSource(), oldPosition);
    const Revision before = document->buffer()->revision();
    shell.backend->openFiles({secondFile});
    QVERIFY(shell.document() != document);
    TabModel *tabs = shell.context()->tabs();
    const int tab = tabs->indexOf(document);
    QCOMPARE(tabs->viewState(tab).cursor, oldPosition);
    auto *parsePool = QCoreApplication::instance()->findChild<QThreadPool *>(u"parse"_s);
    QVERIFY(parsePool);
    PoolBlock parsing(parsePool);
    if (parsedBeforeActivation) parsing.release();
    writeFile(path, fresh.toUtf8());
    QTRY_COMPARE(document->buffer()->text(), fresh);
    QCOMPARE(document->state(), Document::State::Clean);
    QVERIFY(document->buffer()->canUndo());
    const Revision revision = document->buffer()->revision();
    QVERIFY(revision > before);
    const ViewState saved = tabs->viewState(tab);
    QCOMPARE(saved.cursor, position);
    document->parse()->request();
    if (parsedBeforeActivation) QTRY_VERIFY(document->parse()->current());
    else QVERIFY(!document->parse()->current());
    shell.backend->openForwarded({path}, {});
    QTRY_COMPARE(shell.document(), document);
    QTRY_COMPARE(binding->document(), document);
    QCOMPARE(binding->cursorSource(), position);
    parsing.release();
    QTRY_VERIFY(binding->tables()->settled());
    QCOMPARE(document->parse()->current()->revision, revision);
    const int reopened = binding->tables()->grids().size();

    shell.backend->newWindow();
    WindowContext *second = shell.backend->windows().at(1);
    second->tabs()->openDocument(document, saved);
    QQuickWindow *other = shell.windowFor(second);
    QVERIFY(other);
    auto *otherBinding = other->findChild<EditorBinding *>();
    QVERIFY(otherBinding);
    QTRY_COMPARE(otherBinding->document(), document);
    QTRY_VERIFY(otherBinding->tables()->settled());
    QCOMPARE(otherBinding->cursorSource(), position);
    const int newView = otherBinding->tables()->grids().size();
    QCOMPARE(document->buffer()->revision(), revision);
    QCOMPARE(readFile(path), fresh.toUtf8());
    QCOMPARE((std::pair{reopened, newView}), (std::pair{1, 1}));
    QCOMPARE(binding->tables()->grids().first()->rowTexts(1), (QStringList{u"Target"_s, u"cello"_s}));
}

void showGridCanRestoreACompletedTableUnderTheCaret_data()
{
    QTest::addColumn<bool>("manual");
    QTest::newRow("manual-override") << true;
    QTest::newRow("completed-under-caret") << false;
}

void showGridCanRestoreACompletedTableUnderTheCaret()
{
    QFETCH(bool, manual);
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    shell.settings()->setTableGrids(true);
    QQuickWindow *window = shell.window.get();
    activate(window);
    const QString path = shell.root.filePath(u"grid.md"_s);
    const QString starting = manual ? weather : u"Readings.\n\n\n\nAfter."_s;
    writeFile(path, starting.toUtf8());
    shell.backend->openFiles({path});
    Document *document = shell.document();
    EditorBinding *binding = shell.binding();
    TableGrids *tables = binding->tables();
    QTRY_VERIFY(tables->settled());
    auto *parsePool = QCoreApplication::instance()->findChild<QThreadPool *>(u"parse"_s);
    QVERIFY(parsePool);
    PoolBlock parsing(parsePool);
    if (manual) {
        parsing.release();
        QTRY_COMPARE(tables->grids().size(), 1);
        const int cell = int(starting.indexOf(u"North")) + 1;
        binding->select(cell, cell);
        QVERIFY(tables->runCommand(u"showTableMarkdown"_s));
        const int after = int(starting.indexOf(u"After"));
        binding->select(after, after);
        QVERIFY(!tables->can(u"showTableGrid"_s));
        QCOMPARE(tables->grids().size(), 0);
        binding->select(cell, cell);
    } else {
        binding->select(11, 11);
        typeText(window, u"| A | B |"_s);
        sendKey(window, Qt::Key_Return);
        typeText(window, u"|---|---|"_s);
        sendKey(window, Qt::Key_Return);
        typeText(window, u"| one | two"_s);
        QVERIFY(document->buffer()->text().contains(u"| A | B |\n|---|---|\n| one | two"));
        QVERIFY(!tables->settled());
        QVERIFY(!tables->can(u"showTableGrid"_s));
    }
    QCOMPARE(tables->grids().size(), 0);
    const int caret = binding->cursorSource();
    const QString before = document->buffer()->text();
    const Revision revision = document->buffer()->revision();
    const qsizetype undo = document->buffer()->undoDepth();
    sendKey(window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    QObject *palette = shell.object(u"commandPalette"_s);
    QTRY_VERIFY(palette && palette->property("opened").toBool());
    auto *field = qobject_cast<QQuickItem *>(shell.object(u"paletteField"_s));
    QVERIFY(field);
    field->setProperty("text", u"Show table as grid"_s);
    auto *matches = qobject_cast<CommandFilter *>(shell.object(u"paletteMatches"_s));
    QVERIFY(matches);
    QTRY_COMPARE(matches->idAt(0), u"showTableGrid"_s);
    auto *list = qobject_cast<QQuickItem *>(shell.object(u"paletteList"_s));
    QVERIFY(list);
    const auto choice = [&] { return list->property("currentItem").value<QQuickItem *>(); };
    QTRY_VERIFY(choice() && choice()->property("commandId").toString() == u"showTableGrid");
    if (!manual) QVERIFY(!choice()->property("available").toBool());
    parsing.release();
    QTRY_VERIFY(tables->settled());
    QCOMPARE(tables->grids().size(), 0);
    QVERIFY(tables->can(u"showTableGrid"_s));
    QTRY_VERIFY(choice() && choice()->property("available").toBool());
    QTRY_COMPARE(window->activeFocusItem(), field);
    sendKey(window, Qt::Key_Return);
    QTRY_VERIFY(!palette->property("visible").toBool());
    QTRY_COMPARE(tables->grids().size(), 1);
    QTRY_COMPARE(window->activeFocusItem(), shell.item(u"editor"_s));
    QCOMPARE(binding->cursorSource(), caret);
    QVERIFY(tables->caretInGrid());
    QCOMPARE(document->buffer()->text(), before);
    QCOMPARE(document->buffer()->revision(), revision);
    QCOMPARE(document->buffer()->undoDepth(), undo);
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
    QVERIFY(!view.binding->tables()->can(u"showTableGrid"_s));
    QVERIFY(!view.binding->tables()->runCommand(u"showTableGrid"_s));
    view.type(u"z"_s);
    QVERIFY(view.text().startsWith(u"| az | b |"_s));
}

namespace {

// A table whose second row has a cell merged over two columns.
const QString mergedTable = u"Intro\n\n| A | B | C |\n| --- | --- | --- |\n| one || keep |\n| ax | bx | cx |\n\nAfter"_s;

void pasteText(GridEditor &view, const QString &text, const QString &html = {})
{
    auto *mime = new QMimeData;
    mime->setText(text);
    if (!html.isEmpty()) mime->setHtml(html);
    QGuiApplication::clipboard()->setMimeData(mime);
    view.binding->paste();
}

// The layout a grid draws a cell's text with.
QTextLayout *drawnLayout(GridEditor &view, TableGrid::Place place)
{
    TableGrid *grid = view.grid();
    const QRectF rect = grid->cellRect(place);
    for (const TableGrid::Drawn &drawn : grid->drawn(0, grid->height() + 1))
        if (drawn.rect.contains(rect.center())) return drawn.layout;
    return nullptr;
}

} // namespace

void aColumnsHandleActsOnTheColumnItPicked()
{
    GridEditor view(mergedTable);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrids *tables = view.binding->tables();
    // B's handle: the merged cell over A and B narrows to A, and A stays.
    tables->selectColumn(view.grid(), 1, false);
    QVERIFY(tables->runCommand(u"deleteTableColumn"_s));
    QCOMPARE(cells(view), u"A|C / one|keep / ax|cx"_s);
    view.binding->undo();
    // The column stays the one picked through other commands on it.
    tables->selectColumn(view.grid(), 1, false);
    QVERIFY(tables->runCommand(u"alignTableColumnRight"_s));
    QVERIFY(tables->runCommand(u"deleteTableColumn"_s));
    QCOMPARE(cells(view), u"A|C / one|keep / ax|cx"_s);
}

void aRefusedPasteOfCellsChangesNothing()
{
    GridEditor view(mergedTable);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const QString before = view.text();
    // Two values over a block that a merged cell would have to be cut for.
    view.select(int(before.indexOf(u"one")), int(before.indexOf(u"bx")) + 2);
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    pasteText(view, u"x\ty\n"_s);
    QCOMPARE(refused.count(), 1);
    QCOMPARE(view.text(), before);
}

void textWrittenBelowATableStaysText()
{
    const QString text = u"Intro\n\n| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int blank = int(text.indexOf(u"\n\nAfter")) + 1;
    // Prose with pipes is prose, all of it, below the blank line.
    view.select(blank, blank);
    pasteText(view, u"note | with | tail\n"_s);
    QVERIFY2(view.text().contains(u"| one | two |\n\nnote | with | tail\n"_s), qPrintable(view.text()));
    QCOMPARE(cells(view), u"A|B / one|two"_s);
    view.binding->undo();
    QCOMPARE(view.text(), text);
    // A whole row joins the table.
    view.select(blank, blank);
    pasteText(view, u"| three | four |\n"_s);
    QCOMPARE(cells(view), u"A|B / one|two / three|four"_s);
}

void aBacktickThatWouldJoinCellsIsWrittenEscaped()
{
    // A backtick before aaa would open code that the one after bbb closes.
    const QString text = u"| A | B |\n| --- | --- |\n| aaa | bbb` |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int aaa = int(text.indexOf(u"aaa"));
    view.select(aaa, aaa);
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    view.type(u"`"_s);
    QCOMPARE(refused.count(), 0);
    QCOMPARE(cells(view), u"A|B / \\`aaa|bbb`"_s);
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"\\`aaa"_s)) + 2);
    // A backtick written after it closes it: the two are code in the cell.
    view.key(Qt::Key_End);
    view.type(u"`"_s);
    QCOMPARE(cells(view), u"A|B / `aaa`|bbb`"_s);
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"`aaa`"_s)) + 5);
    QCOMPARE(refused.count(), 0);
    // Taking the backslash away alone would join the cells: refused.
    view.binding->undo();
    QCOMPARE(cells(view), u"A|B / \\`aaa|bbb`"_s);
    const QString escaped = view.text();
    const int slash = int(escaped.indexOf(u"\\`aaa"_s)) + 1;
    view.select(slash, slash);
    view.key(Qt::Key_Backspace);
    QCOMPARE(refused.count(), 1);
    QCOMPARE(view.text(), escaped);
}

void slowRichPastesGoInAsACellsText()
{
    const QString text = u"Intro\n\n| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    Attachments attachments(AppPaths::under(view.root.path()));
    view.binding->setAttachments(&attachments);
    const int one = int(text.indexOf(u"one"));
    view.select(one, one + 3);
    // A page large enough to be converted off the window's thread.
    pasteText(view, u"First paragraph.\n\nSecond paragraph."_s,
              u"<p>First paragraph.</p><p>Second paragraph.</p><!--"_s + QString(65536, u' ') + u"-->"_s);
    QTRY_VERIFY(view.text().contains(u"Second paragraph."_s));
    QCOMPARE(cells(view), u"A|B / First paragraph. Second paragraph.|two"_s);
    view.binding->setAttachments(nullptr);
}

void anEditReachingIntoTwoTablesIsRefused()
{
    const QString text = u"| A | B |\n| --- | --- |\n| first | side |\n\nmiddle\n\n| C | D |\n| --- | --- |\n| last | tail |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(2));
    TableGrids *tables = view.binding->tables();
    const int first = int(text.indexOf(u"first")) + 2;
    // From a cell of one into a cell of the other, or past the other whole.
    QCOMPARE(tables->adapt({first, int(text.indexOf(u"last")) + 2 - first}, u"x"_s).kind, TableGrids::Adapted::Refused);
    QCOMPARE(tables->adapt({first, int(text.indexOf(u"After")) - first}, u"replacement\nline"_s).kind,
             TableGrids::Adapted::Refused);
    // A selection from the cell to the text after both takes the first whole too.
    view.select(first, int(text.indexOf(u"After")));
    QCOMPARE(view.anchor(), view.grid(0)->start());
    pasteText(view, u"replacement\nline"_s);
    QCOMPARE(view.text(), u"replacement\nlineAfter"_s);
}

void aDropOnRowsNoLongerThereMovesNothing()
{
    const QString text = u"| A |\n| --- |\n| r1 |\n| r2 |\n| r3 |\n| r4 |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrids *tables = view.binding->tables();
    tables->selectRow(view.grid(), 1, false);
    // Another window takes the last two rows away while a drag is held.
    const int r3 = int(text.indexOf(u"| r3"));
    view.document->buffer()->replace({r3, int(text.indexOf(u"\n\nAfter")) - r3 + 1}, QString(), EditKind::Other);
    const QString now = view.text();
    QVERIFY(!tables->moveRowsTo(view.grid(), 5));
    QCOMPARE(view.text(), now);
}

void anEmojiReplacesAnotherInACell()
{
    const QString text = u"| A   | B   |\n| --- | --- |\n| 😀  | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int emoji = int(text.indexOf(u"😀"_s));
    view.select(emoji, emoji + 2);
    pasteText(view, u"😃"_s);
    QVERIFY2(view.text().contains(u"| 😃  | two |"_s), qPrintable(view.text()));
    view.binding->undo();
    QCOMPARE(view.text(), text);
}

void cellsAShortRowLeavesOutAreReachedOneByOne()
{
    const QString text = u"| A | B | C |\n| --- | --- | --- |\n| x |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int x = int(text.indexOf(u"x |"));
    view.select(x + 1, x + 1);
    view.key(Qt::Key_Tab);
    QCOMPARE(view.grid()->placeOf(view.cursor()), (TableGrid::Place{1, 1}));
    view.key(Qt::Key_Tab);
    QCOMPARE(view.grid()->placeOf(view.cursor()), (TableGrid::Place{1, 2}));
    view.type(u"C"_s);
    QCOMPARE(cells(view), u"A|B|C / x||C"_s);
}

void aSecondViewsCaretStaysWithItsRow()
{
    const QString text = u"| Colour |\n| --- |\n| red |\n| tan |\n| blue |\n| gold |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    auto second = view.secondView();
    QVERIFY(second && second->binding);
    QTRY_COMPARE(second->binding->tables()->grids().size(), 1);
    const int blue = int(text.indexOf(u"blue"));
    second->binding->select(blue + 2, blue + 2);
    // The first view deletes a row above it.
    const int red = int(text.indexOf(u"red"));
    view.select(red, red);
    QVERIFY(view.binding->tables()->runCommand(u"deleteTableRow"_s));
    const int now = int(view.text().indexOf(u"blue"));
    QCOMPARE(second->binding->cursorSource(), now + 2);
}

void aSecondViewsCaretStaysWithItsRowAmongLikeOnes()
{
    const QString text = u"| Colour |\n| --- |\n| red |\n| tan |\n| blue |\n| blue |\n| gold |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    auto second = view.secondView();
    QVERIFY(second && second->binding);
    QTRY_COMPARE(second->binding->tables()->grids().size(), 1);
    // In the first of two blue rows, after "bl".
    const int blue = int(text.indexOf(u"blue"));
    second->binding->select(blue + 2, blue + 2);
    // A row deleted above it: the caret is in the same blue row.
    const int red = int(text.indexOf(u"red"));
    view.select(red, red);
    QVERIFY(view.binding->tables()->runCommand(u"deleteTableRow"_s));
    QCOMPARE(second->binding->cursorSource(), int(view.text().indexOf(u"blue")) + 2);
    // Its own row written to in the first view: it stays in it.
    const int first = int(view.text().indexOf(u"blue"));
    view.select(first + 4, first + 4);
    view.type(u"X"_s);
    QCOMPARE(second->binding->cursorSource(), int(view.text().indexOf(u"blueX")) + 2);
}

void aChangeOfDialectReadsTheTableAgain()
{
    GridEditor view(u"| A | B | C |\n| --- | --- | --- |\n| left || end |\n\nAfter"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrids *tables = view.binding->tables();
    const int left = int(view.text().indexOf(u"left"));
    view.select(left, left);
    QCOMPARE(view.grid()->cellCount(1), 2);
    QVERIFY(tables->can(u"splitTableCell"_s));
    // In the Standard dialect two pipes make an empty cell.
    Dialect::Layers standard;
    standard.document.profile = md::Profile::Standard;
    view.document->setSavedDialect(standard);
    QTRY_COMPARE(view.grid()->cellCount(1), 3);
    QVERIFY(!tables->can(u"splitTableCell"_s));
}

void vimRowCopiesKeepMergedCells()
{
    GridEditor view(u"| A | B | C |\n| --- | --- | --- |\n| one || end |\n| two | x | y |\n\nAfter"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.pane->setProperty("vimKeys", true);
    QVERIFY(view.grids(1));
    const int one = int(view.text().indexOf(u"one"));
    view.select(one, one);
    typeText(&view.window, u"yyp"_s);
    QCOMPARE(cells(view), u"A|B|C / one|end / one|end / two|x|y"_s);
    QCOMPARE(view.grid()->cellAt({2, 0}).span, 2);
}

void copiedCellsKeepTheirQuotes()
{
    GridEditor view(u"| A | B |\n| --- | --- |\n| \"q\" | two |\n| r | s |\n\nAfter"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    view.select(int(view.text().indexOf(u"\"q\"")), int(view.text().indexOf(u"two")) + 3);
    view.binding->copy();
    const int r = int(view.text().indexOf(u"| r |")) + 2;
    view.select(r, r);
    view.binding->paste();
    QCOMPARE(cells(view), u"A|B / \"q\"|two / \"q\"|two"_s);
}

void inlineCommandsFormatEachSelectedCell()
{
    const QString text = u"| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    view.select(int(text.indexOf(u"A")), int(text.indexOf(u"two")) + 3);
    QVERIFY(view.binding->format(u"bold"_s));
    QCOMPARE(cells(view), u"**A**|**B** / **one**|**two**"_s);
    view.binding->undo();
    QCOMPARE(view.text(), text);
}

void aColumnAlignedAnewIsDrawnAligned()
{
    GridEditor view(u"| Wide heading |\n| --- |\n| x |\n\nAfter"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int x = int(view.text().indexOf(u"x |"));
    view.select(x, x);
    QTextLayout *before = drawnLayout(view, {1, 0});
    QVERIFY(before);
    QCOMPARE(before->textOption().alignment() & Qt::AlignHorizontal_Mask, Qt::AlignLeft);
    QVERIFY(view.binding->tables()->runCommand(u"alignTableColumnRight"_s));
    QTextLayout *after = drawnLayout(view, {1, 0});
    QVERIFY(after);
    QCOMPARE(after->textOption().alignment() & Qt::AlignHorizontal_Mask, Qt::AlignRight);
}

void aCompositionShowsInItsCell()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int north = int(view.text().indexOf(u"North"));
    view.select(north + 5, north + 5);
    sendPreedit(&view.window, u"ka"_s);
    QTextLayout *layout = drawnLayout(view, {1, 1});
    QVERIFY(layout);
    QCOMPARE(layout->preeditAreaText(), u"ka"_s);
    QCOMPARE(layout->preeditAreaPosition(), 5);
    sendPreedit(&view.window, QString());
    layout = drawnLayout(view, {1, 1});
    QVERIFY(layout);
    QVERIFY(layout->preeditAreaText().isEmpty());
}

void findSeesATableShownAsMarkdown()
{
    GridEditor view(u"Intro\n\n| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    DocumentFind find;
    find.setDocument(view.document);
    find.setBinding(view.binding);
    find.setActive(true);
    find.setPattern(u"|"_s);
    QTRY_COMPARE(find.count(), 0);
    const int one = int(view.text().indexOf(u"one"));
    view.select(one, one);
    QVERIFY(view.binding->tables()->runCommand(u"showTableMarkdown"_s));
    QTRY_COMPARE(find.count(), 9);
}

void doubleAndTripleClicksSelectInACell()
{
    GridEditor view(u"| A | B |\n| --- | --- |\n| one two | x |\n\nAfter"_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    QQuickItem *drawn = findItem(view.window.contentItem(), u"tableGrid"_s);
    QVERIFY(drawn);
    // Over "two", the second word of the cell.
    QTextLayout *layout = drawnLayout(view, {1, 0});
    QVERIFY(layout);
    const QRectF cell = grid->cellRect({1, 0});
    const qreal x = cell.left() + grid->padding() + layout->lineAt(0).cursorToX(5);
    const QPoint at = drawn->mapToScene(QPointF(x, cell.center().y())).toPoint();
    QTest::mouseDClick(&view.window, Qt::LeftButton, Qt::NoModifier, at);
    QCOMPARE(selected(view), u"two"_s);
    QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier, at);
    QCOMPARE(selected(view), u"one two"_s);
    // A click elsewhere just after a double click puts the caret there.
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
    QTest::mouseDClick(&view.window, Qt::LeftButton, Qt::NoModifier, at);
    const QRectF other = grid->cellRect({1, 1});
    QTest::mouseClick(&view.window, Qt::LeftButton, Qt::NoModifier,
                      drawn->mapToScene(QPointF(other.left() + grid->padding() + 1, other.center().y())).toPoint());
    QCOMPARE(view.anchor(), view.cursor());
    QCOMPARE(view.cell(), u"x"_s);
}

void homeAndEndKeepToTheLineInAWrappedCell()
{
    GridEditor view(u"| A |\n| --- |\n| "_s + QStringLiteral("word ").repeated(45).trimmed() + u" |\n\nAfter"_s,
                    QSize(320, 480));
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    const TableGrid::Place place{1, 0};
    QTRY_VERIFY(grid->lineCount(place) > 2);
    const auto [start, end] = grid->lineRange(place, 1);
    view.select(start + 2, start + 2);
    view.key(Qt::Key_Home);
    QCOMPARE(view.cursor(), start);
    view.key(Qt::Key_End);
    QCOMPARE(view.cursor(), end);
    QCOMPARE(grid->lineOf(place, view.cursor()), 1);
    QVERIFY(start > grid->cellAt(place).start && end < grid->cellAt(place).end);
}

void selectAllTakesThreePressesToTheWholeText()
{
    const QString text = u"Before.\n\n| A | B |\n| --- | --- |\n| one |  |\n\nAfter."_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    TableGrid *grid = view.grid();
    // From an empty cell: the cell, every cell, then the whole text.
    const int empty = grid->cellAt({1, 1}).start;
    view.select(empty, empty);
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(view.anchor(), empty);
    QCOMPARE(view.cursor(), empty);
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(view.anchor(), grid->cellAt({0, 0}).start);
    QCOMPARE(view.cursor(), grid->cellAt({1, 1}).end);
    view.key(Qt::Key_Delete);
    QCOMPARE(cells(view), u"| / |"_s);
    QVERIFY(view.text().startsWith(u"Before."_s) && view.text().endsWith(u"After."_s));
    view.binding->undo();
    QCOMPARE(view.text(), text);
    view.select(empty, empty);
    for (int press = 0; press < 3; ++press) view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(selected(view), view.text());
}

void selectAllInATableOfOneCellStillStopsAtTheCell()
{
    GridEditor view(u"Before.\n\n| Alone |\n| --- |\n\nAfter."_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int alone = int(view.text().indexOf(u"Alone"));
    view.select(alone, alone);
    view.key(Qt::Key_A, Qt::ControlModifier);
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(selected(view), u"Alone"_s);
    // Every cell, though there is one, copies as cells.
    view.binding->copy();
    QCOMPARE(QGuiApplication::clipboard()->text(), u"Alone\n"_s);
    QVERIFY(QGuiApplication::clipboard()->mimeData()->hasHtml());
    view.key(Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(selected(view), view.text());
}

void aSlowPasteGoesWhereItWasMadeAfterTheViewMovesOn()
{
    const QString text = u"Intro\n\n| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    Attachments attachments(AppPaths::under(view.root.path()));
    view.binding->setAttachments(&attachments);
    const QString otherText = u"Other\n\n| A | B |\n| --- | --- |\n| red | two |\n\nAfter"_s;
    writeFile(view.root.filePath(u"other.md"_s), otherText.toUtf8());
    Document *other = view.store->open(view.root.filePath(u"other.md"_s));
    QVERIFY(other);
    Document *pasted = view.document;
    const int one = int(text.indexOf(u"one"));
    view.select(one, one + 3);
    pasteText(view, u"First paragraph.\n\nSecond paragraph."_s,
              u"<p>First paragraph.</p><p>Second paragraph.</p><!--"_s + QString(65536, u' ') + u"-->"_s);
    // The window shows another document before the paste is converted.
    view.pane->setProperty("document", QVariant::fromValue(other));
    QTRY_VERIFY(pasted->buffer()->text().contains(u"Second paragraph."_s));
    QVERIFY2(pasted->buffer()->text().contains(u"| First paragraph. Second paragraph. | two |"_s),
             qPrintable(pasted->buffer()->text()));
    QCOMPARE(other->buffer()->text(), otherText);
    // The same text pasted again over itself changes nothing.
    const QString now = pasted->buffer()->text();
    view.pane->setProperty("document", QVariant::fromValue(pasted));
    QVERIFY(view.grids(1));
    const int first = int(now.indexOf(u"First"));
    view.select(first, first + int(QStringLiteral("First paragraph. Second paragraph.").size()));
    pasteText(view, u"First paragraph.\n\nSecond paragraph."_s,
              u"<p>First paragraph.</p><p>Second paragraph.</p><!--"_s + QString(65536, u' ') + u"-->"_s);
    view.pane->setProperty("document", QVariant::fromValue(other));
    QTest::qWait(1500);
    QCOMPARE(pasted->buffer()->text(), now);
    view.binding->setAttachments(nullptr);
}

void aSlowPasteLeavesACaretTakenElsewhere()
{
    const QString text = u"Intro\n\n| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    Attachments attachments(AppPaths::under(view.root.path()));
    view.binding->setAttachments(&attachments);
    const int one = int(text.indexOf(u"one"));
    view.select(one, one + 3);
    pasteText(view, u"First paragraph.\n\nSecond paragraph."_s,
              u"<p>First paragraph.</p><p>Second paragraph.</p><!--"_s + QString(65536, u' ') + u"-->"_s);
    // The caret goes on into the paragraph after the table meanwhile.
    const int after = int(text.indexOf(u"After"));
    view.select(after + 2, after + 2);
    QTRY_VERIFY(view.text().contains(u"Second paragraph."_s));
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"After")) + 2);
    view.type(u"X"_s);
    QVERIFY(view.text().endsWith(u"AfXter"_s));
    view.binding->setAttachments(nullptr);
}

void anImagePastedInACellAsksForItsDescription()
{
    const QString text = u"Intro\n\n| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    Attachments attachments(AppPaths::under(view.root.path()));
    view.binding->setAttachments(&attachments);
    const int one = int(text.indexOf(u"one"));
    view.select(one, one + 3);
    QImage image(4, 4, QImage::Format_RGB32);
    image.fill(Qt::red);
    auto *mime = new QMimeData;
    mime->setImageData(image);
    QGuiApplication::clipboard()->setMimeData(mime);
    view.binding->paste();
    QTRY_VERIFY(view.text().contains(u"![]("_s));
    // The caret waits between the brackets for the image's description.
    QTRY_COMPARE(view.cursor(), int(view.text().indexOf(u"![]("_s)) + 2);
    view.type(u"Red"_s);
    QVERIFY2(view.text().contains(u"![Red]("_s), qPrintable(view.text()));
    view.binding->setAttachments(nullptr);
}

void aSelectionEndingWhereATableStartsLeavesItOut()
{
    const QString text = u"Intro\n\n\n| a | b |\n| --- | --- |\n| 1 | 2 |\n\nEnd"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int start = view.grid()->start();
    // Whole lines selected above the table stop where it starts.
    view.select(0, 0);
    for (int i = 0; i < 3; ++i) view.key(Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(view.anchor(), 0);
    QCOMPARE(view.cursor(), start);
    view.key(Qt::Key_Delete);
    QCOMPARE(view.text(), text.mid(start));
    QCOMPARE(cells(view), u"a|b / 1|2"_s);
    view.binding->undo();
    QCOMPARE(view.text(), text);
    QVERIFY(view.grids(1));
    // The line just above it taken away, selected or with Vim's dd: the
    // table moves up, a grid still, drawn where it now is.
    const QString moved = u"Intro\n\n| a | b |\n| --- | --- |\n| 1 | 2 |\n\nEnd"_s;
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    view.select(start - 1, start);
    QCOMPARE(view.cursor(), start);
    view.key(Qt::Key_Delete);
    QCOMPARE(refused.count(), 0);
    QCOMPARE(view.text(), moved);
    const auto drawnInPlace = [&] {
        TableGrid *grid = view.grid();
        if (view.binding->tables()->grids().size() != 1 || grid->start() != start - 1) return false;
        const QTextBlock header = view.binding->textDocument()->findBlock(view.binding->sourceToEditor(grid->start()));
        return header.blockFormat().lineHeight() == std::ceil(grid->height()) && !header.next().isVisible();
    };
    QVERIFY(drawnInPlace());
    view.binding->undo();
    QCOMPARE(view.text(), text);
    QVERIFY(view.grids(1));
    view.pane->setProperty("vimKeys", true);
    view.select(start - 1, start - 1);
    typeText(&view.window, u"dd"_s);
    QCOMPARE(refused.count(), 0);
    QCOMPARE(view.text(), moved);
    QVERIFY(drawnInPlace());
}

void textWithATabPastedInACellGoesInAsText()
{
    const QString text = u"| Item | Value |\n| --- | --- |\n| Total cost | 42 |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // A line of code with a tab in it goes in at the caret, the tab a space.
    const int total = int(text.indexOf(u"Total")) + 5;
    view.select(total, total);
    pasteText(view, u"\tprint(x)"_s);
    QCOMPARE(cells(view), u"Item|Value / Total print(x) cost|42"_s);
    view.binding->undo();
    QCOMPARE(view.text(), text);
    // Over a block of cells, text with tabs is cells.
    view.select(int(text.indexOf(u"Total")), int(text.indexOf(u"42")) + 2);
    pasteText(view, u"x\ty\n"_s);
    QCOMPARE(cells(view), u"Item|Value / x|y"_s);
    view.binding->undo();
    QCOMPARE(view.text(), text);
    // From a spreadsheet, which says so, they are cells at the caret too.
    view.select(total, total);
    auto *sheet = new QMimeData;
    sheet->setText(u"p\tq\n"_s);
    sheet->setData(u"application/x-libreoffice-tsvc"_s, "p\tq\n");
    QGuiApplication::clipboard()->setMimeData(sheet);
    view.binding->paste();
    QCOMPARE(cells(view), u"Item|Value / p|q"_s);
}

void blockCommandsAreRefusedOverABlockOfCells()
{
    const QString text = u"| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    view.select(int(text.indexOf(u"A")), int(text.indexOf(u"two")) + 3);
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    for (const QString &command : {u"heading1"_s, u"quote"_s, u"bulletList"_s, u"codeBlock"_s, u"footnote"_s,
                                   u"thematicBreak"_s, u"displayMath"_s})
        QVERIFY2(!view.binding->format(command), qPrintable(command));
    QCOMPARE(refused.count(), 7);
    QCOMPARE(view.text(), text);
    QVERIFY(!view.binding->insertTable(2, 2));
    QCOMPARE(view.text(), text);
    // Inline ones still format each cell.
    QVERIFY(view.binding->format(u"code"_s));
    QCOMPARE(cells(view), u"`A`|`B` / `one`|`two`"_s);
}

void aTablePastedBelowAGridIsATableOfItsOwn()
{
    const QString text = u"Intro\n\n| A | B |\n| --- | --- |\n| one | two |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int blank = int(text.indexOf(u"\n\nAfter")) + 1;
    view.select(blank, blank);
    pasteText(view, u"| X | Y |\n|---|---|\n| 1 | 2 |\n"_s);
    QCOMPARE(cells(view), u"A|B / one|two"_s);
    QVERIFY2(view.text().contains(u"| one | two |\n\n| X | Y |\n|---|---|\n| 1 | 2 |\n"_s), qPrintable(view.text()));
    const int after = int(view.text().indexOf(u"After"));
    view.select(after, after);
    QVERIFY(view.grids(2));
}

void replaceGoesOnThroughTheMatchesOfATable()
{
    const QString text = u"| A |\n| --- |\n| foo |\n| foo |\n| foo |\n\nAfter foo"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    DocumentFind find;
    find.setDocument(view.document);
    find.setBinding(view.binding);
    find.setActive(true);
    find.setCaseSensitive(true);
    find.setPattern(u"foo"_s);
    QTRY_COMPARE(find.count(), 4);
    const int first = int(text.indexOf(u"foo"));
    find.setSelection(first, first + 3);
    QCOMPARE(find.current(), 0);
    // Each replacement widens the column, the table written anew all
    // through; the next match is the next row's all the same.
    for (int row = 2; row <= 3; ++row) {
        find.replace(u"longer text"_s);
        QVERIFY(find.current() >= 0);
        const SourceRange next = find.matches().at(find.current());
        QCOMPARE(view.text().mid(next.start, next.length), u"foo"_s);
        QCOMPARE(view.grid()->placeOf(next.start).row, row);
    }
    find.replace(u"longer text"_s);
    QCOMPARE(cells(view), u"A / longer text / longer text / longer text"_s);
    const SourceRange prose = find.matches().at(find.current());
    QVERIFY(prose.start > view.grid()->end());
}

void aSelectionFromACellOutOfTheTableTakesItWhole()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Down past the last row, Shift held: the whole table and the line after it.
    const int east = int(weather.indexOf(u"East"));
    view.select(east, east);
    view.key(Qt::Key_Down, Qt::ShiftModifier);
    QCOMPARE(view.anchor(), view.grid()->start());
    QVERIFY(view.cursor() > view.grid()->end());
    view.key(Qt::Key_Delete);
    QVERIFY2(!view.text().contains(u"| Time"_s), qPrintable(view.text()));
    view.binding->undo();
    QCOMPARE(view.text(), weather);
    QVERIFY(view.grids(1));
    // Up past the header, the other way.
    const int time = int(weather.indexOf(u"Time"));
    view.select(time, time);
    view.key(Qt::Key_Up, Qt::ShiftModifier);
    QCOMPARE(view.anchor(), view.grid()->end());
    QVERIFY(view.cursor() < view.grid()->start());
    // From a cell to the text below, as a Shift-click makes it.
    const int after = int(weather.indexOf(u"After"));
    view.select(east, after);
    QCOMPARE(view.anchor(), view.grid()->start());
    QCOMPARE(view.cursor(), after);
}

void undoInACellLeavesTheCaretWhereItTyped()
{
    QString text = u"| A | B |\n| --- | --- |\n"_s;
    for (int row = 1; row <= 30; ++row) text += u"| r%1 | x |\n"_s.arg(row);
    text += u"\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int r2 = int(text.indexOf(u"| r2 |")) + 2;
    view.select(r2 + 2, r2 + 2);
    view.type(u"xyz"_s);
    QCOMPARE(view.cell(), u"r2xyz"_s);
    view.select(int(view.text().indexOf(u"r30")), int(view.text().indexOf(u"r30")));
    // Undo takes the caret back to the cell it took the typing from, not
    // to the last row the column's widening rewrote.
    view.binding->undo();
    QCOMPARE(view.text(), text);
    QCOMPARE(view.cell(), u"r2"_s);
    QCOMPARE(view.cursor(), r2 + 2);
    view.binding->redo();
    QCOMPARE(view.cell(), u"r2xyz"_s);
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"r2xyz")) + 5);
}

void upAndDownKeepToTheColumnClickedIn()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    // Down from the blank line above enters the first column.
    const int blank = int(weather.indexOf(u"\n\n| Time")) + 1;
    view.select(blank, blank);
    view.key(Qt::Key_Down);
    QCOMPARE(view.cell(), u"Time"_s);
    // A click in the third column, then Down: the third column's cell below.
    const int air = int(weather.indexOf(u"Air"));
    view.select(air, air);
    view.key(Qt::Key_Down);
    QCOMPARE(view.cell(), u"7.8"_s);
    view.key(Qt::Key_Down);
    QCOMPARE(view.cell(), u"12"_s);
}

void aLineBreakPastedInACellLeavesTheCaretAfterIt()
{
    const QString text = u"| A | B |\n| --- | --- |\n| AB | next |\n\nAfter"_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int ab = int(text.indexOf(u"AB")) + 2;
    view.select(ab, ab);
    QGuiApplication::clipboard()->setText(u"x\n\ny"_s);
    view.binding->paste();
    QCOMPARE(view.cell(), u"ABx y"_s);
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"ABx y")) + 5);
    view.type(u"z"_s);
    QCOMPARE(cells(view), u"A|B / ABx yz|next"_s);
    // A CRLF the same.
    QGuiApplication::clipboard()->setText(u"1\r\n2"_s);
    view.binding->paste();
    QCOMPARE(cells(view), u"A|B / ABx yz1 2|next"_s);
    QCOMPARE(view.cursor(), int(view.text().indexOf(u"ABx yz1 2")) + 9);
}

void replaceAllMakesTheReplacementsTablesCanTake()
{
    // Taking the backslash before the backtick would join the cells; the
    // backslash in the prose goes.
    const QString text = u"| A | B |\n| --- | --- |\n| \\`aaa | bbb` |\n\nSome \\ prose."_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    DocumentFind find;
    find.setDocument(view.document);
    find.setBinding(view.binding);
    find.setActive(true);
    find.setPattern(u"\\"_s);
    QTRY_COMPARE(find.count(), 2);
    QSignalSpy replaced(&find, &DocumentFind::replacedAll);
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    find.replaceAll(QString());
    QTRY_COMPARE(replaced.count(), 1);
    QCOMPARE(replaced.first().first().toInt(), 1);
    QCOMPARE(refused.count(), 1);
    QCOMPARE(view.text(), u"| A | B |\n| --- | --- |\n| \\`aaa | bbb` |\n\nSome  prose."_s);
}

namespace {

void compareSaved(GridEditor &view, const QString &expected)
{
    view.store->save(view.document);
    QTRY_VERIFY(!view.document->differsFromFile());
    QCOMPARE(readFile(view.document->path()), expected.toUtf8());
}

} // namespace

void backwardCutAboveATableLeavesItWhole_data()
{
    QTest::addColumn<QString>("keys");
    QTest::addColumn<bool>("reverse");
    QTest::addColumn<bool>("insideCell");
    QTest::addColumn<QString>("newline");
    QTest::newRow("vim-cut") << u"vk"_s << true << false << u"\n"_s;
    QTest::newRow("vim-delete") << u"vkd"_s << true << false << u"\n"_s;
    QTest::newRow("reverse-selection") << QString() << true << false << u"\n"_s;
    QTest::newRow("forward-selection") << QString() << false << false << u"\n"_s;
    QTest::newRow("reverse-crlf") << QString() << true << false << u"\r\n"_s;
    QTest::newRow("from-inside-a-cell") << QString() << true << true << u"\n"_s;
}

void backwardCutAboveATableLeavesItWhole()
{
    QFETCH(QString, keys);
    QFETCH(bool, reverse);
    QFETCH(bool, insideCell);
    QFETCH(QString, newline);
    QString text = u"Alpha line\n\nBeta line\n\n| A | B |\n| --- | --- |\n| Blue | Gold |\n\nTail."_s;
    text.replace(u"\n"_s, newline);
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int beta = int(text.indexOf(u"Beta"));
    const int tableStart = view.grid()->start();
    const int high = insideCell ? view.grid()->end() : tableStart;
    if (keys.isEmpty()) {
        const int endpoint = insideCell ? int(text.indexOf(u"Blue")) : tableStart;
        view.select(reverse ? endpoint : beta, reverse ? beta : endpoint);
    } else {
        view.pane->setProperty("vimKeys", true);
        const int blank = tableStart - int(newline.size());
        view.select(blank, blank);
        view.type(keys);
    }
    if (!keys.endsWith(u'd')) {
        view.key(Qt::Key_X, Qt::ControlModifier);
        QCOMPARE(QGuiApplication::clipboard()->text(), text.mid(beta, high - beta));
    }
    const QString expected = text.left(beta) + text.mid(high);
    QCOMPARE(view.text(), expected);
    QVERIFY(view.grids(insideCell ? 0 : 1));
    if (!insideCell) QCOMPARE(cells(view), u"A|B / Blue|Gold"_s);
    compareSaved(view, expected);
    view.binding->undo();
    QCOMPARE(view.text(), text);
    QVERIFY(view.grids(1));
    compareSaved(view, text);
}

void replaceAllKeepsItsQueryWhenItRetries_data()
{
    QTest::addColumn<bool>("append");
    QTest::addColumn<QString>("newPattern");
    QTest::addColumn<QString>("interruption");
    QTest::newRow("edit-and-fields") << true << u"Gold"_s << QString();
    QTest::newRow("regex-and-invalid-fields") << true << u"["_s << QString();
    QTest::newRow("regex-then-literal") << true << u"gold"_s << QString();
    QTest::newRow("edit-only") << true << QString() << QString();
    QTest::newRow("fields-only") << false << u"Gold"_s << QString();
    QTest::newRow("grid-appears") << false << QString() << u"project"_s;
    QTest::newRow("switch-through-read-only-view") << false << QString() << u"switch"_s;
    QTest::newRow("reopen-find-bar") << false << QString() << u"reopen"_s;
}

void replaceAllKeepsItsQueryWhenItRetries()
{
    QFETCH(bool, append);
    QFETCH(QString, newPattern);
    QFETCH(QString, interruption);
    const QString text =
        u"| Color name | Other |\n| ---------- | ----- |\n| Blue       | Gold  |\n\nTail."_s;
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    DocumentFind find;
    find.setDocument(view.document);
    find.setBinding(view.binding);
    find.setActive(true);
    const bool capture = newPattern == u"["_s || newPattern == u"gold"_s;
    const bool project = interruption == u"project"_s;
    if (project) {
        view.pane->setProperty("tableGrids", false);
        QVERIFY(view.grids(0));
    }
    find.setRegex(capture || project);
    find.setPattern(capture ? u"(Bl)ue"_s : project ? u"Blue|-{5}"_s : u"Blue"_s);
    QTRY_COMPARE(find.count(), project ? 4 : 1);
    QSignalSpy replaced(&find, &DocumentFind::replacedAll);
    const QString replacement = capture ? u"$1-ed"_s : u"Edited"_s;
    if (interruption == u"switch"_s || interruption == u"reopen"_s) {
        // Start a new command immediately after returning to this document.
        // Only its completion may make an edit.
        find.replaceAll(u"Stale"_s);
        if (interruption == u"switch"_s) {
            const QString path = view.root.filePath(u"readonly.md"_s);
            writeFile(path, QByteArray("Blue \xff\n"));
            Document *readOnly = view.store->open(path);
            QVERIFY(readOnly);
            const QString before = readOnly->buffer()->text();
            view.pane->setProperty("document", QVariant::fromValue(readOnly));
            find.setDocument(readOnly);
            QVERIFY(!find.canReplace());
            find.replaceAll(u"Refused"_s);
            QCOMPARE(readOnly->buffer()->text(), before);
            view.pane->setProperty("document", QVariant::fromValue(view.document));
            find.setDocument(view.document);
        } else {
            find.setActive(false);
            find.setActive(true);
        }
    }
    find.replaceAll(replacement);
    // All changes happen in this event-loop turn, ahead of worker delivery.
    QString newer = text;
    if (append) {
        view.document->buffer()->replace({int(text.size()), 0}, u"!"_s);
        newer += u'!';
    }
    if (!newPattern.isEmpty()) {
        find.setPattern(newPattern);
        if (newPattern == u"gold"_s) find.setRegex(false);
        if (!capture) find.setCaseSensitive(true);
    }
    if (project) {
        view.pane->setProperty("tableGrids", true);
        QCOMPARE(view.binding->tables()->grids().size(), 1);
    }
    QTRY_COMPARE(replaced.count(), 1);
    QCOMPARE(replaced.first().first().toInt(), 1);
    QString expected = newer;
    expected.replace(u"Blue       "_s, (capture ? u"Bl-ed"_s : replacement).leftJustified(11));
    QCOMPARE(view.text(), expected);
    QVERIFY(view.grids(1));
    compareSaved(view, expected);
    view.binding->undo();
    QCOMPARE(view.text(), newer);
    compareSaved(view, newer);
}

void replaceAllKeepsSafeEditsInTheSameTable_data()
{
    QTest::addColumn<QString>("pattern");
    QTest::addColumn<QString>("replacement");
    QTest::addColumn<QStringList>("before");
    QTest::addColumn<QStringList>("after");
    QTest::addColumn<bool>("mixed");
    QTest::addColumn<int>("matches");
    QTest::addColumn<int>("accepted");
    const QString pattern = uR"(\\|SAFE)"_s;
    const QString opening = uR"(\`aaa)"_s;
    const QString closing = u"bbb`"_s;
    QTest::newRow("other-row") << pattern << QString()
        << QStringList{opening, closing, u"SAFE"_s, u"still"_s}
        << QStringList{opening, closing, QString(), u"still"_s} << false << 2 << 1;
    QTest::newRow("same-row-other-cell") << pattern << QString()
        << QStringList{opening, closing + u"SAFE"_s} << QStringList{opening, closing} << false << 2 << 1;
    QTest::newRow("same-cell") << pattern << QString()
        << QStringList{opening + u"SAFE"_s, closing} << QStringList{opening, closing} << false << 2 << 1;
    QTest::newRow("still-replaces-prose-and-a-second-table") << pattern << QString()
        << QStringList{opening + u"SAFE"_s, closing} << QStringList{opening, closing} << true << 4 << 3;
    QTest::newRow("all-refused") << pattern << QString()
        << QStringList{opening, closing} << QStringList{opening, closing} << false << 1 << 0;
    QTest::newRow("cooperating-edits") << uR"(\\|(?<=bbb)`)"_s << QString()
        << QStringList{opening, closing} << QStringList{u"`aaa"_s, u"bbb"_s} << false << 2 << 2;
    QTest::newRow("no-op") << u"aaa"_s << u"aaa"_s
        << QStringList{opening, closing} << QStringList{opening, closing} << false << 1 << 1;
}

void replaceAllKeepsSafeEditsInTheSameTable()
{
    QFETCH(QString, pattern);
    QFETCH(QString, replacement);
    QFETCH(QStringList, before);
    QFETCH(QStringList, after);
    QFETCH(bool, mixed);
    QFETCH(int, matches);
    QFETCH(int, accepted);
    const auto table = [](const QStringList &cells) {
        QString source = u"| Opening cell | Closing cell |\n| ------------ | ------------ |\n"_s;
        for (qsizetype i = 0; i < cells.size(); i += 2)
            source += u"| %1 | %2 |\n"_s.arg(cells[i], -12).arg(cells[i + 1], -12);
        return source;
    };
    const QString text = table(before) + (mixed ? u"\nSAFE in prose.\n\n"_s + table({u"SAFE"_s, u"elsewhere"_s}) : QString());
    const QString expected = table(after) + (mixed ? u"\n in prose.\n\n"_s + table({QString(), u"elsewhere"_s}) : QString());
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(mixed ? 2 : 1));
    DocumentFind find;
    find.setDocument(view.document);
    find.setBinding(view.binding);
    find.setActive(true);
    find.setRegex(true);
    find.setPattern(pattern);
    QTRY_COMPARE(find.count(), matches);
    QSignalSpy replaced(&find, &DocumentFind::replacedAll);
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    find.replaceAll(replacement);
    QTRY_COMPARE(replaced.count(), 1);
    QCOMPARE(replaced.first().first().toInt(), accepted);
    QCOMPARE(refused.count(), int(accepted < matches));
    if (!refused.isEmpty()) QVERIFY(!refused.first().first().toString().isEmpty());
    QCOMPARE(view.text(), expected);
    QVERIFY(view.grids(mixed ? 2 : 1));
    QCOMPARE(view.grid()->rowCount(), int(before.size() / 2 + 1));
    for (qsizetype i = 0; i < after.size(); ++i) {
        const MdTable::Cell &cell = view.grid()->cellAt({int(i / 2 + 1), int(i % 2)});
        QCOMPARE(view.text().mid(cell.start, cell.end - cell.start), after[i]);
    }
    compareSaved(view, expected);
    QCOMPARE(view.document->buffer()->undoDepth(), expected == text ? 0 : 1);
    view.binding->undo();
    QCOMPARE(view.text(), text);
    compareSaved(view, text);
}

void backtickRunsCloseInsideTheirOwnCell_data()
{
    QTest::addColumn<QString>("content");
    QTest::addColumn<int>("opening");
    QTest::addColumn<int>("closing");
    QTest::addColumn<QString>("expected");
    QTest::addColumn<QString>("code");
    QTest::addColumn<bool>("standard");
    QTest::addColumn<QString>("neighborTicks");
    QTest::newRow("one") << u"aaa"_s << 1 << 1 << u"`aaa`"_s << u"aaa"_s << false << u"`"_s;
    QTest::newRow("two") << u"aaa"_s << 2 << 2 << u"``aaa``"_s << u"aaa"_s << false << u"``"_s;
    QTest::newRow("three") << u"aaa"_s << 3 << 3 << u"```aaa```"_s << u"aaa"_s << false << u"```"_s;
    QTest::newRow("earlier-closing-tick-protected")
        << u"aaa"_s << 2 << 2 << u"``aaa``"_s << u"aaa"_s << false << u"`ccc``"_s;
    QTest::newRow("mismatched") << u"aaa"_s << 2 << 1 << uR"(\`\`aaa`)"_s << QString() << false << u"``"_s;
    QTest::newRow("authored-escape-and-backslash")
        << uR"(\\\`aaa)"_s << 0 << 1 << uR"(\\\`aaa\`)"_s << QString() << false << u"`"_s;
    QTest::newRow("pipe-in-code")
        << uR"(aaa\|b)"_s << 2 << 2 << uR"(``aaa\|b``)"_s << u"aaa|b"_s << false << u"``"_s;
    QTest::newRow("standard") << u"aaa"_s << 3 << 3 << u"```aaa```"_s << u"aaa"_s << true << u"```"_s;
}

void backtickRunsCloseInsideTheirOwnCell()
{
    QFETCH(QString, content);
    QFETCH(int, opening);
    QFETCH(int, closing);
    QFETCH(QString, expected);
    QFETCH(QString, code);
    QFETCH(bool, standard);
    QFETCH(QString, neighborTicks);
    const QString neighbor = u"bbb"_s + neighborTicks;
    const QString text = u"| A | B |\n| --- | --- |\n| %1 | %2 |\n\nAfter."_s.arg(content, neighbor);
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    if (standard) {
        Dialect::Layers layers;
        layers.document.profile = md::Profile::Standard;
        view.document->setSavedDialect(layers);
    }
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    const auto checkNeighbor = [&] {
        QCOMPARE(view.grid()->cellCount(1), 2);
        const MdTable::Cell &cell = view.grid()->cellAt({1, 1});
        QCOMPARE(view.text().mid(cell.start, cell.end - cell.start), neighbor);
    };
    const int start = int(text.indexOf(content));
    view.select(start, start);
    for (int i = 0; i < opening; ++i) {
        sendKey(&view.window, u'`');
        checkNeighbor();
    }
    const QString opened = view.text();
    view.key(Qt::Key_End);
    for (int i = 0; i < closing; ++i) {
        sendKey(&view.window, u'`');
        checkNeighbor();
        if (i == 0 && neighborTicks == u"`ccc``"_s) QVERIFY(view.cell().endsWith(uR"(\`)"_s));
    }
    QCOMPARE(refused.count(), 0);
    QCOMPARE(view.cell(), expected);
    QCOMPARE(view.cursor(), int(view.text().indexOf(expected) + expected.size()));
    const md::Document parsed = md::parse(view.text(), view.document->markdownOptions());
    QCOMPARE(parsed.root.children.size(), 2u);
    const md::Node &table = parsed.root.children.front();
    QCOMPARE(table.type, md::NodeType::Table);
    QCOMPARE(table.children.size(), 2u);
    QCOMPARE(table.children.at(1).children.size(), 2u);
    const md::Node &cell = table.children.at(1).children.front();
    QStringList spans;
    for (const md::Node &inlineNode : cell.children)
        if (inlineNode.type == md::NodeType::Code) spans.append(inlineNode.text);
    QCOMPARE(spans, code.isEmpty() ? QStringList() : QStringList{code});
    compareSaved(view, view.text());
    if (standard) {
        // Standard requires the pipe to be escaped even within the span.
        const QString closed = view.text();
        const int at = int(closed.indexOf(u"aaa")) + 1;
        view.select(at, at);
        view.type(u"|"_s);
        QCOMPARE(view.cell(), uR"(```a\|aa```)"_s);
        QCOMPARE(view.cursor(), int(view.text().indexOf(uR"(a\|aa)"_s)) + 3);
        checkNeighbor();
        compareSaved(view, view.text());
        view.binding->undo();
        QCOMPARE(view.text(), closed);
    }
    view.binding->undo();
    QCOMPARE(view.text(), opened);
    checkNeighbor();
    compareSaved(view, opened);
}

void writingBelowAGridKeepsTheNextParagraphSeparate_data()
{
    QTest::addColumn<QString>("separator");
    QTest::addColumn<QString>("input");
    QTest::addColumn<bool>("enter");
    QTest::addColumn<bool>("crlf");
    QTest::addColumn<bool>("tail");
    QTest::newRow("enter-from-empty-row") << u"\n\n"_s << u"Below"_s << true << false << true;
    QTest::newRow("directly-on-the-blank-line") << u"\n\n"_s << u"Below"_s << false << false << true;
    QTest::newRow("extra-blank") << u"\n\n\n"_s << u"Below"_s << false << false << true;
    QTest::newRow("whitespace-crlf") << u"\n \t\n"_s << u"Below"_s << false << true << true;
    QTest::newRow("paste-with-a-final-break") << u"\n\n"_s << u"Below\n"_s << false << false << true;
    QTest::newRow("eof") << QString() << u"Below"_s << false << false << false;
}

void writingBelowAGridKeepsTheNextParagraphSeparate()
{
    QFETCH(QString, separator);
    QFETCH(QString, input);
    QFETCH(bool, enter);
    QFETCH(bool, crlf);
    QFETCH(bool, tail);
    const QString table = u"| A   | B   |\n| --- | --- |\n| one | two |"_s;
    const QString emptyRow = u"\n|     |     |"_s;
    QString text = table + (enter ? emptyRow : QString()) + separator + (tail ? u"After text."_s : QString());
    if (crlf) text.replace(u"\n"_s, u"\r\n"_s);
    GridEditor view(text);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const QString newline = view.document->newline();
    const int blank = view.grid()->end() + (separator.isEmpty() ? 0 : int(newline.size()));
    if (enter) {
        const int start = view.grid()->cellAt({2, 0}).start;
        view.select(start, start);
        view.key(Qt::Key_Return);
        QCOMPARE(view.grid()->rowCount(), 2);
    } else if (tail) {
        view.select(blank, blank);
    } else {
        const int start = int(text.indexOf(u"two"));
        view.select(start, start);
        view.key(Qt::Key_Down);
    }
    const QString before = view.text();
    if (input.endsWith(u'\n')) pasteText(view, input);
    else view.type(input);
    QString expected = table + u"\n\n"_s + input;
    if (tail) {
        if (!input.endsWith(u'\n') && separator != u"\n\n\n"_s) expected += u'\n';
        expected += separator.mid(1);
        expected += u"After text."_s;
    }
    if (crlf) expected.replace(u"\n"_s, u"\r\n"_s);
    QCOMPARE(view.text(), expected);
    QCOMPARE(view.cursor(), int(expected.indexOf(u"Below") + input.size()));
    const md::Document parsed = md::parse(view.text(), view.document->markdownOptions());
    QCOMPARE(parsed.root.children.size(), tail ? 3 : 2);
    QCOMPARE(parsed.root.children[0].type, md::NodeType::Table);
    QCOMPARE(parsed.root.children[1].type, md::NodeType::Paragraph);
    if (tail) QCOMPARE(parsed.root.children[2].type, md::NodeType::Paragraph);
    compareSaved(view, expected);
    view.binding->undo();
    QCOMPARE(view.text(), before);
    compareSaved(view, before);
    if (enter) {
        view.binding->undo();
        QCOMPARE(view.text(), text);
        QVERIFY(view.grids(1));
        QCOMPARE(view.grid()->rowCount(), 3);
        compareSaved(view, text);
    }
}

namespace {

const QString clipboardTable = u"| Color | Name |\n| --- | --- |\n| Blue | First |\n| Gold | Second |\n\nAfter."_s;
const QString tsvMime = u"text/tab-separated-values"_s;
const QString parchCellsMime = u"application/x-parch-table-cells"_s;

void pasteUndo(GridEditor &view, const QString &original)
{
    compareSaved(view, view.text());
    QCOMPARE(view.document->buffer()->undoDepth(), 1);
    view.binding->undo();
    QCOMPARE(view.text(), original);
    compareSaved(view, original);
}

} // namespace

void plainPasteFillsEverySelectedCell_data()
{
    QTest::addColumn<QString>("selection");
    QTest::addColumn<QString>("header");
    QTest::addColumn<QString>("first");
    QTest::addColumn<QString>("second");
    QTest::newRow("row-handle") << u"row"_s << u"Color|Name"_s << u"Done|Done"_s << u"Gold|Second"_s;
    QTest::newRow("keyboard-rectangle") << u"keyboard"_s << u"Color|Name"_s << u"Done|Done"_s << u"Done|Done"_s;
    QTest::newRow("column-handle") << u"column"_s << u"Done|Name"_s << u"Done|First"_s << u"Done|Second"_s;
    QTest::newRow("paste-plain-text") << u"plain"_s << u"Color|Name"_s << u"Done|Done"_s << u"Gold|Second"_s;
    QTest::newRow("merged-row") << u"merged"_s << u"Color|Name"_s << u"Done"_s << u"Gold|Second"_s;
    QTest::newRow("typing") << u"typing"_s << u"Color|Name"_s << u"Done|"_s << u"Gold|Second"_s;
    QTest::newRow("one-caret") << u"caret"_s << u"Color|Name"_s << u"BlDoneue|First"_s << u"Gold|Second"_s;
    QTest::newRow("empty-clipboard") << u"empty"_s << u"Color|Name"_s << u"Blue|First"_s << u"Gold|Second"_s;
    QTest::newRow("rich-prose") << u"rich"_s << u"Color|Name"_s << u"**Done**|"_s << u"Gold|Second"_s;
}

void plainPasteFillsEverySelectedCell()
{
    QFETCH(QString, selection);
    QFETCH(QString, header);
    QFETCH(QString, first);
    QFETCH(QString, second);
    QString original = clipboardTable;
    const bool merged = selection == u"merged";
    if (merged) original.replace(u"| Blue | First |"_s, u"| Blue ||"_s);
    GridEditor view(original);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const MdTable::Cell blue = view.grid()->cellAt({1, 0});
    if (selection == u"caret") view.select(blue.start + 2, blue.start + 2);
    else if (selection == u"keyboard") {
        view.select(blue.end, blue.end);
        view.key(Qt::Key_Right, Qt::ShiftModifier);
        view.key(Qt::Key_Down, Qt::ShiftModifier);
    } else if (selection == u"column") view.binding->tables()->selectColumn(view.grid(), 0, false);
    else view.binding->tables()->selectRow(view.grid(), 1, false);

    if (selection == u"typing") view.type(u"Done"_s);
    else if (selection == u"rich") pasteText(view, u"Done\tOther"_s, u"<b>Done</b>"_s);
    else if (selection == u"empty") {
        auto *mime = new QMimeData;
        mime->setData(u"application/octet-stream"_s, "data");
        QGuiApplication::clipboard()->setMimeData(mime);
        view.binding->paste();
    } else {
        QGuiApplication::clipboard()->setText(u"Done"_s);
        if (selection == u"plain") view.key(Qt::Key_V, Qt::ControlModifier | Qt::ShiftModifier);
        else view.key(Qt::Key_V, Qt::ControlModifier);
    }
    QCOMPARE(view.grid()->rowTexts(0).join(u'|'), header);
    QCOMPARE(view.grid()->rowTexts(1).join(u'|'), first);
    QCOMPARE(view.grid()->rowTexts(2).join(u'|'), second);
    if (merged) QCOMPARE(view.grid()->cellAt({1, 0}).span, 2);
    if (selection == u"empty") {
        QCOMPARE(view.text(), original);
        QVERIFY(!view.document->buffer()->canUndo());
    } else {
        pasteUndo(view, original);
    }
}

void typedTsvPastesItsRowsAndColumns_data()
{
    QTest::addColumn<QString>("format");
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<QString>("plain");
    QTest::addColumn<QStringList>("rows");
    QTest::addColumn<int>("column");
    QTest::newRow("single-column-with-plain") << tsvMime << QByteArray("11\n22") << u"11\n22"_s
        << QStringList{u"11|First"_s, u"22|Second"_s} << 0;
    QTest::newRow("single-column") << tsvMime << QByteArray("11\n22\n") << QString()
        << QStringList{u"11|First"_s, u"22|Second"_s} << 0;
    QTest::newRow("two-by-two") << tsvMime << QByteArray("11\t12\n21\t22\n") << QString()
        << QStringList{u"11|12"_s, u"21|22"_s} << 0;
    QTest::newRow("explicit-sheet-beats-plain") << tsvMime << QByteArray("11\t12\n21\t22\n") << u"ignore\tme"_s
        << QStringList{u"11|12"_s, u"21|22"_s} << 0;
    QTest::newRow("utf8-crlf-quoted-fields")
        << tsvMime << u"\uFEFF\"é\t一\"\t\"two\r\nlines\"\r\n\t\"He said \"\"Hi\"\"\"\r\n"_s.toUtf8() << QString()
        << QStringList{u"é\t一|two lines"_s, u"|He said \"Hi\""_s} << 0;
    QTest::newRow("libreoffice-companion") << u"application/x-libreoffice-tsvc"_s << QByteArray("\xff") << u"11\n22"_s
        << QStringList{u"11|First"_s, u"22|Second"_s} << 0;
    QTest::newRow("actual-empty-last-row") << tsvMime << QByteArray("11\n\n") << QString()
        << QStringList{u"11|First"_s, u"|Second"_s} << 0;
    QTest::newRow("invalid-payload") << tsvMime << QByteArray("\xff") << u"X"_s
        << QStringList{u"XBlue|First"_s, u"Gold|Second"_s} << 0;
    QTest::newRow("truncated-utf8") << tsvMime << QByteArray("12\xd0") << u"X"_s
        << QStringList{u"XBlue|First"_s, u"Gold|Second"_s} << 0;
    QTest::newRow("unclosed-quote") << tsvMime << QByteArray("\"open") << u"X"_s
        << QStringList{u"XBlue|First"_s, u"Gold|Second"_s} << 0;
    const QStringList wide(MdTable::MaxColumns + 1, u"value"_s);
    QTest::newRow("too-many-columns") << tsvMime << wide.join(u'\t').toUtf8() << QString() << QStringList() << 0;
    const QStringList atLimit(MdTable::MaxColumns, u"value"_s);
    QTest::newRow("would-grow-past-column-limit") << tsvMime << atLimit.join(u'\t').toUtf8()
        << QString() << QStringList() << 1;
}

void typedTsvPastesItsRowsAndColumns()
{
    QFETCH(QString, format);
    QFETCH(QByteArray, payload);
    QFETCH(QString, plain);
    QFETCH(QStringList, rows);
    QFETCH(int, column);
    GridEditor view(clipboardTable);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int start = view.grid()->cellAt({1, column}).start;
    view.select(start, start);
    auto *mime = new QMimeData;
    mime->setData(format, payload);
    if (!plain.isNull()) mime->setText(plain);
    QGuiApplication::clipboard()->setMimeData(mime);
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    view.binding->paste();
    if (rows.isEmpty()) {
        QCOMPARE(refused.count(), 1);
        QCOMPARE(view.text(), clipboardTable);
        QVERIFY(!view.document->buffer()->canUndo());
        compareSaved(view, clipboardTable);
        return;
    }
    QCOMPARE(view.grid()->rowCount(), 3);
    QCOMPARE(view.grid()->rowTexts(0), (QStringList{u"Color"_s, u"Name"_s}));
    QCOMPARE(view.grid()->rowTexts(1).join(u'|'), rows[0]);
    QCOMPARE(view.grid()->rowTexts(2).join(u'|'), rows[1]);
    pasteUndo(view, clipboardTable);
}

void htmlRowspanKeepsTheNextCellInItsColumn_data()
{
    QTest::addColumn<QString>("html");
    QTest::addColumn<QString>("rows");
    QTest::addColumn<bool>("mergedDestination");
    QTest::newRow("rowspan")
        << u"<table><tr><td rowspan=2>Day</td><td>Mon</td></tr><tr><td>Tue</td></tr></table>"_s
        << u"Color|Name / Day|Mon / |Tue"_s << false;
    QTest::newRow("both-spans")
        << u"<table><tr><td rowspan=2 colspan=2>Day</td><td>Mon</td></tr><tr><td>Tue</td></tr></table>"_s
        << u"Color|Name| / Day||Mon / ||Tue"_s << false;
    QTest::newRow("zero-through-row-group")
        << u"<table><tbody><tr><td rowspan=0>Day</td><td>Mon</td></tr><tr><td>Tue</td></tr></tbody>"
           "<tbody><tr><td>Wed</td><td>Thu</td></tr></tbody></table>"_s
        << u"Color|Name / Day|Mon / |Tue / Wed|Thu"_s << false;
    QTest::newRow("invalid-spans")
        << u"<table><tr><td rowspan='oops' colspan=-2>Day</td><td>Mon</td></tr>"
           "<tr><td>Tue</td><td>Wed</td></tr></table>"_s
        << u"Color|Name / Day|Mon / Tue|Wed"_s << false;
    QTest::newRow("overlapping-spans")
        << u"<table><tr><td>A</td><td rowspan=2>B</td><td>C</td></tr>"
           "<tr><td colspan=2>D</td><td>E</td></tr></table>"_s
        << u"Color|Name| / A|B|C / D||E"_s << false;
    QTest::newRow("partial-merge-refused")
        << u"<table><tr><td rowspan=2>Day</td></tr><tr></tr></table>"_s
        << QString() << true;
}

void htmlRowspanKeepsTheNextCellInItsColumn()
{
    QFETCH(QString, html);
    QFETCH(QString, rows);
    QFETCH(bool, mergedDestination);
    QString original = clipboardTable;
    if (mergedDestination) original.replace(u"| Blue | First |"_s, u"| Blue ||"_s);
    GridEditor view(original);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int blue = view.grid()->cellAt({1, 0}).start;
    view.select(blue, blue);
    auto *mime = new QMimeData;
    mime->setHtml(html);
    QGuiApplication::clipboard()->setMimeData(mime);
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    view.binding->paste();
    if (mergedDestination) {
        QCOMPARE(refused.count(), 1);
        QCOMPARE(view.text(), original);
        QVERIFY(!view.document->buffer()->canUndo());
        compareSaved(view, original);
    } else {
        QCOMPARE(refused.count(), 0);
        QCOMPARE(cells(view), rows);
        pasteUndo(view, original);
    }
}

void gridPasteUsesTheFirstHtmlTable_data()
{
    QTest::addColumn<QString>("html");
    QTest::addColumn<QString>("plain");
    QTest::addColumn<QString>("value");
    const QString first = u"<table><tr><td>First</td></tr></table>"_s;
    const QString other = u"<p>Between</p><table><tr><td>Other table</td></tr></table>"_s;
    QTest::newRow("two-tables") << first + other << QString() << u"First"_s;
    QTest::newRow("with-tabbed-companion") << first + other << u"First\tWrong\nOther table\tWrong"_s << u"First"_s;
    QTest::newRow("uppercase-and-omitted-tags")
        << u"<TABLE><TBODY><TR><TD>First</TBODY></TABLE>"_s + other << QString() << u"First"_s;
    QTest::newRow("nested")
        << u"<table><tr><td>First <table><tr><td>inner</td></tr></table></td></tr></table>"_s
        << u"First\tWrong\ninner\tWrong"_s << u"First inner"_s;
    QTest::newRow("comment")
        << u"<!-- <table><tr><td>Hidden</td></tr></table> -->"_s + first << QString() << u"First"_s;
    QTest::newRow("empty-first-table")
        << u"<table><tbody><tr></tr></tbody></table>"_s + other << u"X"_s << u"XBlue"_s;
    QTest::newRow("single") << first << QString() << u"First"_s;
}

void gridPasteUsesTheFirstHtmlTable()
{
    QFETCH(QString, html);
    QFETCH(QString, plain);
    QFETCH(QString, value);
    GridEditor view(clipboardTable);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int blue = view.grid()->cellAt({1, 0}).start;
    view.select(blue, blue);
    auto *mime = new QMimeData;
    mime->setHtml(html);
    if (!plain.isNull()) mime->setText(plain);
    QGuiApplication::clipboard()->setMimeData(mime);
    view.binding->paste();
    QCOMPARE(cells(view), u"Color|Name / %1|First / Gold|Second"_s.arg(value));
    pasteUndo(view, clipboardTable);
}

void sortUsesTheVisibleCellText()
{
    const QString original = u"| Key | Extra | Row |\n| --- | --- | --- |\n"
                             "| 20 | | twenty |\n| 1_0 | | underscore |\n| 3 | | three |\n"
                             "| _10_ | | emphasis |\n| [10](https://example.org) | | link |\n"
                             "| `10` | | code |\n| \\_ | | escaped |\n| _ | | marker |\n"
                             "| 1e999 | | overflow |\n| | | blank |\n|  || merged-blank |\n\nAfter."_s;
    GridEditor view(original);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    const int rows = view.grid()->rowCount();
    QCOMPARE(rows, 12);
    QHash<QString, QStringList> byId;
    for (int r = 1; r < rows; ++r) {
        const QStringList cells = view.grid()->rowTexts(r);
        byId.insert(cells.last(), cells);
    }
    for (const bool descending : {false, true}) {
        view.binding->tables()->selectColumn(view.grid(), 0, false);
        QObject *menu = view.pane->findChild<QObject *>(u"tableMenu"_s);
        QVERIFY(menu);
        QVERIFY(menu->setProperty("kind", u"column"_s));
        QVERIFY(QMetaObject::invokeMethod(menu, "open"));
        QTRY_VERIFY(menu->property("opened").toBool());
        auto *item = menu->findChild<QQuickItem *>(descending ? u"sortDescendingEntry"_s : u"sortAscendingEntry"_s);
        QVERIFY(item && item->isVisible() && item->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(item, "triggered"));
        QVERIFY(QMetaObject::invokeMethod(menu, "close"));
        QTRY_VERIFY(!menu->property("visible").toBool());
        QStringList order;
        for (int r = 1; r < rows; ++r) {
            const QStringList cells = view.grid()->rowTexts(r);
            QCOMPARE(cells, byId.value(cells.last()));
            order.append(cells.last());
        }
        QStringList numbers{u"emphasis"_s, u"link"_s, u"code"_s};
        numbers.prepend(descending ? u"twenty"_s : u"three"_s);
        numbers.append(descending ? u"three"_s : u"twenty"_s);
        QCOMPARE(order.first(5), numbers);
        QCOMPARE(order.last(2), (QStringList{u"blank"_s, u"merged-blank"_s}));
        QVERIFY(order.indexOf(u"escaped"_s) < order.indexOf(u"marker"_s));
        QCOMPARE(view.grid()->cellAt({rows - 1, 0}).span, 2);

        const auto scanned = MdTable::scan(original, 0, int(original.size()));
        QVERIFY(scanned);
        MdTable::Model model = MdTable::model(original, *scanned);
        QVERIFY(MdTable::sortRows(model, 0, descending).isEmpty());
        QCOMPARE(view.text(), MdTable::write(model).text + original.mid(scanned->end));
        pasteUndo(view, original);
    }
}

void copiedCellsReadWellAndPasteBackAsMarkdown_data()
{
    QTest::addColumn<QString>("transfer");
    QTest::newRow("private") << u"copy"_s;
    QTest::newRow("cut") << u"cut"_s;
    QTest::newRow("only-public") << u"public"_s;
    QTest::newRow("corrupt-private") << u"corrupt"_s;
    QTest::newRow("unknown-private-version") << u"version"_s;
    QTest::newRow("unsafe-private-cell") << u"unsafe"_s;
}

void copiedCellsReadWellAndPasteBackAsMarkdown()
{
    QFETCH(QString, transfer);
    const QString original = u"| A | B | C |\n| --- | --- | --- |\n"
                             "| Pear<br>two | **big** [x](https://e.com) | E\\|W \"q\" 1_0 |\n\n"
                             "| A | B | C |\n| --- | --- | --- |\n| first | second | third |\n\nAfter."_s;
    GridEditor view(original);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(2));
    view.binding->tables()->selectRow(view.grid(), 1, false);
    if (transfer == u"cut") {
        view.binding->cut();
        QCOMPARE(view.grid()->rowTexts(1), (QStringList{QString(), QString(), QString()}));
        pasteUndo(view, original);
    } else {
        view.binding->copy();
    }
    const QMimeData *mime = QGuiApplication::clipboard()->mimeData();
    QVERIFY(mime);
    const QString tsv = u"\"Pear\ntwo\"\tbig x\t\"E|W \"\"q\"\" 1_0\"\n"_s;
    QCOMPARE(mime->text(), tsv);
    QCOMPARE(mime->data(tsvMime), tsv.toUtf8());
    QVERIFY(mime->hasFormat(parchCellsMime));
    QVERIFY(mime->html().contains(u"Pear<br"));
    QVERIFY(mime->html().contains(u"<strong>big</strong>"));
    QVERIFY(mime->html().contains(u"<a href=\"https://e.com\">x</a>"));
    const bool privateCells = transfer == u"copy" || transfer == u"cut";
    if (!privateCells) {
        auto *publicCells = new QMimeData;
        publicCells->setText(mime->text());
        publicCells->setData(tsvMime, mime->data(tsvMime));
        publicCells->setHtml(mime->html());
        if (transfer == u"corrupt") publicCells->setData(parchCellsMime, "not a rectangle");
        if (transfer == u"version")
            publicCells->setData(parchCellsMime, "parch-table-cells 2\nWrong\tX\tY\n");
        if (transfer == u"unsafe")
            publicCells->setData(parchCellsMime, "parch-table-cells 1\nbad | cell\tx\ty\n");
        QGuiApplication::clipboard()->setMimeData(publicCells);
    }
    const int first = view.grid(1)->cellAt({1, 0}).start;
    view.select(first, first);
    view.binding->paste();
    const QStringList expected = privateCells
        ? QStringList{u"Pear<br>two"_s, u"**big** [x](https://e.com)"_s, u"E\\|W \"q\" 1_0"_s}
        : QStringList{u"Pear two"_s, u"big x"_s, u"E\\|W \"q\" 1_0"_s};
    QCOMPARE(view.grid(1)->rowTexts(1), expected);
    pasteUndo(view, original);
}

void slowPastesLeaveTablesShownAsMarkdownAsWritten()
{
    const QString html = u"<p>First.</p><!--"_s + QString(65536, u' ') + u"-->"_s;
    // A table in a code block is text.
    {
        const QString text = u"Intro\n\n```\n| A | B |\n|---|---|\n| one | two |\n```\n\nAfter"_s;
        GridEditor view(text);
        QVERIFY2(view.ok(), qPrintable(view.error));
        QTest::qWait(200);
        QVERIFY(view.grids(0));
        Attachments attachments(AppPaths::under(view.root.path()));
        view.binding->setAttachments(&attachments);
        const int one = int(text.indexOf(u"one")) + 3;
        view.select(one, one);
        pasteText(view, u"First."_s, html);
        QTRY_VERIFY(view.text().contains(u"First."_s));
        QVERIFY2(view.text().contains(u"| A | B |\n|---|---|\n"_s), qPrintable(view.text()));
        view.binding->setAttachments(nullptr);
    }
    // With grids off, a table stays as its Markdown is written.
    {
        const QString text = u"Intro\n\n| A | B |\n|---|---|\n| one | two |\n\nAfter"_s;
        GridEditor view(text);
        QVERIFY2(view.ok(), qPrintable(view.error));
        view.pane->setProperty("tableGrids", false);
        QTest::qWait(200);
        QVERIFY(view.grids(0));
        Attachments attachments(AppPaths::under(view.root.path()));
        view.binding->setAttachments(&attachments);
        const int one = int(text.indexOf(u"one")) + 3;
        view.select(one, one);
        pasteText(view, u"First."_s, html);
        QTRY_VERIFY(view.text().contains(u"First."_s));
        QVERIFY2(view.text().contains(u"| A | B |\n|---|---|\n"_s), qPrintable(view.text()));
        view.binding->setAttachments(nullptr);
    }
}

void aTableGrowsNoLargerThanAGridHolds()
{
    GridEditor view(u"A paragraph.\n\nAnother one."_s);
    QVERIFY2(view.ok(), qPrintable(view.error));
    // The picker offers no more rows than a grid of so many columns holds.
    view.select(5, 5);
    QMetaObject::invokeMethod(view.pane, "chooseTable");
    QObject *picker = view.pane->findChild<QObject *>(u"tablePicker"_s);
    QVERIFY(picker);
    QTRY_VERIFY(picker->property("opened").toBool());
    picker->setProperty("columns", 64);
    QCOMPARE(picker->property("maxRows").toInt(), 312);
    picker->setProperty("columns", 3);
    picker->setProperty("rows", 1000);
    QCOMPARE(picker->property("maxColumns").toInt(), 20);
    view.key(Qt::Key_Escape);
    QTRY_VERIFY(!picker->property("visible").toBool());
    // A table asked for larger is made as large as a grid holds, and is one.
    QVERIFY(view.binding->insertTable(64, 1000));
    QVERIFY(view.grids(1));
    QCOMPARE(view.grid()->columnCount(), 64);
    QCOMPARE(view.grid()->rowCount(), 312);
    // It takes no more rows, and says why.
    const QString full = view.text();
    QSignalSpy refused(view.binding->tables(), &TableGrids::refused);
    QVERIFY(!view.binding->tables()->appendRow(view.grid()));
    QCOMPARE(refused.count(), 1);
    QCOMPARE(view.text(), full);
}

void escapeWithTextSelectedInACellIsTheViews()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    QSignalSpy escaped(view.pane, SIGNAL(escapePressed()));
    // A word selected in a cell: Escape goes on to the view, as in the text.
    view.binding->tables()->selectWordAt(int(weather.indexOf(u"North")) + 1);
    QCOMPARE(selected(view), u"North"_s);
    view.key(Qt::Key_Escape);
    QCOMPARE(escaped.count(), 1);
    // A block of cells is let go of first.
    view.select(int(weather.indexOf(u"North")), int(weather.indexOf(u"12")) + 1);
    view.key(Qt::Key_Escape);
    QCOMPARE(escaped.count(), 1);
    QCOMPARE(view.anchor(), view.cursor());
}

void theSpellingMenuKeepsOneTableMenu()
{
    GridEditor view(weather);
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.grids(1));
    QObject *menu = view.pane->findChild<QObject *>(u"spellingMenu"_s);
    QVERIFY(menu);
    for (int i = 0; i < 4; ++i)
        QVERIFY(QMetaObject::invokeMethod(menu, "offer", Q_ARG(QVariant, 0), Q_ARG(QVariant, 5), Q_ARG(QVariant, u"Nrth"_s),
                                          Q_ARG(QVariant, QVariant(QStringList{u"North"_s})), Q_ARG(QVariant, true)));
    QTRY_COMPARE(menu->findChildren<QObject *>(u"spellingTableMenu"_s).size(), 1);
}

} // namespace TableGridTests
