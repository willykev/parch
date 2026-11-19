#include "vimtests.h"

#include "apppaths.h"
#include "commandfilter.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "theme.h"
#include "vimmode.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QHash>
#include <QInputMethodEvent>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <memory>

namespace {

// Texts in tests mark the cursor with ‸, before the character it is on, and
// a selection's anchor and cursor with « and », in either order.
struct Marked {
    QString text;
    int anchor = 0;
    int cursor = 0;
};

Marked unmark(const QString &marked)
{
    Marked result;
    int anchor = -1, cursor = -1;
    for (const QChar c : marked) {
        if (c == QChar(0x2038)) anchor = cursor = int(result.text.size());
        else if (c == u'«') anchor = int(result.text.size());
        else if (c == u'»') cursor = int(result.text.size());
        else result.text += c;
    }
    result.anchor = anchor < 0 ? 0 : anchor;
    result.cursor = cursor < 0 ? result.anchor : cursor;
    return result;
}

QString mark(const QString &text, int anchor, int cursor)
{
    QString marked = text;
    if (anchor == cursor) return marked.insert(cursor, QChar(0x2038));
    if (anchor > cursor) return marked.insert(anchor, u'«').insert(cursor, u'»');
    return marked.insert(cursor, u'»').insert(anchor, u'«');
}

// Presses keys as Vim writes them: each character, and <Esc>, <CR>, <BS>,
// <Del>, <Tab>, <Insert>, the arrows, <Home>, <End>, <S-CR> and the Ctrl
// keys <C-r>, <C-[>, <C-v>, <C-c> and <C-z>.
void press(QQuickWindow *window, const QString &keys)
{
    static const QHash<QString, std::pair<Qt::Key, Qt::KeyboardModifiers>> named{
        {QStringLiteral("Esc"), {Qt::Key_Escape, Qt::NoModifier}},
        {QStringLiteral("CR"), {Qt::Key_Return, Qt::NoModifier}},
        {QStringLiteral("S-CR"), {Qt::Key_Return, Qt::ShiftModifier}},
        {QStringLiteral("BS"), {Qt::Key_Backspace, Qt::NoModifier}},
        {QStringLiteral("Del"), {Qt::Key_Delete, Qt::NoModifier}},
        {QStringLiteral("Tab"), {Qt::Key_Tab, Qt::NoModifier}},
        {QStringLiteral("Insert"), {Qt::Key_Insert, Qt::NoModifier}},
        {QStringLiteral("Left"), {Qt::Key_Left, Qt::NoModifier}},
        {QStringLiteral("Right"), {Qt::Key_Right, Qt::NoModifier}},
        {QStringLiteral("Up"), {Qt::Key_Up, Qt::NoModifier}},
        {QStringLiteral("Down"), {Qt::Key_Down, Qt::NoModifier}},
        {QStringLiteral("Home"), {Qt::Key_Home, Qt::NoModifier}},
        {QStringLiteral("End"), {Qt::Key_End, Qt::NoModifier}},
        {QStringLiteral("C-r"), {Qt::Key_R, Qt::ControlModifier}},
        {QStringLiteral("C-["), {Qt::Key_BracketLeft, Qt::ControlModifier}},
        {QStringLiteral("C-v"), {Qt::Key_V, Qt::ControlModifier}},
        {QStringLiteral("C-c"), {Qt::Key_C, Qt::ControlModifier}},
        {QStringLiteral("C-z"), {Qt::Key_Z, Qt::ControlModifier}}};
    for (qsizetype i = 0; i < keys.size(); ++i) {
        if (keys[i] == u'<') {
            const qsizetype close = keys.indexOf(u'>', i + 1);
            const auto found = close > i + 1 ? named.constFind(keys.mid(i + 1, close - i - 1)) : named.cend();
            if (found != named.cend()) {
                activate(window);
                QTest::keyClick(window, found->first, found->second);
                i = close;
                continue;
            }
        }
        sendKey(window, keys[i]);
    }
}

// One editor over one document, as a window shows it, with Vim keys unless
// told otherwise.
struct Editor {
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
    VimMode *vim = nullptr;
    QString error;

    explicit Editor(const QString &text = {}, bool vimKeys = true)
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
        pane->setSize(QSizeF(500, 300));
        window.resize(500, 300);
        pane->setProperty("document", QVariant::fromValue(document));
        pane->setProperty("vimKeys", vimKeys);
        editor = pane->findChild<QQuickItem *>(QStringLiteral("editor"));
        binding = pane->findChild<EditorBinding *>();
        vim = binding ? binding->vim() : nullptr;
        window.show();
        if (editor) editor->forceActiveFocus();
        QCoreApplication::processEvents();
    }
    bool ok() const { return document && pane && editor && binding && vim; }
    QString text() const { return document->buffer()->text(); }
    void select(int anchor, int cursor)
    {
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, binding->sourceToEditor(anchor)),
                                  Q_ARG(int, binding->sourceToEditor(cursor)));
    }
    QString marked() const { return mark(text(), binding->anchorSource(), binding->cursorSource()); }
    void keys(const QString &keys) { press(&window, keys); }
    // A marked text in place, in Normal mode with nothing pending.
    void load(const QString &marked)
    {
        keys(QStringLiteral("<Esc><Esc>"));
        const Marked start = unmark(marked);
        SourceBuffer *buffer = document->buffer();
        buffer->replace({0, buffer->length()}, start.text);
        select(start.anchor, start.cursor);
    }
    QString after(const QString &marked, const QString &keys)
    {
        load(marked);
        this->keys(keys);
        return this->marked();
    }
};

// A text with its cursor, keys, and what they make of it.
struct Case {
    const char *before;
    const char *keys;
    const char *after;
};

// Every case that fails, each on a line of its own.
QString failures(Editor &view, std::initializer_list<Case> cases)
{
    const auto shown = [](QString text) { return text.replace(u'\n', QStringLiteral("⏎")); };
    QString failed;
    for (const Case &c : cases) {
        const QString got = view.after(QString::fromUtf8(c.before), QString::fromUtf8(c.keys));
        if (got != QString::fromUtf8(c.after))
            failed += QStringLiteral("\n  %1  then  %2  gave  %3  not  %4")
                          .arg(shown(QString::fromUtf8(c.before)), shown(QString::fromUtf8(c.keys)), shown(got),
                               shown(QString::fromUtf8(c.after)));
    }
    return failed;
}

} // namespace

namespace VimTests {

void vimKeysAreOffUntilChosenAndKept()
{
    QTemporaryDir root;
    const QString file = root.filePath(QStringLiteral("settings.json"));
    {
        Settings settings(file);
        QVERIFY(!settings.vimKeys());
        QSignalSpy changed(&settings, &Settings::changed);
        settings.setVimKeys(true);
        QVERIFY(settings.vimKeys());
        QCOMPARE(changed.count(), 1);
        settings.setVimKeys(true);
        QCOMPARE(changed.count(), 1);
    }
    QVERIFY(Settings(file).vimKeys());
    // A value that is not true or false is the default, off.
    writeFile(file, QByteArray("{\"vimKeys\": \"yes\"}"));
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("ignoring settings out of range.*vimKeys")));
    QVERIFY(!Settings(file).vimKeys());
}

void undoStepsJoinIntoOneChange()
{
    SourceBuffer buffer(QStringLiteral("one"));
    qint64 now = 0;
    buffer.setClock([&now] { return now; });
    // Steps after a mark become one, whatever their kinds.
    const qsizetype mark = buffer.undoDepth();
    QCOMPARE(mark, 0);
    buffer.replace({3, 0}, QStringLiteral(" two"), EditKind::Other);
    buffer.replace({7, 0}, QStringLiteral("\n"), EditKind::Typing);
    now += 5000;
    buffer.replace({8, 0}, QStringLiteral("three"), EditKind::Typing);
    QCOMPARE(buffer.undoDepth(), 3);
    buffer.joinSince(mark);
    QCOMPARE(buffer.undoDepth(), 1);
    // Typing right after the joined step starts a step of its own.
    buffer.replace({13, 0}, QStringLiteral("!"), EditKind::Typing);
    QCOMPARE(buffer.undoDepth(), 2);
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("one two\nthree"));
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("one"));
    QVERIFY(!buffer.canUndo());
    buffer.redo();
    QCOMPARE(buffer.text(), QStringLiteral("one two\nthree"));

    // Typing never joins a step made before a join, even with no step since.
    SourceBuffer typed(QStringLiteral(""));
    typed.setClock([] { return 0; });
    typed.replace({0, 0}, QStringLiteral("a"), EditKind::Typing);
    typed.joinSince(typed.undoDepth());
    typed.replace({1, 0}, QStringLiteral("b"), EditKind::Typing);
    typed.undo();
    QCOMPARE(typed.text(), QStringLiteral("a"));
    // Nor a single step made since the mark.
    const qsizetype before = typed.undoDepth();
    typed.replace({1, 0}, QStringLiteral("c"), EditKind::Typing);
    typed.joinSince(before);
    typed.replace({2, 0}, QStringLiteral("d"), EditKind::Typing);
    typed.undo();
    QCOMPARE(typed.text(), QStringLiteral("ac"));

    // A save in the middle of the joined steps is no longer a step of its
    // own, so the text is modified; a save at their end stays the saved one.
    SourceBuffer saved(QStringLiteral("x"));
    const qsizetype start = saved.undoDepth();
    saved.replace({1, 0}, QStringLiteral("y"));
    saved.markSaved();
    saved.replace({2, 0}, QStringLiteral("z"));
    saved.joinSince(start);
    QVERIFY(saved.modified());
    saved.markSaved();
    QVERIFY(!saved.modified());
    const qsizetype again = saved.undoDepth();
    saved.replace({3, 0}, QStringLiteral("1"));
    saved.replace({4, 0}, QStringLiteral("2"));
    saved.markSaved();
    saved.joinSince(again);
    QVERIFY(!saved.modified());
    saved.undo();
    QCOMPARE(saved.text(), QStringLiteral("xyz"));
    QVERIFY(saved.modified());

    // Undone past the mark, nothing is joined; redo steps stay after the joined one.
    SourceBuffer undone(QStringLiteral(""));
    undone.replace({0, 0}, QStringLiteral("a"));
    const qsizetype late = undone.undoDepth();
    undone.replace({1, 0}, QStringLiteral("b"));
    undone.undo();
    undone.undo();
    undone.joinSince(late);
    QCOMPARE(undone.undoDepth(), 0);
    undone.redo();
    undone.redo();
    QCOMPARE(undone.text(), QStringLiteral("ab"));
    SourceBuffer redoable(QStringLiteral(""));
    const qsizetype first = redoable.undoDepth();
    redoable.replace({0, 0}, QStringLiteral("a"));
    redoable.replace({1, 0}, QStringLiteral("b"));
    redoable.replace({2, 0}, QStringLiteral("c"));
    redoable.undo();
    redoable.joinSince(first);
    QCOMPARE(redoable.text(), QStringLiteral("ab"));
    redoable.redo();
    QCOMPARE(redoable.text(), QStringLiteral("abc"));
    redoable.undo();
    redoable.undo();
    QCOMPARE(redoable.text(), QString());
}

void motionsMoveAsVimDoes()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    QCOMPARE(view.vim->mode(), VimMode::Normal);
    const QString failed = failures(view, {
        // h and l stay on the line; Backspace and Space go on to the next.
        {"ab‸c", "h", "a‸bc"},
        {"‸abc", "h", "‸abc"},
        {"abc\n‸def", "h", "abc\n‸def"},
        {"abc‸d", "2h", "a‸bcd"},
        {"abc‸d", "9h", "‸abcd"},
        {"‸abc", "l", "a‸bc"},
        {"ab‸c", "l", "ab‸c"},
        {"‸abc", "5l", "ab‸c"},
        {"ab‸c\ndef", " ", "abc\n‸def"},
        {"abc\n‸def", "<BS>", "ab‸c\ndef"},
        {"a‸bc\nxyz", "<Right><Down>", "abc\nxy‸z"},
        // j and k keep the column they started from.
        {"ab‸cd\nxy\nabcd", "j", "abcd\nx‸y\nabcd"},
        {"ab‸cd\nxy\nabcd", "jj", "abcd\nxy\nab‸cd"},
        {"ab‸cd\nxy\nabcd", "2j", "abcd\nxy\nab‸cd"},
        {"abcd\nxy\nab‸cd", "k", "abcd\nx‸y\nabcd"},
        {"a‸bc\nabcde", "$j", "abc\nabcd‸e"},
        {"one\n‸two", "j", "one\n‸two"},
        {"‸one\n  two", "<CR>", "one\n  ‸two"},
        // Within the line.
        {"  ab‸cd", "0", "‸  abcd"},
        {"  ab‸cd", "^", "  ‸abcd"},
        {"  ab‸cd", "$", "  abc‸d"},
        {"a‸b\ncd\nef", "2$", "ab\nc‸d\nef"},
        {"a‸bc", "<End>", "ab‸c"},
        {"ab‸c", "<Home>", "‸abc"},
        // Words, and WORDS of everything but blanks.
        {"‸one two three", "w", "one ‸two three"},
        {"‸one two three", "2w", "one two ‸three"},
        {"‸foo.bar baz", "w", "foo‸.bar baz"},
        {"foo‸.bar baz", "w", "foo.‸bar baz"},
        {"‸foo.bar baz", "W", "foo.bar ‸baz"},
        {"one ‸two\n  three", "w", "one two\n  ‸three"},
        {"‸one\n\ntwo", "w", "one\n‸\ntwo"},
        {"one ‸two", "w", "one tw‸o"},
        {"one two ‸three", "b", "one ‸two three"},
        {"one two th‸ree", "b", "one two ‸three"},
        {"one\n‸two", "b", "‸one\ntwo"},
        {"foo.bar ‸baz", "B", "‸foo.bar baz"},
        {"foo.bar ‸baz", "b", "foo.‸bar baz"},
        {"one two ‸three", "2b", "‸one two three"},
        {"‸one two", "e", "on‸e two"},
        {"on‸e two", "e", "one tw‸o"},
        {"‸foo.bar baz", "E", "foo.ba‸r baz"},
        {"‸foo.bar baz", "e", "fo‸o.bar baz"},
        {"‸one two", "2e", "one tw‸o"},
        {"‸日本語テキスト", "w", "日本語‸テキスト"},
        // Lines of the document.
        {"one\n  two\nth‸ree", "gg", "‸one\n  two\nthree"},
        {"‸one\n  two\nthree", "G", "one\n  two\n‸three"},
        {"‸one\n  two\nthree", "2G", "one\n  ‸two\nthree"},
        {"one\n  two\nth‸ree", "2gg", "one\n  ‸two\nthree"},
        // Paragraphs end at empty lines.
        {"one\n‸two\n\nthree\nfour\n\nfive", "}", "one\ntwo\n‸\nthree\nfour\n\nfive"},
        {"one\n‸two\n\nthree\nfour\n\nfive", "2}", "one\ntwo\n\nthree\nfour\n‸\nfive"},
        {"one\ntwo\n\nthree\nfo‸ur\n\nfive", "{", "one\ntwo\n‸\nthree\nfour\n\nfive"},
        {"one\n\ntw‸o three", "}", "one\n\ntwo thre‸e"},
        // Brackets, and a count's percent of the lines.
        {"‸(a [b] c)", "%", "(a [b] c‸)"},
        {"(a [b] c‸)", "%", "‸(a [b] c)"},
        {"x ‸a(b)", "%", "x a(b‸)"},
        {"a\nb\nc\n‸d", "50%", "a\n‸b\nc\nd"},
        // To a character on the line, and again.
        {"‸a,b,c,d", "f,", "a‸,b,c,d"},
        {"‸a,b,c,d", "f,;", "a,b‸,c,d"},
        {"‸a,b,c,d", "f,;,", "a‸,b,c,d"},
        {"‸a,b,c,d", "2f,", "a,b‸,c,d"},
        {"‸a,b,c", "t,", "‸a,b,c"},
        {"‸a,b,c", "t,;", "a,‸b,c"},
        {"a,b,‸c", "F,", "a,b‸,c"},
        {"a,b,c‸d", "T,", "a,b,‸cd"},
        {"a,b,‸c", "2F,", "a‸,b,c"},
        {"‸abc", "fz", "‸abc"},
    });
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
}

void operatorsActOnMotions()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    const QString failed = failures(view, {
        // d with motions, counts before and after it.
        {"‸one two", "dw", "‸two"},
        {"one ‸two\nthree", "dw", "one‸ \nthree"},
        {"‸a b c d", "d2w", "‸c d"},
        {"‸a b c d", "2dw", "‸c d"},
        {"‸a b c d e", "2d2w", "‸e"},
        {"‸one two", "de", "‸ two"},
        {"one tw‸o", "db", "one ‸o"},
        {"a‸bc", "D", "‸a"},
        {"ab‸c", "d0", "‸c"},
        {"  ab‸c", "d^", "  ‸c"},
        {"‸a,b,c", "df,", "‸b,c"},
        {"‸a,b,c", "dt,", "‸,b,c"},
        {"a,b‸,c", "dF,", "a‸,c"},
        {"a,b‸,c", "dT,", "a,‸,c"},
        {"‸(a b) c", "d%", "‸ c"},
        // Whole lines.
        {"one\n‸two\nthree", "dd", "one\n‸three"},
        {"‸one\ntwo\nthree", "2dd", "‸three"},
        {"one\n‸two", "dd", "‸one"},
        {"one\n‸  two\nthree", "dd", "one\n‸three"},
        {"‸one\ntwo\nthree", "dj", "‸three"},
        {"one\n‸two\nthree", "dk", "‸three"},
        {"one\n‸two\nthree", "dG", "‸one"},
        {"one\ntwo\n‸three", "dgg", "‸"},
        // An exclusive motion to a later line's start: from the indent, lines.
        {"‸one\ntwo\n\nthree", "d}", "‸\nthree"},
        {"o‸ne\ntwo\n\nthree", "d}", "‸o\n\nthree"},
        // c, with cw on a word as ce.
        {"‸one two", "cwX<Esc>", "‸X two"},
        {"one‸  two", "cwX<Esc>", "one‸Xtwo"},
        {"a‸bc", "CX<Esc>", "a‸X"},
        {"one\n  ‸two\nthree", "ccX<Esc>", "one\n‸X\nthree"},
        {"one\n  t‸wo", "SX<Esc>", "one\n‸X"},
        // y, then p and P.
        {"‸one\ntwo", "yyp", "one\n‸one\ntwo"},
        {"‸one\ntwo", "yyP", "‸one\none\ntwo"},
        {"one\n‸two", "yyp", "one\ntwo\n‸two"},
        {"‸one\ntwo", "Yjp", "one\ntwo\n‸one"},
        {"‸one two", "ywP", "one‸ one two"},
        {"‸one two", "ywp", "oone‸ ne two"},
        {"one t‸wo", "yiw", "one ‸two"},
        {"‸ab", "yl3p", "aaa‸ab"},
        // > and <, as Tab indents lists.
        {"‸one\ntwo", ">>", "\t‸one\ntwo"},
        {"‸one\ntwo", "2>>", "\t‸one\n\ttwo"},
        {"‸one\ntwo", ">j", "\t‸one\n\ttwo"},
        {"\t‸one", "<<", "‸one"},
        {"    ‸one", "<<", "‸one"},
        {"- a\n‸- b", ">>", "- a\n  ‸- b"},
    });
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
}

void commandsEditAsVimDoes()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    const QString failed = failures(view, {
        {"a‸bc", "x", "a‸c"},
        {"a‸bc", "2x", "‸a"},
        {"‸abc", "5x", "‸"},
        {"a\n‸\nb", "x", "a\n‸\nb"},
        {"a‸bc", "<Del>", "a‸c"},
        {"ab‸c", "X", "a‸c"},
        {"ab‸c", "5X", "‸c"},
        {"a‸bc", "sX<Esc>", "a‸Xc"},
        {"a‸bc", "2sX<Esc>", "a‸X"},
        {"‸one\n  two", "J", "one‸ two"},
        {"‸one\ntwo\nthree", "3J", "one two‸ three"},
        {"‸one \ntwo", "J", "one ‸two"},
        {"‸one\n)", "J", "one‸)"},
        {"‸one\n\ntwo", "J", "on‸e\ntwo"},
        {"‸one", "J", "‸one"},
        {"a‸bc", "rx", "a‸xc"},
        {"a‸bc", "2rx", "ax‸x"},
        {"a‸bc", "3rx", "a‸bc"},
        {"a‸bc", "r<CR>", "a\n‸c"},
        {"‸abC", "~", "A‸bC"},
        {"‸abC", "3~", "AB‸c"},
        {"‸one\ntwo", "oX<Esc>", "one\n‸X\ntwo"},
        {"‸one\ntwo", "OX<Esc>", "‸X\none\ntwo"},
        {"a‸bc", "iX<Esc>", "a‸Xbc"},
        {"a‸bc", "<Insert>X<Esc>", "a‸Xbc"},
        {"a‸bc", "aX<Esc>", "ab‸Xc"},
        {"  a‸bc", "IX<Esc>", "  ‸Xabc"},
        {"  a‸bc", "AX<Esc>", "  abc‸X"},
        {"‸a", "3iX<Esc>", "XX‸Xa"},
        {"‸a", "2oX<Esc>", "a\nX\n‸X"},
        {"a‸bc", "xu", "a‸bc"},
        {"a‸bc", "xu<C-r>", "a‸c"},
        // Keys that are no command do nothing, and type nothing.
        {"a‸bc", "Qq<Tab>", "a‸bc"},
    });
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
}

void textObjectsTakeTheirText()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    const QString failed = failures(view, {
        {"one t‸wo three", "diw", "one ‸ three"},
        {"one t‸wo three", "daw", "one ‸three"},
        {"one two t‸hree", "daw", "one tw‸o"},
        {"‸one two three", "d2iw", "‸two three"},
        {"one‸   two", "diw", "one‸two"},
        {"a ‸b.c d", "diW", "a ‸ d"},
        {"a ‸b.c d", "daW", "a ‸d"},
        {"one t‸wo three", "ciwX<Esc>", "one ‸X three"},
        {"one\n‸two\n\nthree", "dip", "‸\nthree"},
        {"one\n‸two\n\nthree", "dap", "‸three"},
        {"one\n\n‸three\nfour", "dap", "‸one"},
        {"one\n‸two\n\nthree", "cipX<Esc>", "‸X\n\nthree"},
        {"say \"h‸i\" now", "di\"", "say \"‸\" now"},
        {"‸say \"hi\" now", "di\"", "say \"‸\" now"},
        {"say \"h‸i\" now", "da\"", "say ‸now"},
        {"say \"h‸i\" now", "ci\"X<Esc>", "say \"‸X\" now"},
        {"say \"h‸i\" now", "d2i\"", "say ‸ now"},
        {"a \"b\\\"‸c\" d", "di\"", "a \"‸\" d"},
        {"say 'o‸k' now", "di'", "say '‸' now"},
        {"say `o‸k` now", "da`", "say ‸now"},
        {"f(a, ‸b) x", "di(", "f(‸) x"},
        {"f(a, ‸b) x", "da(", "f‸ x"},
        {"f(a, ‸b) x", "dib", "f(‸) x"},
        {"f(a, ‸b) x", "di)", "f(‸) x"},
        {"f(a, (‸b)) x", "2di(", "f(‸) x"},
        {"f‸(a) x", "di(", "f(‸) x"},
        {"x(‸)", "ci(Y<Esc>", "x(‸Y)"},
        {"{\n    ‸a\n}", "di{", "{\n‸}"},
        {"{\n    ‸a\n}", "diB", "{\n‸}"},
        {"x[a ‸b]", "di[", "x[‸]"},
        {"x[a ‸b]", "da[", "‸x"},
        {"x[a ‸b]", "di]", "x[‸]"},
        {"‸abc", "di(", "‸abc"},
    });
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
}

void visualModeSelectsAndActs()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    // The selection shows the area, the character at its cursor included.
    view.load(QStringLiteral("‸abcd"));
    view.keys(QStringLiteral("vll"));
    QCOMPARE(view.vim->mode(), VimMode::Visual);
    QCOMPARE(view.marked(), QStringLiteral("«abc»d"));
    QCOMPARE(view.vim->cursor(), 2);
    view.load(QStringLiteral("ab‸cd"));
    view.keys(QStringLiteral("vh"));
    QCOMPARE(view.marked(), QStringLiteral("a»bc«d"));
    view.load(QStringLiteral("a‸b\ncd"));
    view.keys(QStringLiteral("Vj"));
    QCOMPARE(view.vim->mode(), VimMode::VisualLine);
    QCOMPARE(view.marked(), QStringLiteral("«ab\ncd»"));
    // v and V switch between each other and back out.
    view.keys(QStringLiteral("v"));
    QCOMPARE(view.vim->mode(), VimMode::Visual);
    view.keys(QStringLiteral("v"));
    QCOMPARE(view.vim->mode(), VimMode::Normal);
    // A selection made with the mouse is Visual mode's.
    view.load(QStringLiteral("‸abcd"));
    view.select(1, 3);
    QCOMPARE(view.vim->mode(), VimMode::Visual);
    view.keys(QStringLiteral("d"));
    QCOMPARE(view.marked(), QStringLiteral("a‸d"));

    const QString failed = failures(view, {
        {"‸one two", "vlld", "‸ two"},
        {"a‸bcd", "vlohd", "‸d"},
        {"a‸bc\ndef", "v$d", "a‸def"},
        {"‸abc", "vl<Esc>", "a‸bc"},
        {"one\n‸two\nthree", "Vd", "one\n‸three"},
        {"one\n‸two\nthree", "Vjd", "‸one"},
        {"o‸ne\ntwo", "Vjy", "‸one\ntwo"},
        {"o‸ne\ntwo", "vjD", "‸"},
        {"‸one\ntwo\n\nthree", "vipd", "‸\nthree"},
        {"f(a ‸b) x", "vi(d", "f(‸) x"},
        {"f(a ‸b) x", "va(d", "f‸ x"},
        {"one t‸wo three", "viwd", "one ‸ three"},
        {"say \"h‸i\" now", "vi\"d", "say \"‸\" now"},
        {"‸one two", "vecX<Esc>", "‸X two"},
        {"o‸ne\ntwo", "VcX<Esc>", "‸X\ntwo"},
        {"‸one\ntwo", "Vj>", "\t‸one\n\ttwo"},
        {"‸one\ntwo", "vj>", "\t‸one\n\ttwo"},
        {"\t‸one\n\ttwo", "Vj<", "‸one\ntwo"},
        {"‸a\nb\nc", "VjjJ", "a b‸ c"},
        {"‸abc", "vl~", "‸ABc"},
        {"‸abc", "vlU", "‸ABc"},
        {"‸ABC", "vlu", "‸abC"},
        {"‸abc", "vlrx", "‸xxc"},
        {"‸one two", "yiwwviwp", "one on‸e"},
        {"‸one\ntwo", "yyjVp", "one\n‸one"},
        {"‸one two", "v/tw<CR>d", "‸wo"},
    });
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
}

void searchFindsAndRepeats()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    const QString failed = failures(view, {
        {"‸one two one two", "/two<CR>", "one ‸two one two"},
        {"‸one two one two", "/two<CR>n", "one two one ‸two"},
        {"‸one two one two", "/two<CR>nn", "one ‸two one two"},
        {"‸one two one two", "/two<CR>nN", "one ‸two one two"},
        {"one two one t‸wo", "?one<CR>", "one two ‸one two"},
        {"one two one t‸wo", "?one<CR>n", "‸one two one two"},
        {"‸one two one two", "2/two<CR>", "one two one ‸two"},
        // Letters match either case until the pattern has a capital.
        {"‸a Foo foo", "/foo<CR>", "a ‸Foo foo"},
        {"‸a Foo foo", "/Foo<CR>n", "a ‸Foo foo"},
        // * and # look for the whole word under the cursor.
        {"‸foo bar foo foobar", "*", "foo bar ‸foo foobar"},
        {"‸foo bar foo foobar", "**", "‸foo bar foo foobar"},
        {"foo bar ‸foo", "#", "‸foo bar foo"},
        {"‸ foo y foo", "*", " foo y ‸foo"},
        {"‸abc", "/zzz<CR>", "‸abc"},
        // A search is a motion for operators too.
        {"‸one two three", "d/thr<CR>", "‸three"},
        {"‸one two three", "/t<CR>dn", "one ‸three"},
    });
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
    // What a search says: nothing found, or that it went round.
    view.load(QStringLiteral("‸abc"));
    view.keys(QStringLiteral("/zzz<CR>"));
    QCOMPARE(view.vim->message(), QStringLiteral("Pattern not found: zzz"));
    view.keys(QStringLiteral("l"));
    QCOMPARE(view.vim->message(), QString());
    view.load(QStringLiteral("ab‸c ab"));
    view.keys(QStringLiteral("/ab<CR>n"));
    QCOMPARE(view.vim->message(), QStringLiteral("Search hit the bottom, went on at the top"));
    QCOMPARE(view.marked(), QStringLiteral("‸abc ab"));
    // The command line shows what is typed, and Backspace takes it back.
    view.keys(QStringLiteral("/xy"));
    QCOMPARE(view.vim->commandLine(), QStringLiteral("/xy"));
    view.keys(QStringLiteral("<BS>"));
    QCOMPARE(view.vim->commandLine(), QStringLiteral("/x"));
    view.keys(QStringLiteral("<BS><BS>"));
    QCOMPARE(view.vim->commandLine(), QString());
    QCOMPARE(view.vim->mode(), VimMode::Normal);
}

void registersKeepYanksAndPuts()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    const QString failed = failures(view, {
        {"‸one\ntwo", "\"ayyj\"ap", "one\ntwo\n‸one"},
        {"‸one\ntwo", "\"ayyj\"Ayy\"ap", "one\ntwo\n‸one\ntwo"},
        {"‸one two", "\"byiww\"Byiw\"bP", "one onetw‸otwo"},
        // A delete goes to the unnamed register; "0 keeps the last yank.
        {"‸one two", "yiwwdiwp", "one tw‸o"},
        {"‸one two", "yiwwdiw\"0p", "one on‸e"},
        // "_ keeps nothing.
        {"‸one two", "yiww\"_diwp", "one on‸e"},
    });
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
    // A register with nothing in it says so.
    view.load(QStringLiteral("‸abc"));
    view.keys(QStringLiteral("\"kp"));
    QCOMPARE(view.text(), QStringLiteral("abc"));
    QCOMPARE(view.vim->message(), QStringLiteral("Nothing in register k"));
    // "+ is the clipboard, both ways.
    view.load(QStringLiteral("‸one\ntwo"));
    view.keys(QStringLiteral("\"+yy"));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("one\n"));
    QGuiApplication::clipboard()->setText(QStringLiteral("pasted"));
    view.load(QStringLiteral("‸ab"));
    view.keys(QStringLiteral("\"+p"));
    QCOMPARE(view.marked(), QStringLiteral("apaste‸db"));
    // Registers are every view's: a yank in one editor puts in another.
    Editor other(QStringLiteral("x"));
    QVERIFY2(other.ok(), qPrintable(other.error));
    view.load(QStringLiteral("‸shared"));
    view.keys(QStringLiteral("\"syiw"));
    activate(&other.window);
    other.keys(QStringLiteral("\"sP"));
    QCOMPARE(other.text(), QStringLiteral("sharedx"));
}

void dotRepeatsTheLastChange()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    const QString failed = failures(view, {
        {"‸a b c d", "dw.", "‸c d"},
        {"‸a b c d e f", "dw2.", "‸d e f"},
        {"‸abcdefg", "3x.", "‸g"},
        {"‸abcdefg", "3x2.", "‸fg"},
        {"‸one two three", "cwX<Esc>w.", "X ‸X three"},
        {"‸a\nb", "A;<Esc>j.", "a;\nb‸;"},
        {"‸one", "oX<Esc>.", "one\nX\n‸X"},
        {"‸a", "3iX<Esc>.", "XXXX‸XXa"},
        {"‸one", ">>.", "\t\t‸one"},
        {"‸a,b,c", "df,.", "‸c"},
        {"‸abc", "rxl.", "x‸xc"},
        {"‸ab", "~.", "A‸B"},
        {"x(‸a) y(b)", "ci(Z<Esc>f(l.", "x(Z) y(‸Z)"},
        {"‸one two three", "d/t<CR>.", "‸three"},
        {"‸abcdef", "vld.", "‸ef"},
        {"‸a\nb\nc\nd\ne", "Vjd.", "‸e"},
        // A motion, a yank or an undo is no change to repeat.
        {"‸a b c", "dwwyiw.", "b‸ "},
    });
    QVERIFY2(failed.isEmpty(), qPrintable(failed));
}

void undoTakesBackWholeChanges()
{
    // Each change is one step, whatever it took to make it.
    const auto undone = [](const char *text, const char *keys) {
        Editor view(QString::fromUtf8(text));
        if (!view.ok()) return QStringLiteral("(no editor)");
        view.select(0, 0);
        view.keys(QString::fromUtf8(keys));
        view.keys(QStringLiteral("u"));
        return view.document->buffer()->canUndo() ? QStringLiteral("(more to undo)") : view.text();
    };
    QCOMPARE(undone("one two", "cwX<Esc>"), QStringLiteral("one two"));
    QCOMPARE(undone("a", "3iX<Esc>"), QStringLiteral("a"));
    QCOMPARE(undone("", "iab<CR>cd<Esc>"), QString());
    QCOMPARE(undone("one", "oX<Esc>"), QStringLiteral("one"));
    QCOMPARE(undone("one two", "Vy}p"), QStringLiteral("one two"));
    QCOMPARE(undone("a\nb\nc", "3J"), QStringLiteral("a\nb\nc"));

    Editor view(QStringLiteral("abcd"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.select(0, 0);
    view.keys(QStringLiteral("xxx"));
    QCOMPARE(view.text(), QStringLiteral("d"));
    view.keys(QStringLiteral("2u"));
    QCOMPARE(view.text(), QStringLiteral("bcd"));
    view.keys(QStringLiteral("<C-r>"));
    QCOMPARE(view.text(), QStringLiteral("cd"));
    view.keys(QStringLiteral("9u"));
    QCOMPARE(view.text(), QStringLiteral("abcd"));
    QCOMPARE(view.vim->message(), QString());
    view.keys(QStringLiteral("u"));
    QCOMPARE(view.vim->message(), QStringLiteral("Already at the oldest change"));
    // The conventional keys undo too.
    view.keys(QStringLiteral("x<C-z>"));
    QCOMPARE(view.text(), QStringLiteral("abcd"));

    // A dot repeat is one step, and so is a stay in Insert mode however long
    // it pauses; what is typed after Escape is a step of its own.
    qint64 now = 0;
    Editor typed(QStringLiteral("a b c"));
    QVERIFY2(typed.ok(), qPrintable(typed.error));
    typed.document->buffer()->setClock([&now] { return now; });
    typed.select(0, 0);
    typed.keys(QStringLiteral("dw."));
    typed.keys(QStringLiteral("u"));
    QCOMPARE(typed.text(), QStringLiteral("b c"));
    typed.keys(QStringLiteral("ix"));
    now += 5000;
    typed.keys(QStringLiteral("y<CR>z<Esc>"));
    typed.keys(QStringLiteral("a1<Esc>"));
    QCOMPARE(typed.text(), QStringLiteral("xy\nz1b c"));
    typed.keys(QStringLiteral("u"));
    QCOMPARE(typed.text(), QStringLiteral("xy\nzb c"));
    typed.keys(QStringLiteral("u"));
    QCOMPARE(typed.text(), QStringLiteral("b c"));

    // What a dialog changes, as a reload does, is a step of its own, even
    // in the middle of a stay in Insert mode.
    Editor reloaded(QStringLiteral("x"));
    QVERIFY2(reloaded.ok(), qPrintable(reloaded.error));
    reloaded.select(0, 0);
    reloaded.keys(QStringLiteral("Aab"));
    reloaded.pane->setProperty("inputBlocked", true);
    SourceBuffer *buffer = reloaded.document->buffer();
    buffer->replace({0, buffer->length()}, QStringLiteral("file"));
    reloaded.pane->setProperty("inputBlocked", false);
    reloaded.keys(QStringLiteral("c<Esc>"));
    QCOMPARE(reloaded.text(), QStringLiteral("filec"));
    reloaded.keys(QStringLiteral("u"));
    QCOMPARE(reloaded.text(), QStringLiteral("file"));
    reloaded.keys(QStringLiteral("u"));
    QCOMPARE(reloaded.text(), QStringLiteral("xab"));
    reloaded.keys(QStringLiteral("u"));
    QCOMPARE(reloaded.text(), QStringLiteral("x"));
}

void modifierPressesKeepPendingCommands()
{
    Editor view(QStringLiteral("abc"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.load(QStringLiteral("a‸bc"));
    view.keys(QStringLiteral("d"));
    QTest::keyPress(&view.window, Qt::Key_Shift);
    QCOMPARE(view.vim->pending(), QStringLiteral("d"));
    QTest::simulateEvent(&view.window, true, Qt::Key_Dollar, Qt::ShiftModifier, QStringLiteral("$"), false, 0);
    QTest::simulateEvent(&view.window, false, Qt::Key_Dollar, Qt::ShiftModifier, QStringLiteral("$"), false, 0);
    QTest::keyRelease(&view.window, Qt::Key_Shift);
    QCOMPARE(view.text(), QStringLiteral("a"));

    for (Qt::Key key : {Qt::Key_Control, Qt::Key_Alt, Qt::Key_AltGr, Qt::Key_Meta, Qt::Key_Super_L, Qt::Key_Super_R,
                        Qt::Key_Hyper_L, Qt::Key_CapsLock, Qt::Key_NumLock, Qt::Key_Mode_switch}) {
        view.load(QStringLiteral("a‸bc"));
        view.keys(QStringLiteral("d"));
        QTest::simulateEvent(&view.window, true, key, Qt::NoModifier, {}, false, 0);
        QCOMPARE(view.vim->pending(), QStringLiteral("d"));
        QTest::simulateEvent(&view.window, false, key, Qt::NoModifier, {}, false, 0);
        view.keys(QStringLiteral("w"));
        QCOMPARE(view.text(), QStringLiteral("a"));
    }
#ifdef Q_OS_LINUX
    for (quint32 keysym : {0xfe03u, 0xfe11u}) { // XKB ISO_Level3_Shift and ISO_Level5_Shift
        view.load(QStringLiteral("a‸bc"));
        view.keys(QStringLiteral("d"));
        QKeyEvent press(QEvent::KeyPress, Qt::Key_unknown, Qt::GroupSwitchModifier, 0, keysym, 0);
        QCoreApplication::sendEvent(view.window.activeFocusItem(), &press);
        QCOMPARE(view.vim->pending(), QStringLiteral("d"));
    }
#endif
    // A printable AltGr key still acts on the pending command.
    view.load(QStringLiteral("a‸bc"));
    view.keys(QStringLiteral("r"));
    QTest::simulateEvent(&view.window, true, Qt::Key_AltGr, Qt::GroupSwitchModifier, {}, false, 0);
    QTest::simulateEvent(&view.window, true, Qt::Key_At, Qt::GroupSwitchModifier, QStringLiteral("@"), false, 0);
    QTest::simulateEvent(&view.window, false, Qt::Key_At, Qt::GroupSwitchModifier, QStringLiteral("@"), false, 0);
    QTest::simulateEvent(&view.window, false, Qt::Key_AltGr, Qt::GroupSwitchModifier, {}, false, 0);
    QCOMPARE(view.text(), QStringLiteral("a@c"));
}

void deadKeysKeepPendingCharacterCommands()
{
    Editor view(QStringLiteral("cafe"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    const auto deadKey = [&](Qt::Key key) {
        QTest::simulateEvent(&view.window, true, key, Qt::NoModifier, {}, false, 0);
        QTest::simulateEvent(&view.window, false, key, Qt::NoModifier, {}, false, 0);
    };
    view.load(QStringLiteral("‸cafe and more"));
    view.keys(QStringLiteral("0lr"));
    deadKey(Qt::Key_Dead_Acute);
    QCOMPARE(view.vim->pending(), QStringLiteral("r"));
    view.keys(QStringLiteral("e"));
    QCOMPARE(view.text(), QStringLiteral("céfe and more"));
    view.keys(QStringLiteral("u"));
    QCOMPARE(view.text(), QStringLiteral("cafe and more"));

    // A dead key with no precomposed result cancels without an edit. The
    // next command, after the cancellation, takes its argument as usual.
    view.keys(QStringLiteral("0lr"));
    deadKey(Qt::Key_Dead_Acute);
    view.keys(QStringLiteral("q"));
    QCOMPARE(view.text(), QStringLiteral("cafe and more"));
    QVERIFY(view.vim->pending().isEmpty());
    view.keys(QStringLiteral("rx"));
    QCOMPARE(view.text(), QStringLiteral("cxfe and more"));

    // A key already composed by the platform takes its character once.
    view.load(QStringLiteral("c‸afe"));
    view.keys(QStringLiteral("r"));
    deadKey(Qt::Key_Dead_Acute);
    view.keys(QStringLiteral("é"));
    QCOMPARE(view.text(), QStringLiteral("céfe"));

    for (auto key : {Qt::Key_Dead_Acute, Qt::Key_Dead_Grave, Qt::Key_Dead_Diaeresis, Qt::Key_Dead_Circumflex,
                     Qt::Key_Dead_Tilde}) {
        view.load(QStringLiteral("caf‸e"));
        view.keys(QStringLiteral("r"));
        deadKey(key);
        QCOMPARE(view.vim->pending(), QStringLiteral("r"));
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("é"));
        QCoreApplication::sendEvent(view.window.activeFocusItem(), &commit);
        QCOMPARE(view.text(), QStringLiteral("café"));
    }
    for (const QString &command : {QStringLiteral("f"), QStringLiteral("t")}) {
        view.load(QStringLiteral("‸a e é"));
        view.keys(command);
        QVERIFY(!(view.window.activeFocusItem()->flags() & QQuickItem::ItemAcceptsInputMethod));
        QInputMethodEvent preedit(QStringLiteral("ci"), {});
        QCoreApplication::sendEvent(view.window.activeFocusItem(), &preedit);
        QCOMPARE(view.vim->pending(), command);
        deadKey(Qt::Key_Dead_Acute);
        QCOMPARE(view.vim->pending(), command);
        view.keys(QStringLiteral("e"));
        QCOMPARE(view.vim->cursor(), command == u"f" ? 4 : 3);
    }
    view.load(QStringLiteral("‸cafe"));
    view.keys(QStringLiteral("r"));
    QVERIFY(!(view.window.activeFocusItem()->flags() & QQuickItem::ItemAcceptsInputMethod));
    view.keys(QStringLiteral("x"));
    QCOMPARE(view.text(), QStringLiteral("xafe"));
}

void controlRRedoesWithVimEditorFocus()
{
    ShellOptions options;
    options.settings = R"({"vimKeys":true})";
    ShellFixture shell(options);
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    editor->forceActiveFocus();
    QObject *headings = shell.object(QStringLiteral("headingsDialog"));
    QVERIFY(headings);
    SourceBuffer *buffer = shell.document()->buffer();
    const auto readyToRedo = [&] {
        press(window, QStringLiteral("<Esc>"));
        buffer->replace({0, buffer->length()}, QStringLiteral("# Title\none\n"));
        selectRange(editor, 8, 8);
        press(window, QStringLiteral("xx2u"));
    };
    for (const QString &mode : {QString(), QStringLiteral("v"), QStringLiteral("V")}) {
        readyToRedo();
        QCOMPARE(buffer->text(), QStringLiteral("# Title\none\n"));
        press(window, mode + QStringLiteral("2<C-r>"));
        QCOMPARE(buffer->text(), QStringLiteral("# Title\ne\n"));
        QVERIFY(!headings->property("visible").toBool());
    }
    for (QLatin1StringView pending : {QLatin1StringView("d"), QLatin1StringView("f"), QLatin1StringView("t"),
                                    QLatin1StringView("r"), QLatin1StringView("\""), QLatin1StringView("\"a")}) {
        readyToRedo();
        QVERIFY(buffer->canRedo());
        press(window, pending + QStringLiteral("<C-r>"));
        QCOMPARE(buffer->text(), QStringLiteral("# Title\none\n"));
        QVERIFY(buffer->canRedo());
        QVERIFY(!headings->property("visible").toBool());
        press(window, QStringLiteral("2<C-r>"));
        QCOMPARE(buffer->text(), QStringLiteral("# Title\ne\n"));
    }
    readyToRedo();
    press(window, QStringLiteral("i<C-r>"));
    QTRY_VERIFY(headings->property("opened").toBool());
    QCOMPARE(buffer->text(), QStringLiteral("# Title\none\n"));
    press(window, QStringLiteral("<Esc><Esc>"));
    shell.settings()->setVimKeys(false);
    QTRY_COMPARE(window->activeFocusItem(), editor);
    press(window, QStringLiteral("<C-r>"));
    QTRY_VERIFY(headings->property("opened").toBool());
}

void controlROpensHeadingsInReadOnlyVim()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString path = root.filePath(QStringLiteral("read-only.md"));
    writeFile(path, "# Title\n\nInvalid \xff text.\n");
    ShellFixture shell({.settings = R"({"vimKeys":true})", .files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QVERIFY(editor->property("readOnly").toBool());
    editor->forceActiveFocus();
    press(window, QStringLiteral("<C-r>"));
    QTRY_VERIFY(shell.object(QStringLiteral("headingsDialog"))->property("opened").toBool());
    QTRY_COMPARE(shell.object(QStringLiteral("headingsList"))->property("count").toInt(), 1);
}

void escapeAlwaysReachesNormalMode()
{
    Editor view;
    QVERIFY2(view.ok(), qPrintable(view.error));
    for (const char *keys : {"i", "a", "A", "o", "v", "V", "vl", "Vj", ":", ":wq", "/", "/ab", "?x", "d", "3", "2d",
                             "\"a", "\"ad", "f", "df", "r", "g", "dg", "di", "c", "ci", "y", "vi", ">"}) {
        const QString pressed = QString::fromUtf8(keys);
        view.load(QStringLiteral("‸abc\ndef"));
        view.keys(pressed + QStringLiteral("<Esc>"));
        QVERIFY2(view.vim->mode() == VimMode::Normal, keys);
        QVERIFY2(view.vim->pending().isEmpty(), keys);
        QVERIFY2(view.vim->commandLine().isEmpty(), keys);
        QVERIFY2(view.binding->anchorSource() == view.binding->cursorSource(), keys);
        QVERIFY2(view.text() == QStringLiteral("abc\ndef") || pressed.startsWith(u'o'), keys);
        // Ctrl+[ is Escape too.
        view.load(QStringLiteral("‸abc\ndef"));
        view.keys(pressed + QStringLiteral("<C-[>"));
        QVERIFY2(view.vim->mode() == VimMode::Normal && view.vim->pending().isEmpty(), keys);
    }
    // Escape with nothing to leave goes on to the view, which dismisses notices with it.
    view.load(QStringLiteral("‸abc"));
    QSignalSpy escaped(view.pane, SIGNAL(escapePressed()));
    view.keys(QStringLiteral("i<Esc>"));
    QCOMPARE(escaped.count(), 0);
    view.keys(QStringLiteral("<Esc>"));
    QCOMPARE(escaped.count(), 1);
}

void insertTypesAsWithoutVimKeys()
{
    // Off, keys type, Vim's as any other.
    Editor plain(QStringLiteral("- one"), false);
    QVERIFY2(plain.ok(), qPrintable(plain.error));
    QVERIFY(!plain.vim->active());
    QCOMPARE(plain.vim->modeName(), QString());
    plain.select(5, 5);
    plain.keys(QStringLiteral("dd:q<Esc>"));
    QCOMPARE(plain.text(), QStringLiteral("- onedd:q"));

    // In Insert mode every key does what it does without Vim keys.
    Editor vim(QStringLiteral("- one"));
    QVERIFY2(vim.ok(), qPrintable(vim.error));
    plain.document->buffer()->replace({0, plain.document->buffer()->length()}, QStringLiteral("- one"));
    plain.select(5, 5);
    vim.keys(QStringLiteral("A"));
    QCOMPARE(vim.vim->mode(), VimMode::Insert);
    QCOMPARE(vim.marked(), plain.marked());
    for (const char *keys : {"x", "<CR>", "two", "<Tab>", "<BS>", "<S-CR>", "hjkl :wq", "<Left><Left>d", "<Del>",
                             "<CR><CR>", "<C-z>", "<Home>p"}) {
        plain.keys(QString::fromUtf8(keys));
        vim.keys(QString::fromUtf8(keys));
        QVERIFY2(vim.marked() == plain.marked(), qPrintable(keys + QStringLiteral(": ") + vim.marked()));
    }
    for (QChar c : QStringLiteral("é漢")) {
        sendKey(&plain.window, c);
        sendKey(&vim.window, c);
    }
    QCOMPARE(vim.marked(), plain.marked());

    // An input method composes and commits in Insert mode as without Vim keys.
    QVERIFY(vim.editor->flags() & QQuickItem::ItemAcceptsInputMethod);
    const auto compose = [](Editor &view) {
        sendPreedit(&view.window, QStringLiteral("ni"));
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("你"));
        QCoreApplication::sendEvent(view.window.activeFocusItem(), &commit);
    };
    compose(plain);
    compose(vim);
    QCOMPARE(vim.marked(), plain.marked());
    QVERIFY(vim.text().endsWith(QStringLiteral("你")) || vim.text().contains(QStringLiteral("你")));
    // Repeated, the commit is typed again.
    vim.keys(QStringLiteral("<Esc>"));
    QCOMPARE(vim.vim->mode(), VimMode::Normal);
    vim.load(QStringLiteral("‸ab"));
    vim.keys(QStringLiteral("i"));
    compose(vim);
    vim.keys(QStringLiteral("<Esc>ll."));
    QCOMPARE(vim.text(), QStringLiteral("你a你b"));

    // Outside Insert mode the editor takes no input method, and what one
    // commits anyway is keys: x deletes, nothing is typed.
    QVERIFY(!(vim.editor->flags() & QQuickItem::ItemAcceptsInputMethod));
    vim.load(QStringLiteral("‸abc"));
    QInputMethodEvent keys;
    keys.setCommitString(QStringLiteral("x"));
    QCoreApplication::sendEvent(vim.window.activeFocusItem(), &keys);
    QCOMPARE(vim.text(), QStringLiteral("bc"));
    QInputMethodEvent insert;
    insert.setCommitString(QStringLiteral("iok"));
    QCoreApplication::sendEvent(vim.window.activeFocusItem(), &insert);
    QCOMPARE(vim.text(), QStringLiteral("okbc"));
    QCOMPARE(vim.vim->mode(), VimMode::Insert);
    QVERIFY(vim.editor->flags() & QQuickItem::ItemAcceptsInputMethod);

    // Turned off, the editor types again at once, and takes input methods.
    vim.keys(QStringLiteral("<Esc>"));
    vim.pane->setProperty("vimKeys", false);
    QVERIFY(vim.editor->flags() & QQuickItem::ItemAcceptsInputMethod);
    vim.load(QStringLiteral("‸ok"));
    vim.keys(QStringLiteral("x"));
    QCOMPARE(vim.text(), QStringLiteral("xok"));
}

void commandLineRunsWindowCommands()
{
    Editor view(QStringLiteral("one\ntwo\nthree\nfour\nfive"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    QSignalSpy requested(view.vim, &VimMode::commandRequested);
    // Lines by number.
    view.keys(QStringLiteral(":4<CR>"));
    QCOMPARE(view.marked(), QStringLiteral("one\ntwo\nthree\n‸four\nfive"));
    view.keys(QStringLiteral(":$<CR>"));
    QCOMPARE(view.vim->cursor(), 19);
    view.keys(QStringLiteral(":1<CR>"));
    QCOMPARE(view.vim->cursor(), 0);
    // :w saves; :q and :q! close the tab, which keeps the text; :wq and :x
    // close a named document's tab, which saves it first.
    const auto ran = [&](const char *keys) {
        requested.clear();
        view.keys(QString::fromUtf8(keys));
        if (requested.isEmpty() && !requested.wait(1000)) return QStringLiteral("(nothing)");
        return requested.takeFirst().at(0).toString();
    };
    QCOMPARE(ran(":w<CR>"), QStringLiteral("save"));
    QCOMPARE(ran(":write<CR>"), QStringLiteral("save"));
    QCOMPARE(ran(":q<CR>"), QStringLiteral("closeTab"));
    QCOMPARE(ran(":q!<CR>"), QStringLiteral("closeTab"));
    QCOMPARE(ran(":wq<CR>"), QStringLiteral("closeTab"));
    QCOMPARE(ran(":x<CR>"), QStringLiteral("closeTab"));
    // Unknown commands say so; nothing runs.
    requested.clear();
    view.keys(QStringLiteral(":frobnicate<CR>"));
    QCOMPARE(view.vim->message(), QStringLiteral("Not an editor command: frobnicate"));
    QVERIFY(!requested.wait(100));
    // A draft is named before :wq closes anything: it saves, which asks.
    Document *draft = view.store->newDraft();
    view.pane->setProperty("document", QVariant::fromValue(draft));
    QTRY_VERIFY(view.vim->cursor() == 0 && view.binding->document() == draft);
    QCOMPARE(ran(":wq<CR>"), QStringLiteral("save"));
    QCOMPARE(ran(":x<CR>"), QStringLiteral("save"));
}

namespace {

bool isOpen(const QObject *popup) { return popup && popup->property("opened").toBool(); }

void runIn(QQuickWindow *window, const char *command)
{
    QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
}

QString shownText(const ShellFixture &shell, const char *name)
{
    QQuickItem *item = shell.item(QString::fromLatin1(name));
    return item && item->isVisible() ? item->property("text").toString() : QStringLiteral("(hidden)");
}

} // namespace

void commandLineSavesAndClosesTabs()
{
    QTemporaryDir files;
    const QString first = files.filePath(QStringLiteral("first.md"));
    const QString second = files.filePath(QStringLiteral("second.md"));
    const QString third = files.filePath(QStringLiteral("third.md"));
    writeFile(first, "first");
    writeFile(second, "second");
    writeFile(third, "third");
    ShellFixture shell({.settings = "{\"vimKeys\": true}", .files = {first, second, third}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    TabModel *tabs = shell.context()->tabs();
    QCOMPARE(tabs->rowCount(), 3);
    QCOMPARE(shell.document()->path(), third);
    QCOMPARE(shownText(shell, "vimMode"), QStringLiteral("NORMAL"));
    // The command line shows as it is typed; :w saves.
    press(window, QStringLiteral("A one<Esc>:w"));
    QCOMPARE(shownText(shell, "vimCommand"), QStringLiteral(":w"));
    press(window, QStringLiteral("<CR>"));
    QTRY_COMPARE(readFile(third), QByteArray("third one"));
    QCOMPARE(shownText(shell, "vimCommand"), QStringLiteral("(hidden)"));
    // :q closes the tab, saving what it has not saved yet.
    press(window, QStringLiteral("A two<Esc>:q<CR>"));
    QTRY_COMPARE(tabs->rowCount(), 2);
    QTRY_COMPARE(readFile(third), QByteArray("third one two"));
    // :wq does the same; the next document comes up in Normal mode.
    QTRY_COMPARE(shell.document()->path(), second);
    QCOMPARE(shownText(shell, "vimMode"), QStringLiteral("NORMAL"));
    press(window, QStringLiteral("x:wq<CR>"));
    QTRY_COMPARE(tabs->rowCount(), 1);
    QTRY_COMPARE(readFile(second), QByteArray("econd"));
    // A draft is never closed by :wq or :x: it is to be named first.
    shell.backend->newDraft();
    QTRY_COMPARE(tabs->rowCount(), 2);
    QVERIFY(shell.document()->isDraft());
    press(window, QStringLiteral("idraft<Esc>:wq<CR>"));
    QTest::qWait(50);
    QCOMPARE(tabs->rowCount(), 2);
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("draft"));
    // An unknown command says so in the status line.
    press(window, QStringLiteral(":nope<CR>"));
    QCOMPARE(shownText(shell, "vimCommand"), QStringLiteral("Not an editor command: nope"));
}

void windowShowsAndLeavesVimKeys()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 960);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QQuickItem *block = shell.item(QStringLiteral("blockCaret"));
    QObject *palette = shell.object(QStringLiteral("commandPalette"));
    QObject *entry = shell.object(QStringLiteral("vimKeysEntry"));
    QVERIFY(editor && block && palette && entry);
    SourceBuffer *buffer = shell.document()->buffer();
    // Off by default: Vim's keys type, and no mode shows.
    QVERIFY(!shell.settings()->vimKeys());
    QCOMPARE(shownText(shell, "vimArea"), QStringLiteral("(hidden)"));
    press(window, QStringLiteral("dd"));
    QCOMPARE(buffer->text(), QStringLiteral("dd"));
    QCOMPARE(entry->property("text").toString(), QStringLiteral("Turn on Vim keys"));

    // The command turns them on, in Normal mode, with a block caret.
    runIn(window, "vimKeys");
    QVERIFY(shell.settings()->vimKeys());
    QTRY_COMPARE(shownText(shell, "vimMode"), QStringLiteral("NORMAL"));
    QCOMPARE(entry->property("text").toString(), QStringLiteral("Turn off Vim keys"));
    QTRY_VERIFY(block->isVisible());
    QVERIFY(block->width() > 1);
    press(window, QStringLiteral("i"));
    QCOMPARE(shownText(shell, "vimMode"), QStringLiteral("INSERT"));
    QVERIFY(!block->isVisible());
    press(window, QStringLiteral("x<Esc>"));
    QCOMPARE(buffer->text(), QStringLiteral("dxd"));
    QCOMPARE(shownText(shell, "vimMode"), QStringLiteral("NORMAL"));
    press(window, QStringLiteral("v"));
    QCOMPARE(shownText(shell, "vimMode"), QStringLiteral("VISUAL"));
    press(window, QStringLiteral("V"));
    QCOMPARE(shownText(shell, "vimMode"), QStringLiteral("VISUAL LINE"));
    press(window, QStringLiteral("<Esc>"));
    QCOMPARE(shownText(shell, "vimMode"), QStringLiteral("NORMAL"));
    // A command half typed shows beside the mode.
    press(window, QStringLiteral("2d"));
    QCOMPARE(shownText(shell, "vimCommand"), QStringLiteral("2d"));
    press(window, QStringLiteral("<Esc>"));
    QCOMPARE(shownText(shell, "vimCommand"), QStringLiteral("(hidden)"));

    // The palette opens in every mode, the command line's too; Escape there
    // closes only the palette.
    for (const char *keys : {":wq", "i", "v", ""}) {
        press(window, QString::fromLatin1(keys));
        const QString mode = shownText(shell, "vimMode");
        const QString command = shownText(shell, "vimCommand");
        QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
        QTRY_VERIFY2(isOpen(palette), keys);
        QTest::keyClick(window, Qt::Key_Escape);
        QTRY_VERIFY2(!isOpen(palette), keys);
        QTRY_COMPARE(window->activeFocusItem(), editor);
        QCOMPARE(shownText(shell, "vimMode"), mode);
        QCOMPARE(shownText(shell, "vimCommand"), command);
        press(window, QStringLiteral("<Esc>"));
    }
    QCOMPARE(buffer->text(), QStringLiteral("dxd"));
    // So does the menu.
    QObject *menu = shell.object(QStringLiteral("windowMenu"));
    press(window, QStringLiteral("i"));
    QTest::keyClick(window, Qt::Key_F10);
    QTRY_VERIFY(isOpen(menu));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(menu));
    press(window, QStringLiteral("<Esc>"));

    // Everything is named and fits, a long command line at the narrowest width too.
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    press(window, QStringLiteral(":abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz"));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    resizeWindow(window, 360);
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    press(window, QStringLiteral("<Esc>"));
    resizeWindow(window, 960);

    // From the palette, in Normal mode, the same command turns them off.
    QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(isOpen(palette));
    auto *matches = qobject_cast<CommandFilter *>(findObject(window, QStringLiteral("paletteMatches")));
    QVERIFY(matches);
    typeText(window, QStringLiteral("vim"));
    QCOMPARE(matches->idAt(0), QStringLiteral("vimKeys"));
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!isOpen(palette));
    QVERIFY(!shell.settings()->vimKeys());
    QCOMPARE(shownText(shell, "vimArea"), QStringLiteral("(hidden)"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    selectRange(editor, 3, 3);
    press(window, QStringLiteral("dd"));
    QCOMPARE(buffer->text(), QStringLiteral("dxddd"));

    // Preferences turn them on again, and the choice is kept.
    QObject *dialog = shell.object(QStringLiteral("preferencesDialog"));
    runIn(window, "preferences");
    QTRY_VERIFY(isOpen(dialog));
    auto *check = qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("vimKeysChoice")));
    QVERIFY(check);
    QVERIFY(!check->property("checked").toBool());
    check->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Space);
    QVERIFY(shell.settings()->vimKeys());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    QTRY_COMPARE(shownText(shell, "vimMode"), QStringLiteral("NORMAL"));
    QVERIFY(Settings(shell.paths().settingsFile()).vimKeys());
}

} // namespace VimTests
