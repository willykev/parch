#include "editingtests.h"

#include "apppaths.h"
#include "commandfilter.h"
#include "commandregistry.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "markdownediting.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "theme.h"
#include "windowcontext.h"
#include "windowtesting.h"
#include "wordcounter.h"

#include <QAccessible>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>
#include <memory>

namespace {

// Texts in tests mark the selection: ‸ is a caret, and « and » are the
// anchor and the cursor of a selection, in either order.
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
        if (c == QChar(0x2038)) {
            anchor = cursor = int(result.text.size());
        } else if (c == u'«') {
            anchor = int(result.text.size());
        } else if (c == u'»') {
            cursor = int(result.text.size());
        } else {
            result.text += c;
        }
    }
    result.anchor = anchor < 0 ? int(result.text.size()) : anchor;
    result.cursor = cursor < 0 ? result.anchor : cursor;
    return result;
}

QString mark(const QString &text, int anchor, int cursor)
{
    QString marked = text;
    if (anchor == cursor) return marked.insert(cursor, QChar(0x2038));
    // The later mark first, so that the earlier position still holds.
    if (anchor > cursor) return marked.insert(anchor, u'«').insert(cursor, u'»');
    return marked.insert(cursor, u'»').insert(anchor, u'«');
}

// What a change makes of a marked text, applied through a SourceBuffer, so
// that it is checked to be one undo step that undoes and redoes exactly.
QString applied(const QString &before, const std::optional<TextChange> &change)
{
    const Marked start = unmark(before);
    if (!change) return QStringLiteral("(nothing)");
    SourceBuffer buffer(start.text);
    if (!change->edits.isEmpty()) {
        if (buffer.replace(change->edits) != SourceBuffer::EditOutcome::Applied) return QStringLiteral("(rejected)");
        const QString after = buffer.text();
        buffer.undo();
        if (buffer.text() != start.text || buffer.canUndo()) return QStringLiteral("(not one undo step)");
        buffer.redo();
        if (buffer.text() != after) return QStringLiteral("(redo differs)");
    }
    if (change->anchor < 0 || change->cursor < 0 || change->anchor > buffer.length() || change->cursor > buffer.length())
        return QStringLiteral("(selection outside the text)");
    return mark(buffer.text(), change->anchor, change->cursor);
}

QString formatted(const QString &command, const QString &marked, const MarkdownEditing::Options &options = {})
{
    const Marked start = unmark(marked);
    return applied(marked, MarkdownEditing::format(command, start.text, start.anchor, start.cursor, options));
}

MarkdownEditing::Options setext()
{
    MarkdownEditing::Options options;
    options.setextHeadings = true;
    return options;
}

MarkdownEditing::Options crlf()
{
    MarkdownEditing::Options options;
    options.newline = QStringLiteral("\r\n");
    return options;
}

// One editor over one document, as a window shows it: a theme on a
// temporary folder, a store on temporary paths, and an EditorPane.
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
    QString error;

    explicit Editor(const QString &text, const QString &name = QStringLiteral("doc.md"))
    {
        writeTheme(themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
        theme = std::make_unique<Theme>(themeFolder.path());
        store = std::make_unique<DocumentStore>(AppPaths::under(root.path()));
        engine.rootContext()->setContextProperty(QStringLiteral("theme"), theme.get());
        writeFile(root.filePath(name), text.toUtf8());
        document = store->open(root.filePath(name));
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
        editor = pane->findChild<QQuickItem *>(QStringLiteral("editor"));
        binding = pane->findChild<EditorBinding *>();
        window.show();
        if (editor) editor->forceActiveFocus();
        QCoreApplication::processEvents();
    }
    bool ok() const { return document && pane && editor && binding; }
    QString text() const { return document->buffer()->text(); }
    // Selects in source offsets, as the writer would with the mouse.
    void select(int anchor, int cursor)
    {
        QMetaObject::invokeMethod(editor, "select", Q_ARG(int, binding->sourceToEditor(anchor)),
                                  Q_ARG(int, binding->sourceToEditor(cursor)));
    }
    // The text with the editor's selection marked.
    QString marked() const { return mark(text(), binding->anchorSource(), binding->cursorSource()); }
    void key(Qt::Key key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { sendKey(&window, key, modifiers); }
};

} // namespace

namespace EditingTests {

void sourceEditsGroupIntoOneUndoStep()
{
    SourceBuffer buffer(QStringLiteral("one two three"));
    QSignalSpy modified(&buffer, &SourceBuffer::modifiedChanged);
    // Several places at once, given against the text as it is, undo as one.
    QCOMPARE(buffer.replace({{{0, 0}, QStringLiteral("**")}, {{3, 0}, QStringLiteral("**")}, {{8, 5}, QStringLiteral("3")}}),
             SourceBuffer::EditOutcome::Applied);
    QCOMPARE(buffer.text(), QStringLiteral("**one** two 3"));
    QCOMPARE(modified.count(), 1);
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("one two three"));
    QVERIFY(!buffer.canUndo());
    buffer.redo();
    QCOMPARE(buffer.text(), QStringLiteral("**one** two 3"));
    buffer.undo();

    // One edit that would split a grapheme refuses them all.
    SourceBuffer accented(QStringLiteral("e\u0301 x"));
    QCOMPARE(accented.replace({{{0, 0}, QStringLiteral("[")}, {{1, 0}, QStringLiteral("]")}}),
             SourceBuffer::EditOutcome::Rejected);
    QCOMPARE(accented.text(), QStringLiteral("e\u0301 x"));
    QVERIFY(!accented.canUndo());
    // Overlapping or unsorted edits are refused as well.
    QCOMPARE(buffer.replace({{{4, 3}, QStringLiteral("a")}, {{0, 5}, QStringLiteral("b")}}),
             SourceBuffer::EditOutcome::Rejected);
    QCOMPARE(buffer.replace({{{0, 2}, QStringLiteral("a")}, {{1, 2}, QStringLiteral("b")}}),
             SourceBuffer::EditOutcome::Rejected);
    // Edits that change nothing are nothing to undo.
    QCOMPARE(buffer.replace({{{0, 3}, QStringLiteral("one")}}), SourceBuffer::EditOutcome::Unchanged);
    QVERIFY(!buffer.canUndo());

    // Edits made one by one between beginStep() and endStep() are one step
    // too, nested steps included, and typing afterwards starts another.
    buffer.beginStep();
    buffer.replace({0, 0}, QStringLiteral("a"), EditKind::Typing);
    buffer.beginStep();
    buffer.replace({1, 0}, QStringLiteral("b"), EditKind::Typing);
    buffer.endStep();
    buffer.replace({int(buffer.length()), 0}, QStringLiteral("!"));
    buffer.endStep();
    buffer.replace({int(buffer.length()), 0}, QStringLiteral("?"), EditKind::Typing);
    QCOMPARE(buffer.text(), QStringLiteral("abone two three!?"));
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("abone two three!"));
    buffer.undo();
    QCOMPARE(buffer.text(), QStringLiteral("one two three"));
    QVERIFY(!buffer.canUndo());
    // A step with nothing in it leaves nothing behind.
    buffer.beginStep();
    buffer.endStep();
    QVERIFY(!buffer.canUndo());
}

void inlineFormattingWrapsAndToggles()
{
    // A selection is wrapped and stays selected; selected again, with or
    // without its markers, it is unwrapped.
    QCOMPARE(formatted("bold", "a «word» b"), QStringLiteral("a **«word»** b"));
    QCOMPARE(formatted("bold", "a **«word»** b"), QStringLiteral("a «word» b"));
    QCOMPARE(formatted("bold", "a «**word**» b"), QStringLiteral("a «word» b"));
    QCOMPARE(formatted("bold", "__«word»__"), QStringLiteral("«word»"));
    // The selection keeps its direction.
    QCOMPARE(formatted("bold", "a »word« b"), QStringLiteral("a **»word«** b"));
    // Spaces at the edges of a selection stay outside the markers.
    QCOMPARE(formatted("bold", "«word »next"), QStringLiteral("**«word»** next"));
    // Without a selection: a pair around the caret, removed again, and the
    // markers of the span the caret is in removed.
    QCOMPARE(formatted("bold", "a ‸ b"), QStringLiteral("a **‸** b"));
    QCOMPARE(formatted("bold", "a **‸** b"), QStringLiteral("a ‸ b"));
    QCOMPARE(formatted("bold", "a **wo‸rd** b"), QStringLiteral("a wo‸rd b"));
    // Between two spans the caret is in neither.
    QCOMPARE(formatted("bold", "**a** b‸c **d**"), QStringLiteral("**a** b**‸**c **d**"));
    // Every line of a selection over several lines, after its list marker.
    QCOMPARE(formatted("bold", "«one\ntwo»"), QStringLiteral("**«one**\n**two»**"));
    QCOMPARE(formatted("bold", "«- one\n- two»"), QStringLiteral("- **«one**\n- **two»**"));
    QCOMPARE(formatted("bold", "«**one**\n**two**»"), QStringLiteral("«one\ntwo»"));
    QCOMPARE(formatted("italic", "«one\r\n\r\ntwo»"), QStringLiteral("*«one*\r\n\r\n*two»*"));

    // Bold and italic nest in either order and come apart again.
    QCOMPARE(formatted("italic", "«word»"), QStringLiteral("*«word»*"));
    QCOMPARE(formatted("bold", "*«word»*"), QStringLiteral("***«word»***"));
    QCOMPARE(formatted("italic", "**«word»**"), QStringLiteral("***«word»***"));
    QCOMPARE(formatted("italic", "***«word»***"), QStringLiteral("**«word»**"));
    QCOMPARE(formatted("bold", "***«word»***"), QStringLiteral("*«word»*"));
    QCOMPARE(formatted("italic", "‸"), QStringLiteral("*‸*"));
    QCOMPARE(formatted("italic", "*‸*"), QStringLiteral("‸"));
    QCOMPARE(formatted("italic", "*wo‸rd* and **bold**"), QStringLiteral("wo‸rd and **bold**"));
    QCOMPARE(formatted("italic", "**bo‸ld**"), QStringLiteral("**bo*‸*ld**"));

    QCOMPARE(formatted("strikethrough", "«gone»"), QStringLiteral("~~«gone»~~"));
    QCOMPARE(formatted("strikethrough", "~~«gone»~~"), QStringLiteral("«gone»"));
    QCOMPARE(formatted("highlight", "«marked»"), QStringLiteral("==«marked»=="));
    QCOMPARE(formatted("highlight", "==ma‸rked=="), QStringLiteral("ma‸rked"));
    QCOMPARE(formatted("inlineMath", "«x^2»"), QStringLiteral("$«x^2»$"));
    QCOMPARE(formatted("inlineMath", "$«x^2»$"), QStringLiteral("«x^2»"));
    QCOMPARE(formatted("inlineMath", "‸"), QStringLiteral("$‸$"));

    // Code takes a delimiter longer than any run of backticks inside it.
    QCOMPARE(formatted("code", "«value»"), QStringLiteral("`«value»`"));
    QCOMPARE(formatted("code", "«a`b»"), QStringLiteral("``«a`b»``"));
    QCOMPARE(formatted("code", "«`x»"), QStringLiteral("`` «`x» ``"));
    QCOMPARE(formatted("code", "`«value»`"), QStringLiteral("«value»"));
    QCOMPARE(formatted("code", "«`value`»"), QStringLiteral("«value»"));
    QCOMPARE(formatted("code", "‸"), QStringLiteral("`‸`"));
    QCOMPARE(formatted("code", "say `co‸de` now"), QStringLiteral("say co‸de now"));

    QCOMPARE(formatted("comment", "«note»"), QStringLiteral("<!-- «note» -->"));
    QCOMPARE(formatted("comment", "<!-- «note» -->"), QStringLiteral("«note»"));
    QCOMPARE(formatted("comment", "«<!-- note -->»"), QStringLiteral("«note»"));
    QCOMPARE(formatted("comment", "‸"), QStringLiteral("<!-- ‸ -->"));
    QCOMPARE(formatted("comment", "<!-- no‸te -->"), QStringLiteral("no‸te"));
    QCOMPARE(formatted("comment", "«a\nb»"), QStringLiteral("<!-- «a\nb» -->"));
}

void linksAndImagesSelectWhatComesNext()
{
    QCOMPARE(formatted("link", "see «parch»"), QStringLiteral("see [parch](«url»)"));
    QCOMPARE(formatted("link", "‸"), QStringLiteral("[text](«url»)"));
    // A selected address becomes the destination, and the text is selected.
    QCOMPARE(formatted("link", "«https://example.org/a»"), QStringLiteral("[«text»](https://example.org/a)"));
    // In a link, the command takes the link away and keeps its text.
    QCOMPARE(formatted("link", "a [par‸ch](https://example.org) b"), QStringLiteral("a par‸ch b"));
    QCOMPARE(formatted("image", "«logo»"), QStringLiteral("![logo](«url»)"));
    QCOMPARE(formatted("image", "‸"), QStringLiteral("![alt text](«url»)"));
    QCOMPARE(formatted("image", "![lo‸go](a.png)"), QStringLiteral("lo‸go"));
    // The image command leaves a link alone, and the link command an image.
    QCOMPARE(formatted("link", "![lo‸go](a.png)"), QStringLiteral("![lo[text](«url»)go](a.png)"));
}

void headingsFollowTheChosenSyntax()
{
    QCOMPARE(formatted("heading1", "Ti‸tle"), QStringLiteral("# Ti‸tle"));
    QCOMPARE(formatted("heading1", "# Ti‸tle"), QStringLiteral("Ti‸tle"));
    QCOMPARE(formatted("heading2", "# Ti‸tle"), QStringLiteral("## Ti‸tle"));
    QCOMPARE(formatted("heading6", "‸Title"), QStringLiteral("###### ‸Title"));
    QCOMPARE(formatted("paragraph", "### Ti‸tle"), QStringLiteral("Ti‸tle"));
    QCOMPARE(formatted("paragraph", "Ti‸tle"), QStringLiteral("(nothing)"));
    QCOMPARE(formatted("heading3", "«a\n\nb»"), QStringLiteral("«### a\n\n### b»"));
    QCOMPARE(formatted("heading2", "> Ti‸tle"), QStringLiteral("> ## Ti‸tle"));
    // ATX headings in Setext mode for levels one and two, underlined as wide
    // as the title shows, at least three.
    QCOMPARE(formatted("heading1", "Ti‸tle", setext()), QStringLiteral("Ti‸tle\n====="));
    QCOMPARE(formatted("heading2", "A‸", setext()), QStringLiteral("A‸\n---"));
    QCOMPARE(formatted("heading1", "標題‸", setext()), QStringLiteral("標題‸\n===="));
    QCOMPARE(formatted("heading2", "Ti‸tle\n=====", setext()), QStringLiteral("Ti‸tle\n-----"));
    QCOMPARE(formatted("heading1", "Ti‸tle\n=====", setext()), QStringLiteral("Ti‸tle"));
    QCOMPARE(formatted("heading3", "Ti‸tle\n=====", setext()), QStringLiteral("### Ti‸tle"));
    QCOMPARE(formatted("heading2", "# Ti‸tle\nnext", setext()), QStringLiteral("Ti‸tle\n-----\nnext"));
    QCOMPARE(formatted("paragraph", "Ti‸tle\n---"), QStringLiteral("Ti‸tle"));
    // A Setext heading is read as one whatever the setting.
    QCOMPARE(formatted("heading1", "Ti‸tle\n---"), QStringLiteral("# Ti‸tle"));
    MarkdownEditing::Options both = setext();
    both.newline = QStringLiteral("\r\n");
    QCOMPARE(formatted("heading1", "Ti‸tle\r\nnext", both), QStringLiteral("Ti‸tle\r\n=====\r\nnext"));
}

void blockFormattingTogglesEveryLine()
{
    QCOMPARE(formatted("quote", "a‸"), QStringLiteral("> a‸"));
    QCOMPARE(formatted("quote", "> a‸"), QStringLiteral("a‸"));
    QCOMPARE(formatted("quote", "‸"), QStringLiteral("> ‸"));
    QCOMPARE(formatted("quote", "«a\n\nb»"), QStringLiteral("«> a\n>\n> b»"));
    QCOMPARE(formatted("quote", "«> a\n>\n> b»"), QStringLiteral("«a\n\nb»"));

    QCOMPARE(formatted("bulletList", "a‸"), QStringLiteral("- a‸"));
    QCOMPARE(formatted("bulletList", "- a‸"), QStringLiteral("a‸"));
    QCOMPARE(formatted("bulletList", "‸"), QStringLiteral("- ‸"));
    QCOMPARE(formatted("bulletList", "«a\n\nb»"), QStringLiteral("«- a\n\n- b»"));
    QCOMPARE(formatted("bulletList", "«* a\n- [ ] b»"), QStringLiteral("«* a\n- b»"));
    QCOMPARE(formatted("bulletList", "- [x] a‸"), QStringLiteral("- a‸"));
    QCOMPARE(formatted("numberedList", "«a\nb\nc»"), QStringLiteral("«1. a\n2. b\n3. c»"));
    QCOMPARE(formatted("numberedList", "«- a\n- b»"), QStringLiteral("«1. a\n2. b»"));
    QCOMPARE(formatted("numberedList", "«1. a\n2. b»"), QStringLiteral("«a\nb»"));
    // A new item after a numbered one continues its numbers.
    QCOMPARE(formatted("numberedList", "1. a\n‸b"), QStringLiteral("1. a\n2. ‸b"));
    QCOMPARE(formatted("numberedList", "«a\n  b\nc»"), QStringLiteral("«1. a\n  1. b\n2. c»"));
    QCOMPARE(formatted("taskList", "«a\n1. b»"), QStringLiteral("«- [ ] a\n1. [ ] b»"));
    QCOMPARE(formatted("taskList", "- [ ] a‸"), QStringLiteral("a‸"));
    QCOMPARE(formatted("bulletList", "«a\r\nb»", crlf()), QStringLiteral("«- a\r\n- b»"));

    QCOMPARE(formatted("toggleTask", "- [ ] a‸"), QStringLiteral("- [x] a‸"));
    QCOMPARE(formatted("toggleTask", "- [X] a‸"), QStringLiteral("- [ ] a‸"));
    QCOMPARE(formatted("toggleTask", "- a‸"), QStringLiteral("- [ ] a‸"));
    QCOMPARE(formatted("toggleTask", "a‸"), QStringLiteral("- [ ] a‸"));
    QCOMPARE(formatted("toggleTask", "«- [ ] a\n- [x] b»"), QStringLiteral("«- [x] a\n- [x] b»"));
    QCOMPARE(formatted("toggleTask", "«- [x] a\n- [x] b»"), QStringLiteral("«- [ ] a\n- [ ] b»"));

    QCOMPARE(formatted("codeBlock", "‸"), QStringLiteral("```\n‸\n```"));
    QCOMPARE(formatted("codeBlock", "«a\nb»"), QStringLiteral("```\n«a\nb»\n```"));
    QCOMPARE(formatted("codeBlock", "```\n«a\nb»\n```"), QStringLiteral("«a\nb»"));
    QCOMPARE(formatted("codeBlock", "«```\na\n```»"), QStringLiteral("«a»"));
    QCOMPARE(formatted("codeBlock", "x\n```\na‸\n```\ny"), QStringLiteral("x\na‸\ny"));
    QCOMPARE(formatted("codeBlock", "«```\nx\n```»\n"), QStringLiteral("«x»\n"));
    QCOMPARE(formatted("codeBlock", "«~~~\nx\n~~~»"), QStringLiteral("«x»"));
    QCOMPARE(formatted("codeBlock", "«a\n```b»"), QStringLiteral("````\n«a\n```b»\n````"));
    QCOMPARE(formatted("codeBlock", "«a\r\nb»", crlf()), QStringLiteral("```\r\n«a\r\nb»\r\n```"));

    QCOMPARE(formatted("displayMath", "‸"), QStringLiteral("$$\n‸\n$$"));
    QCOMPARE(formatted("displayMath", "«a\nb»"), QStringLiteral("$$\n«a\nb»\n$$"));
    QCOMPARE(formatted("displayMath", "$$\n«a»\n$$"), QStringLiteral("«a»"));
    QCOMPARE(formatted("displayMath", "x «a+b» y"), QStringLiteral("x $$«a+b»$$ y"));
    QCOMPARE(formatted("displayMath", "x $$«a+b»$$ y"), QStringLiteral("x «a+b» y"));
}

void insertionsKeepTheirBlankLines()
{
    // A thematic break has a blank line before it, so that it can never
    // underline the paragraph above as a heading.
    QCOMPARE(formatted("thematicBreak", "a‸"), QStringLiteral("a\n\n---\n‸"));
    QCOMPARE(formatted("thematicBreak", "a\n‸\nb"), QStringLiteral("a\n\n---\n‸\nb"));
    QCOMPARE(formatted("thematicBreak", "‸"), QStringLiteral("---\n‸"));
    QCOMPARE(formatted("thematicBreak", "a‸\nb"), QStringLiteral("a\n\n---\n‸\nb"));
    QCOMPARE(formatted("pageBreak", "a‸"), QStringLiteral("a\n\n+++\n‸"));
    QCOMPARE(formatted("thematicBreak", "a‸", crlf()), QStringLiteral("a\r\n\r\n---\r\n‸"));

    QCOMPARE(formatted("table", "‸"), QStringLiteral("| «Column 1» | Column 2 |\n| -------- | -------- |\n|          |          |\n"));
    QCOMPARE(formatted("table", "Intro‸"),
             QStringLiteral("Intro\n\n| «Column 1» | Column 2 |\n| -------- | -------- |\n|          |          |\n"));

    // A footnote's reference goes after the selection and its definition to
    // the end, where the caret goes to write it.
    QCOMPARE(formatted("footnote", "Text‸"), QStringLiteral("Text[^1]\n\n[^1]: ‸"));
    QCOMPARE(formatted("footnote", "Text‸\n"), QStringLiteral("Text[^1]\n\n[^1]: ‸\n"));
    QCOMPARE(formatted("footnote", "«word» more"), QStringLiteral("word[^1] more\n\n[^1]: ‸"));
    QCOMPARE(formatted("footnote", "A[^1] B‸\n\n[^1]: x\n"), QStringLiteral("A[^1] B[^2]\n\n[^1]: x\n[^2]: ‸\n"));
    QCOMPARE(formatted("footnote", "Text‸\r\n", crlf()), QStringLiteral("Text[^1]\r\n\r\n[^1]: ‸\r\n"));
}

void smartKeysContinueAndIndentLists()
{
    const auto enter = [](const QString &marked, const MarkdownEditing::Options &options = {}) {
        const Marked start = unmark(marked);
        return applied(marked, MarkdownEditing::newLine(start.text, start.anchor, start.cursor, options));
    };
    QCOMPARE(enter("- a‸"), QStringLiteral("- a\n- ‸"));
    QCOMPARE(enter("* a‸"), QStringLiteral("* a\n* ‸"));
    QCOMPARE(enter("1. a‸"), QStringLiteral("1. a\n2. ‸"));
    QCOMPARE(enter("9) a‸"), QStringLiteral("9) a\n10) ‸"));
    QCOMPARE(enter("- [x] a‸"), QStringLiteral("- [x] a\n- [ ] ‸"));
    QCOMPARE(enter("  - a‸"), QStringLiteral("  - a\n  - ‸"));
    QCOMPARE(enter("> - a‸"), QStringLiteral("> - a\n> - ‸"));
    QCOMPARE(enter("- a‸ b"), QStringLiteral("- a\n- ‸b"));
    QCOMPARE(enter("- a‸", crlf()), QStringLiteral("- a\r\n- ‸"));
    // Enter on an empty item ends the list.
    QCOMPARE(enter("- a\n- ‸"), QStringLiteral("- a\n‸"));
    QCOMPARE(enter("1. a\n2. ‸"), QStringLiteral("1. a\n‸"));
    QCOMPARE(enter("- [ ] ‸"), QStringLiteral("‸"));
    // Elsewhere Enter is a plain line break.
    QCOMPARE(enter("plain‸"), QStringLiteral("(nothing)"));
    QCOMPARE(enter("‸- a"), QStringLiteral("(nothing)"));
    QCOMPARE(enter("---‸"), QStringLiteral("(nothing)"));

    const Marked line = unmark(QStringLiteral("a‸b"));
    QCOMPARE(applied(QStringLiteral("a‸b"), MarkdownEditing::hardBreak(line.text, line.anchor, line.cursor)),
             QStringLiteral("a  \n‸b"));
    QCOMPARE(applied(QStringLiteral("a‸b"), MarkdownEditing::hardBreak(line.text, line.anchor, line.cursor, crlf())),
             QStringLiteral("a  \r\n‸b"));

    const auto tab = [](const QString &marked, bool outdent = false) {
        const Marked start = unmark(marked);
        return applied(marked, MarkdownEditing::indent(start.text, start.anchor, start.cursor, outdent));
    };
    QCOMPARE(tab("- a\n- b‸"), QStringLiteral("- a\n  - b‸"));
    QCOMPARE(tab("1. a\n2. b‸"), QStringLiteral("1. a\n   2. b‸"));
    QCOMPARE(tab("- a\n  - b‸", true), QStringLiteral("- a\n- b‸"));
    QCOMPARE(tab("- a‸"), QStringLiteral("  - a‸"));
    QCOMPARE(tab("\t- a‸"), QStringLiteral("\t\t- a‸"));
    QCOMPARE(tab("\t- a‸", true), QStringLiteral("- a‸"));
    QCOMPARE(tab("«a\nb»"), QStringLiteral("«\ta\n\tb»"));
    QCOMPARE(tab("«\ta\n    b»", true), QStringLiteral("«a\nb»"));
    QCOMPARE(tab("plain‸"), QStringLiteral("(nothing)"));
    QCOMPARE(tab("plain‸", true), QStringLiteral("(nothing)"));
}

void tablesNavigateAndFormat()
{
    const auto move = [](const QString &marked, bool backwards = false) {
        const Marked start = unmark(marked);
        return applied(marked, MarkdownEditing::moveToCell(start.text, start.anchor, start.cursor, backwards));
    };
    const QString table = QStringLiteral("| a | b |\n|---|---|\n| c | d |");
    QCOMPARE(move("| a‸ | b |\n|---|---|\n| c | d |"), QStringLiteral("| a | «b» |\n|---|---|\n| c | d |"));
    QCOMPARE(move("| a | b‸ |\n|---|---|\n| c | d |"), QStringLiteral("| a | b |\n|---|---|\n| «c» | d |"));
    QCOMPARE(move("| a | b |\n|---|---|\n| c | d‸ |"), QStringLiteral("| a | b |\n|---|---|\n| c | d |\n| ‸ |  |"));
    QCOMPARE(move("| a | b‸ |\n|---|---|\n| c | d |", true), QStringLiteral("| «a» | b |\n|---|---|\n| c | d |"));
    QCOMPARE(move("| a | b |\n|---|---|\n| ‸c | d |", true), QStringLiteral("| a | «b» |\n|---|---|\n| c | d |"));
    QCOMPARE(move("| ‸a | b |\n|---|---|\n| c | d |", true), QStringLiteral("| ‸a | b |\n|---|---|\n| c | d |"));
    QCOMPARE(move("a | b‸\n--|--\nc | d"), QStringLiteral("a | b\n--|--\n«c» | d"));
    QCOMPARE(move("not | a table‸"), QStringLiteral("(nothing)"));
    Q_UNUSED(table);

    const auto format = [](const QString &marked) {
        const Marked start = unmark(marked);
        return applied(marked, MarkdownEditing::formatTable(start.text, start.anchor, start.cursor));
    };
    // Alignment from the delimiter row; the caret stays in its cell.
    QCOMPARE(format("|a|b|\n|:-|-:|\n|lo‸ng cell|x|"),
             QStringLiteral("| a         |   b |\n| :-------- | --: |\n| lo‸ng cell |   x |"));
    QCOMPARE(format("a|b|c\n:-:|---|:--\n‸x|yy|z"),
             QStringLiteral("|  a  | b   | c   |\n| :-: | --- | :-- |\n|  ‸x  | yy  | z   |"));
    // Escaped pipes and pipes in code spans are text, not boundaries.
    QCOMPARE(format("| a \\| b | c |\n|---|---|\n| x‸ | y |"),
             QStringLiteral("| a \\| b | c   |\n| ------ | --- |\n| x‸      | y   |"));
    QCOMPARE(format("| `a|b` | c |\n|---|---|\n| x‸ | y |"),
             QStringLiteral("| `a|b` | c   |\n| ----- | --- |\n| x‸     | y   |"));
    // Wide characters take two columns.
    QCOMPARE(format("| 名前 | x |\n|---|---|\n| ab‸ | y |"),
             QStringLiteral("| 名前 | x   |\n| ---- | --- |\n| ab‸   | y   |"));
    // A short row gains empty cells and a long one keeps its extra cells.
    QCOMPARE(format("| a | b |\n|---|---|\n| x‸ |\n| 1 | 2 | 3 |"),
             QStringLiteral("| a   | b   |\n| --- | --- |\n| x‸   |     |\n| 1   | 2   | 3   |"));
    // Indentation and a quote stay; a table in a CRLF document keeps its breaks.
    QCOMPARE(format("> |a|b|\n> |-|-|\n> |c‸|d|"), QStringLiteral("> | a   | b   |\n> | --- | --- |\n> | c‸   | d   |"));
    QCOMPARE(format("|a|b|\r\n|-|-|\r\n|c‸|d|\r\n"), QStringLiteral("| a   | b   |\r\n| --- | --- |\r\n| c‸   | d   |\r\n"));
    // A formatted table is left as it is, and so is text that is no table.
    QCOMPARE(format("| a   | b   |\n| --- | --- |\n| c‸   | d   |"), QStringLiteral("(nothing)"));
    QCOMPARE(format("a | b‸"), QStringLiteral("(nothing)"));
    QCOMPARE(MarkdownEditing::displayWidth(QStringLiteral("e\u0301名👩\u200d💻")), 5);
}

void everyCommandIsOneStepOrNothing()
{
    // Every command, on several texts and selections, makes one undo step
    // that undoes exactly, or nothing, and never leaves the selection outside the text.
    const QStringList texts{QStringLiteral("‸"), QStringLiteral("plain‸ text"), QStringLiteral("«two\nlines»"),
                            QStringLiteral("- [ ] «task»\n- item"), QStringLiteral("| a | b‸ |\n|---|---|"),
                            QStringLiteral("«line\r\nwith crlf»\r\n"), QStringLiteral("# Head‸ing\n\ntext"),
                            QStringLiteral("e\u0301«x\u0301»")};
    for (const QString &command : MarkdownEditing::commands()) {
        QVERIFY(MarkdownEditing::isCommand(command));
        for (const QString &text : texts) {
            const QString result = formatted(command, text);
            QVERIFY2(!result.startsWith(u'(') || result == QStringLiteral("(nothing)"),
                     qPrintable(command + u' ' + text + QStringLiteral(" -> ") + result));
        }
    }
    QVERIFY(!MarkdownEditing::isCommand(QStringLiteral("quit")));
    QCOMPARE(MarkdownEditing::format(QStringLiteral("quit"), u"x", 0, 0), std::nullopt);
}

void editorRunsCommandsAsOneUndoStep()
{
    Editor view(QStringLiteral("one two\nthree"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.select(4, 7);
    QVERIFY(view.binding->format(QStringLiteral("bold")));
    QCOMPARE(view.marked(), QStringLiteral("one **«two»**\nthree"));
    // A selection over lines, made backwards, stays backwards.
    view.select(17, 0);
    QVERIFY(view.binding->format(QStringLiteral("bulletList")));
    QCOMPARE(view.marked(), QStringLiteral("»- one **two**\n- three«"));
    view.binding->undo();
    QCOMPARE(view.text(), QStringLiteral("one **two**\nthree"));
    view.binding->undo();
    QCOMPARE(view.text(), QStringLiteral("one two\nthree"));
    // An unknown command, or one that changes nothing, does nothing.
    view.select(0, 0);
    QVERIFY(!view.binding->format(QStringLiteral("quit")));
    QVERIFY(!view.binding->format(QStringLiteral("paragraph")));
    QCOMPARE(view.text(), QStringLiteral("one two\nthree"));
    // Setext headings once the view is told to write them, in the document's own breaks.
    view.pane->setProperty("setextHeadings", true);
    QVERIFY(view.binding->format(QStringLiteral("heading1")));
    QCOMPARE(view.text(), QStringLiteral("one two\n=======\nthree"));

    Editor crlf(QStringLiteral("a\r\nb\r\n"));
    QVERIFY2(crlf.ok(), qPrintable(crlf.error));
    crlf.select(0, 4);
    QVERIFY(crlf.binding->format(QStringLiteral("codeBlock")));
    QCOMPARE(crlf.text(), QStringLiteral("```\r\na\r\nb\r\n```\r\n"));
    QCOMPARE(crlf.marked(), QStringLiteral("```\r\n«a\r\nb»\r\n```\r\n"));

    // A read-only document takes no command.
    Editor invalid{QString()};
    QVERIFY2(invalid.ok(), qPrintable(invalid.error));
    writeFile(invalid.root.filePath(QStringLiteral("bytes.md")), QByteArray("caf\xe9\n"));
    Document *readOnly = invalid.store->open(invalid.root.filePath(QStringLiteral("bytes.md")));
    QCOMPARE(readOnly->state(), Document::State::ReadOnly);
    invalid.pane->setProperty("document", QVariant::fromValue(readOnly));
    QTRY_VERIFY(invalid.binding->document() == readOnly);
    QVERIFY(!invalid.binding->editable());
    const QString before = readOnly->buffer()->text();
    QVERIFY(!invalid.binding->format(QStringLiteral("bold")));
    QCOMPARE(readOnly->buffer()->text(), before);
    QVERIFY(!readOnly->buffer()->canUndo());
}

void editorKeysContinueListsAndTables()
{
    Editor view(QStringLiteral("- one"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.select(5, 5);
    view.key(Qt::Key_Return);
    QCOMPARE(view.marked(), QStringLiteral("- one\n- ‸"));
    // Tab nests the new item under the one above; Shift+Tab brings it back.
    view.key(Qt::Key_Tab);
    QCOMPARE(view.marked(), QStringLiteral("- one\n  - ‸"));
    view.key(Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(view.marked(), QStringLiteral("- one\n- ‸"));
    // Enter on the empty item ends the list; each key was one step.
    view.key(Qt::Key_Return);
    QCOMPARE(view.marked(), QStringLiteral("- one\n‸"));
    view.binding->undo();
    QCOMPARE(view.marked(), QStringLiteral("- one\n- ‸"));
    view.binding->undo();
    view.binding->undo();
    view.binding->undo();
    QCOMPARE(view.text(), QStringLiteral("- one"));
    // Shift+Enter is a hard line break, one edit.
    view.select(5, 5);
    view.key(Qt::Key_Return, Qt::ShiftModifier);
    QCOMPARE(view.marked(), QStringLiteral("- one  \n‸"));
    view.binding->undo();
    QCOMPARE(view.text(), QStringLiteral("- one"));

    // Elsewhere Tab types a tab and Enter a plain break.
    Editor plain(QStringLiteral("word"));
    QVERIFY2(plain.ok(), qPrintable(plain.error));
    plain.select(4, 4);
    plain.key(Qt::Key_Tab);
    plain.key(Qt::Key_Return);
    QCOMPARE(plain.marked(), QStringLiteral("word\t\n‸"));

    // In a table Tab and Shift+Tab go from cell to cell, past the delimiter
    // row, and only select.
    Editor table(QStringLiteral("| a | b |\n|---|---|\n| c | d |"));
    QVERIFY2(table.ok(), qPrintable(table.error));
    table.select(3, 3);
    table.key(Qt::Key_Tab);
    QCOMPARE(table.marked(), QStringLiteral("| a | «b» |\n|---|---|\n| c | d |"));
    table.key(Qt::Key_Tab);
    QCOMPARE(table.marked(), QStringLiteral("| a | b |\n|---|---|\n| «c» | d |"));
    table.key(Qt::Key_Backtab, Qt::ShiftModifier);
    QCOMPARE(table.marked(), QStringLiteral("| a | «b» |\n|---|---|\n| c | d |"));
    QVERIFY(!table.document->buffer()->canUndo());
}

namespace {
QKeySequence keys(const char *text) { return QKeySequence(QString::fromLatin1(text)); }

QStringList keysOf(const CommandRegistry &registry, const QString &id)
{
    QStringList texts;
    for (const QKeySequence &sequence : registry.shortcuts(id)) texts.append(sequence.toString(QKeySequence::PortableText));
    return texts;
}

void addStandard(CommandRegistry &registry)
{
    for (const CommandInfo &info : standardCommands()) registry.add(info);
}
} // namespace

void registryGivesEveryKeyOneCommand()
{
    QTemporaryDir root;
    const QString file = root.filePath(QStringLiteral("settings.json"));
    Settings settings(file);
    CommandRegistry registry(&settings);
    addStandard(registry);
    // Every formatting command is there, and no key is two commands' or a typing key.
    for (const QString &command : MarkdownEditing::commands()) {
        QVERIFY2(registry.contains(command), qPrintable(command));
        QCOMPARE(registry.needs(command), QStringLiteral("text"));
    }
    QSet<QKeySequence> seen;
    for (const QString &id : registry.ids()) {
        QVERIFY(!registry.title(id).isEmpty());
        for (const QKeySequence &sequence : registry.shortcuts(id)) {
            QVERIFY2(!CommandRegistry::isTypingKey(sequence), qPrintable(id));
            QVERIFY2(!seen.contains(sequence), qPrintable(id + u' ' + sequence.toString()));
            seen.insert(sequence);
        }
    }
    QCOMPARE(keysOf(registry, QStringLiteral("palette")), QStringList{QStringLiteral("Ctrl+Shift+P")});
    QCOMPARE(registry.keys().value(QStringLiteral("save")).toString(), keys("Ctrl+S").toString(QKeySequence::NativeText));

    // Refusals: keys that type, keys the editor keeps, and keys taken.
    const auto problem = [&](const char *id, const char *sequence) {
        return registry.check(QString::fromLatin1(id), QString::fromLatin1(sequence)).value(QStringLiteral("problem")).toString();
    };
    QCOMPARE(problem("bold", "B"), QStringLiteral("typing"));
    QCOMPARE(problem("bold", "Shift+B"), QStringLiteral("typing"));
    QCOMPARE(problem("bold", "Tab"), QStringLiteral("typing"));
    QCOMPARE(problem("bold", "Escape"), QStringLiteral("reserved"));
    QCOMPARE(problem("bold", "Ctrl+C"), QStringLiteral("reserved"));
    QCOMPARE(problem("bold", "Ctrl+Z"), QStringLiteral("reserved"));
    QCOMPARE(problem("undo", "Ctrl+Z"), QString());
    QCOMPARE(problem("bold", ""), QStringLiteral("invalid"));
    QCOMPARE(problem("bold", "Ctrl+I"), QStringLiteral("taken"));
    QCOMPARE(registry.check(QStringLiteral("bold"), QStringLiteral("Ctrl+I")).value(QStringLiteral("takenByTitle")).toString(),
             QStringLiteral("Italic"));
    QVERIFY(registry.check(QStringLiteral("bold"), QStringLiteral("F7")).value(QStringLiteral("allowed")).toBool());
    QVERIFY(!registry.bind(QStringLiteral("bold"), QStringLiteral("B")));
    QVERIFY(!registry.bind(QStringLiteral("bold"), QStringLiteral("Ctrl+I")));
    QCOMPARE(keysOf(registry, QStringLiteral("bold")), QStringList{QStringLiteral("Ctrl+B")});

    // A new key replaces the command's keys and is kept in the settings.
    QSignalSpy changed(&registry, &CommandRegistry::shortcutsChanged);
    QVERIFY(registry.bind(QStringLiteral("bold"), QStringLiteral("Ctrl+Alt+B")));
    QCOMPARE(keysOf(registry, QStringLiteral("bold")), QStringList{QStringLiteral("Ctrl+Alt+B")});
    QCOMPARE(changed.count(), 1);
    {
        Settings again(file);
        CommandRegistry restarted(&again);
        addStandard(restarted);
        QCOMPARE(keysOf(restarted, QStringLiteral("bold")), QStringList{QStringLiteral("Ctrl+Alt+B")});
        QVERIFY(restarted.index(restarted.ids().indexOf(QStringLiteral("bold"))).data(CommandRegistry::CustomizedRole).toBool());
    }
    // Swapping: Bold takes Italic's key, and Italic takes Bold's.
    QVERIFY(registry.swap(QStringLiteral("bold"), QStringLiteral("Ctrl+I")));
    QCOMPARE(keysOf(registry, QStringLiteral("bold")), QStringList{QStringLiteral("Ctrl+I")});
    QCOMPARE(keysOf(registry, QStringLiteral("italic")), QStringList{QStringLiteral("Ctrl+Alt+B")});
    // Swapping a command that has keys of its own besides keeps the rest.
    QVERIFY(registry.swap(QStringLiteral("library"), QStringLiteral("Ctrl+PgDown")));
    QCOMPARE(keysOf(registry, QStringLiteral("nextTab")), (QStringList{QStringLiteral("Ctrl+Tab"), QStringLiteral("F9")}));
    QCOMPARE(keysOf(registry, QStringLiteral("library")), QStringList{QStringLiteral("Ctrl+PgDown")});
    // Resetting gives the defaults back, from whichever command had them.
    registry.reset(QStringLiteral("italic"));
    QCOMPARE(keysOf(registry, QStringLiteral("italic")), QStringList{QStringLiteral("Ctrl+I")});
    QCOMPARE(keysOf(registry, QStringLiteral("bold")), QStringList());
    registry.reset(QStringLiteral("bold"));
    QCOMPARE(keysOf(registry, QStringLiteral("bold")), QStringList{QStringLiteral("Ctrl+B")});
    // A command can be left without keys; it then has no shortcut at all.
    registry.unbind(QStringLiteral("save"));
    QVERIFY(registry.shortcuts(QStringLiteral("save")).isEmpty());
    QVERIFY(!registry.keys().contains(QStringLiteral("save")));
    QAbstractItemModel *bound = registry.bound();
    QStringList boundIds;
    for (int i = 0; i < bound->rowCount(); ++i) boundIds << bound->index(i, 0).data(CommandRegistry::IdRole).toString();
    QVERIFY(!boundIds.contains(QStringLiteral("save")));
    QVERIFY(boundIds.contains(QStringLiteral("bold")));
    QVERIFY(!boundIds.contains(QStringLiteral("paragraph")));
    registry.resetAll();
    QCOMPARE(keysOf(registry, QStringLiteral("save")), QStringList{QStringLiteral("Ctrl+S")});
    QCOMPARE(keysOf(registry, QStringLiteral("library")), QStringList{QStringLiteral("F9")});
    // A row for every key.
    int keyCount = 0;
    for (const QString &id : registry.ids()) keyCount += int(registry.shortcuts(id).size());
    QCOMPARE(bound->rowCount(), keyCount);
    QVERIFY(!readFile(file).contains("shortcuts"));

    // A key press, as the dialog records it.
    QCOMPARE(registry.sequenceOf(Qt::Key_B, Qt::ControlModifier), QStringLiteral("Ctrl+B"));
    QCOMPARE(registry.sequenceOf(Qt::Key_Control, Qt::ControlModifier), QString());
    QCOMPARE(registry.sequenceOf(Qt::Key_Backtab, Qt::ControlModifier | Qt::ShiftModifier), QStringLiteral("Ctrl+Shift+Tab"));
}

void registryReadsDamagedKeysAndRunsActions()
{
    QTemporaryDir root;
    const QString file = root.filePath(QStringLiteral("settings.json"));
    // The writer's own keys win over defaults; a value that is no list of
    // keys is ignored; a typing key is never used; an unknown command's keys
    // are kept for the version that knows it.
    writeFile(file, R"({"shortcuts": {"bold": ["Ctrl+S"], "italic": "Ctrl+J", "quote": ["Q"], "later": ["Ctrl+J"]}})");
    Settings settings(file);
    CommandRegistry registry(&settings);
    addStandard(registry);
    QCOMPARE(keysOf(registry, QStringLiteral("bold")), QStringList{QStringLiteral("Ctrl+S")});
    QVERIFY(registry.shortcuts(QStringLiteral("save")).isEmpty());
    QCOMPARE(keysOf(registry, QStringLiteral("italic")), QStringList{QStringLiteral("Ctrl+I")});
    QVERIFY(registry.shortcuts(QStringLiteral("quote")).isEmpty());
    QVERIFY(registry.bind(QStringLiteral("save"), QStringLiteral("Ctrl+Alt+S")));
    QVERIFY(readFile(file).contains("later"));

    // An action runs for its window when the command can run there.
    QStringList ran;
    registry.add({QStringLiteral("anything"), QStringLiteral("Anything"), QStringLiteral("Test"), {}, CommandInfo::Needs::Nothing},
                 [&](WindowContext *) { ran << QStringLiteral("anything"); });
    registry.setAction(QStringLiteral("save"), [&](WindowContext *) { ran << QStringLiteral("save"); });
    QVERIFY(registry.run(QStringLiteral("anything"), nullptr));
    QCOMPARE(ran, QStringList{QStringLiteral("anything")});
    // Without a document in front there is nothing to save.
    QVERIFY(!registry.canRun(QStringLiteral("save"), nullptr));
    QVERIFY(registry.run(QStringLiteral("save"), nullptr));
    QCOMPARE(ran, QStringList{QStringLiteral("anything")});
    // A command the window runs itself is not run here.
    QVERIFY(!registry.run(QStringLiteral("bold"), nullptr));
    QVERIFY(!registry.run(QStringLiteral("nothing such"), nullptr));
}

namespace {

bool isOpen(const QObject *popup) { return popup && popup->property("opened").toBool(); }

// Runs a command in a window, as its shortcuts, menu, palette and toolbar do.
void runIn(QQuickWindow *window, const char *command)
{
    QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
}

bool within(QQuickItem *item, const QQuickItem *ancestor)
{
    for (; item; item = item->parentItem())
        if (item == ancestor) return true;
    return false;
}

// Tab, from the control with the keys in a dialog, reaches every visible
// control in it that takes focus or that a person operates, and each that
// shows keyboard focus shows it. Names what fails.
QStringList unreachedIn(QQuickWindow *window, const QObject *dialog)
{
    auto *content = dialog->property("contentItem").value<QQuickItem *>();
    QQuickItem *root = content ? content->parentItem() : nullptr;
    if (!root) return {QStringLiteral("the dialog has no content")};
    QList<QQuickItem *> controls;
    std::function<void(QQuickItem *)> visit = [&](QQuickItem *item) {
        if (!item->isVisible() || item->opacity() <= 0) return;
        QAccessibleInterface *access = QAccessible::queryAccessibleInterface(item);
        if (item->isEnabled() && item->width() > 0 && item->height() > 0
            && (item->activeFocusOnTab() || (access && isInteractiveRole(access->role()))))
            controls << item;
        for (QQuickItem *child : item->childItems()) visit(child);
    };
    visit(root);
    QStringList problems;
    QList<QQuickItem *> reached;
    for (int i = 0; i <= 2 * controls.size() + 2; ++i) {
        QQuickItem *now = window->activeFocusItem();
        if (!now || reached.contains(now)) break;
        reached << now;
        if (now->property("visualFocus").isValid() && !now->property("visualFocus").toBool())
            problems << describe(now) + QStringLiteral(" takes focus unshown");
        QTest::keyClick(window, Qt::Key_Tab);
    }
    for (QQuickItem *control : std::as_const(controls)) {
        const bool found = std::any_of(reached.cbegin(), reached.cend(), [&](QQuickItem *item) { return within(item, control); });
        if (!found) problems << describe(control) + QStringLiteral(" is not reached by Tab");
    }
    return problems;
}

QString countShown(const ShellFixture &shell)
{
    QQuickItem *count = shell.item(QStringLiteral("wordCount"));
    return count && count->isVisible() ? count->property("text").toString() : QStringLiteral("(hidden)");
}

} // namespace

void paletteFindsAndRunsCommands()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    typeText(window, QStringLiteral("hello"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    selectRange(editor, 0, 5);
    QObject *palette = shell.object(QStringLiteral("commandPalette"));
    QVERIFY(palette);
    // Ctrl+Shift+P opens it with the keys in its field.
    QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(isOpen(palette));
    QVERIFY(window->property("dialogOpen").toBool());
    QQuickItem *field = shell.item(QStringLiteral("paletteField"));
    if (!field) field = qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("paletteField")));
    QVERIFY(field);
    QTRY_COMPARE(window->activeFocusItem(), field);
    auto *matches = qobject_cast<CommandFilter *>(findObject(window, QStringLiteral("paletteMatches")));
    QVERIFY(matches);
    // Everything, with nothing typed; the best match first as letters are typed.
    QCOMPARE(matches->count(), int(shell.backend->commands()->ids().size()));
    typeText(window, QStringLiteral("bold"));
    QCOMPARE(field->property("text").toString(), QStringLiteral("bold"));
    QCOMPARE(matches->idAt(0), QStringLiteral("bold"));
    // Letters in order find a command, best where they start its words.
    field->setProperty("text", QStringLiteral("nwd"));
    QVERIFY(matches->rowOf(QStringLiteral("newDraft")) >= 0 && matches->rowOf(QStringLiteral("newDraft")) < 3);
    QVERIFY(matches->rowOf(QStringLiteral("newWindow")) >= 0 && matches->rowOf(QStringLiteral("newWindow")) < 3);
    QCOMPARE(matches->rowOf(QStringLiteral("bold")), -1);
    QVERIFY(CommandFilter::score(QStringLiteral("hea"), QStringLiteral("Heading 1"))
            > CommandFilter::score(QStringLiteral("hea"), QStringLiteral("Show or hide toolbar")));
    QCOMPARE(CommandFilter::score(QStringLiteral("zz"), QStringLiteral("Bold")), -1);
    // The category finds commands too: every formatting command for "format".
    field->setProperty("text", QStringLiteral("format"));
    QVERIFY(matches->rowOf(QStringLiteral("italic")) >= 0);
    QCOMPARE(matches->idAt(0), QStringLiteral("formatTable"));
    // Assistive technology hears the chosen command; nothing is unnamed.
    field->setProperty("text", QStringLiteral("ital"));
    QCOMPARE(matches->idAt(0), QStringLiteral("italic"));
    QAccessibleInterface *access = QAccessible::queryAccessibleInterface(field);
    QVERIFY(access);
    QTRY_VERIFY(access->text(QAccessible::Description).startsWith(QStringLiteral("Italic, Format, ")));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    // The arrows move the choice and Enter runs it, on the editor's selection.
    field->setProperty("text", QStringLiteral("bold"));
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Up);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!isOpen(palette));
    QTRY_COMPARE(shell.document()->buffer()->text(), QStringLiteral("**hello**"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    // Escape closes it, running nothing.
    QTest::keyClick(window, Qt::Key_P, Qt::ControlModifier | Qt::ShiftModifier);
    QTRY_VERIFY(isOpen(palette));
    typeText(window, QStringLiteral("italic"));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(palette));
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("**hello**"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    // The menu opens it too.
    QObject *menu = shell.object(QStringLiteral("windowMenu"));
    QVERIFY(QMetaObject::invokeMethod(menu, "commandChosen", Q_ARG(QString, QStringLiteral("palette"))));
    QTRY_VERIFY(isOpen(palette));
    QCOMPARE(field->property("text").toString(), QString());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(palette));
}

void shortcutsDialogRebindsSwapsAndResets()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    CommandRegistry *commands = shell.backend->commands();
    const auto keysOf = [&](const char *id) { return commands->keys().value(QString::fromLatin1(id)).toString(); };
    const QString italicKeys = keysOf("italic"), boldKeys = keysOf("bold");
    QObject *dialog = shell.object(QStringLiteral("shortcutsDialog"));
    QVERIFY(dialog);
    runIn(window, "shortcuts");
    QTRY_VERIFY(isOpen(dialog));
    auto *field = qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("shortcutsField")));
    auto *list = qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("shortcutsList")));
    auto *said = qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("shortcutsMessage")));
    QVERIFY(field && list && said);
    QTRY_COMPARE(window->activeFocusItem(), field);
    // Every control is named and reached by Tab.
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    const QStringList unreached = unreachedIn(window, dialog);
    QVERIFY2(unreached.isEmpty(), qPrintable(unreached.join("; ")));
    field->forceActiveFocus(Qt::TabFocusReason);
    // Found by name, then recorded from the list with Enter.
    typeText(window, QStringLiteral("Bold"));
    QCOMPARE(dialog->property("current").toString(), QStringLiteral("bold"));
    QTest::keyClick(window, Qt::Key_Down);
    QTRY_VERIFY(within(window->activeFocusItem(), list));
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(dialog->property("recording").toString(), QStringLiteral("bold"));
    // A key that types, and one the editor keeps, are refused with the reason.
    QTest::keyClick(window, Qt::Key_B);
    QVERIFY2(said->property("text").toString().contains(QStringLiteral("types text")), qPrintable(said->property("text").toString()));
    QTest::keyClick(window, Qt::Key_C, Qt::ControlModifier);
    QVERIFY2(said->property("text").toString().contains(QStringLiteral("kept for editing")), qPrintable(said->property("text").toString()));
    QCOMPARE(keysOf("bold"), boldKeys);
    // Italic's key is offered to swap; Escape before that keeps the keys.
    QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
    QVERIFY2(said->property("text").toString().contains(QStringLiteral("Italic")), qPrintable(said->property("text").toString()));
    auto *swap = qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("swapKeys")));
    QVERIFY(swap && swap->isVisible());
    QTRY_COMPARE(window->activeFocusItem(), swap);
    QTest::keyClick(window, Qt::Key_Return);
    QCOMPARE(keysOf("bold"), italicKeys);
    QCOMPARE(keysOf("italic"), boldKeys);
    QCOMPARE(dialog->property("recording").toString(), QString());
    QVERIFY(shell.settings()->shortcutOverrides().contains(QStringLiteral("bold")));
    // Recording, Escape leaves the keys and the dialog as they were.
    QTRY_VERIFY(within(window->activeFocusItem(), list));
    QTest::keyClick(window, Qt::Key_Return);
    QTest::keyClick(window, Qt::Key_Escape);
    QVERIFY(isOpen(dialog));
    QCOMPARE(keysOf("bold"), italicKeys);
    // The new key runs Bold in the window.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    typeText(window, QStringLiteral("hi"));
    selectRange(shell.item(QStringLiteral("editor")), 0, 2);
    QTest::keySequence(window, QKeySequence(italicKeys));
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("**hi**"));
    // Remove, then Reset all.
    runIn(window, "shortcuts");
    QTRY_VERIFY(isOpen(dialog));
    typeText(window, QStringLiteral("Bold"));
    QMetaObject::invokeMethod(qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("removeKeys"))), "clicked");
    QVERIFY(!commands->keys().contains(QStringLiteral("bold")));
    QMetaObject::invokeMethod(qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("resetAllKeys"))), "clicked");
    QCOMPARE(keysOf("bold"), boldKeys);
    QCOMPARE(keysOf("italic"), italicKeys);
    QVERIFY(shell.settings()->shortcutOverrides().isEmpty());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
}

void preferencesApplyLiveToEveryWindow()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    Settings *settings = shell.settings();
    QObject *dialog = shell.object(QStringLiteral("preferencesDialog"));
    runIn(window, "preferences");
    QTRY_VERIFY(isOpen(dialog));
    // Every control is named and reached by Tab, scrolled into view.
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    const QStringList unreached = unreachedIn(window, dialog);
    QVERIFY2(unreached.isEmpty(), qPrintable(unreached.join("; ")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    // A second window, which follows every change as the first does.
    shell.backend->newWindow();
    QQuickWindow *second = shell.windowFor(shell.backend->windows().at(1));
    QVERIFY(second);
    QTRY_VERIFY(second->isActive());
    activate(window);
    runIn(window, "preferences");
    QTRY_VERIFY(isOpen(dialog));
    const auto control = [&](const char *name) { return qobject_cast<QQuickItem *>(findObject(window, QString::fromLatin1(name))); };
    const auto panes = [&] {
        return QList<QQuickItem *>{findItem(window->contentItem(), QStringLiteral("editorPane")),
                                   findItem(second->contentItem(), QStringLiteral("editorPane"))};
    };

    // Line length, from the keyboard: each step applies at once in both windows.
    QQuickItem *length = control("lineLengthChoice");
    QVERIFY(length);
    length->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(settings->maxLineLength(), 73);
    for (QQuickItem *pane : panes()) QCOMPARE(pane->property("maxLineLength").toInt(), 73);
    // Line height, in tenths.
    QQuickItem *height = control("lineHeightChoice");
    height->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(settings->lineHeight(), 1.6);
    for (QQuickItem *pane : panes()) QCOMPARE(pane->property("lineHeight").toReal(), 1.6);
    // Text size is the zoom, and the theme's size comes back.
    const int size = settings->fontPixelSize();
    QQuickItem *textSize = control("textSizeChoice");
    textSize->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Up);
    QCOMPARE(settings->fontPixelSize(), size + 1);
    QVERIFY(settings->zoomed());
    for (QQuickItem *pane : panes()) QCOMPARE(pane->property("fontPixelSize").toInt(), size + 1);
    QMetaObject::invokeMethod(control("themeTextSize"), "clicked");
    QVERIFY(!settings->zoomed());
    // Any installed family, and the theme's again.
    QQuickItem *family = control("fontFamilyChoice");
    QVERIFY(family);
    const QStringList families = family->property("model").toStringList();
    QVERIFY(families.size() >= 1);
    if (families.size() >= 2) {
        QMetaObject::invokeMethod(family, "activated", Q_ARG(int, 1));
        QCOMPARE(settings->editorFontFamily(), families.at(1));
        QVERIFY(!settings->themeFontFamily());
        for (QQuickItem *pane : panes()) QCOMPARE(pane->property("fontFamily").toString(), families.at(1));
        QMetaObject::invokeMethod(family, "activated", Q_ARG(int, 0));
        QVERIFY(settings->themeFontFamily());
        QCOMPARE(settings->editorFontFamily(), shell.theme->fontFamily());
    }
    // Underlined headings, as the heading commands then write them.
    QMetaObject::invokeMethod(control("headingSyntaxChoice"), "activated", Q_ARG(int, 1));
    QVERIFY(settings->setextHeadings());
    for (QQuickItem *pane : panes()) QVERIFY(pane->property("setextHeadings").toBool());
    // The toolbar, off in every window.
    QQuickItem *toolbar = shell.item(QStringLiteral("toolbar"));
    QVERIFY(toolbar->isVisible());
    QQuickItem *check = control("toolbarChoice");
    check->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Space);
    QVERIFY(!settings->showToolbar());
    QVERIFY(!toolbar->isVisible());
    QVERIFY(!findItem(second->contentItem(), QStringLiteral("toolbar"))->isVisible());
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!isOpen(dialog));
    typeText(window, QStringLiteral("Title"));
    runIn(window, "heading1");
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("Title\n====="));

    // Kept for the next launch.
    Settings again(shell.paths().settingsFile());
    QCOMPARE(again.maxLineLength(), 73);
    QCOMPARE(again.lineHeight(), 1.6);
    QVERIFY(!again.zoomed());
    QVERIFY(again.setextHeadings());
    QVERIFY(!again.showToolbar());
    QVERIFY(again.themeFontFamily());
    // Values out of range change nothing.
    again.setLineHeight(5);
    again.setMaxLineLength(10);
    again.setFontPixelSize(1000);
    QCOMPARE(again.lineHeight(), 1.6);
    QCOMPARE(again.maxLineLength(), 73);
    QCOMPARE(again.fontPixelSize(), Settings::MaxFontPixelSize);
}

void toolbarShowsHidesAndRunsCommands()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1440);
    QQuickItem *toolbar = shell.item(QStringLiteral("toolbar"));
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    QVERIFY(toolbar);
    QTRY_VERIFY(toolbar->isVisible());
    // Each button is a command, named by its title, its keys in its tooltip.
    QQuickItem *bold = findItem(toolbar, QStringLiteral("toolbar-bold"));
    QVERIFY(bold);
    QCOMPARE(bold->property("text").toString(), QStringLiteral("Bold"));
    QCOMPARE(bold->property("keys").toString(), shell.backend->commands()->keys().value(QStringLiteral("bold")).toString());
    QAccessibleInterface *access = QAccessible::queryAccessibleInterface(bold);
    QVERIFY(access && access->text(QAccessible::Name) == QStringLiteral("Bold"));
    // A click formats the selection and leaves the keys in the text.
    typeText(window, QStringLiteral("word"));
    selectRange(editor, 0, 4);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(bold));
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("**word**"));
    QCOMPARE(window->activeFocusItem(), editor);
    // Headings from their menu.
    QQuickItem *heading = findItem(toolbar, QStringLiteral("toolbar-heading"));
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centreOf(heading));
    QObject *headings = findObject(window, QStringLiteral("toolbarHeadingMenu"));
    QTRY_VERIFY(isOpen(headings));
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Down);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!isOpen(headings));
    QCOMPARE(shell.document()->buffer()->text(), QStringLiteral("## **word**"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));

    // Narrow, it waits until asked, and then wraps rather than overflows.
    resizeWindow(window, 360);
    QTRY_VERIFY(!toolbar->isVisible());
    QVERIFY(shell.settings()->showToolbar());
    runIn(window, "toolbar");
    QTRY_VERIFY(toolbar->isVisible());
    QVERIFY(toolbar->height() > 2 * bold->height());
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    QVERIFY2(tabKeyReachesEveryControl(window, &problems), qPrintable(problems.join(", ")));
    runIn(window, "toolbar");
    QTRY_VERIFY(!toolbar->isVisible());
    QVERIFY(shell.settings()->showToolbar());
    // Wide, the command hides it in every window, and the menu says so.
    resizeWindow(window, 1440);
    QTRY_VERIFY(toolbar->isVisible());
    runIn(window, "toolbar");
    QTRY_VERIFY(!toolbar->isVisible());
    QVERIFY(!shell.settings()->showToolbar());
    QQuickItem *entry = qobject_cast<QQuickItem *>(shell.object(QStringLiteral("toolbarEntry")));
    QVERIFY(entry);
    QCOMPARE(entry->property("text").toString(), QStringLiteral("Show toolbar"));
    runIn(window, "toolbar");
    QTRY_VERIFY(toolbar->isVisible());
    QCOMPARE(entry->property("text").toString(), QStringLiteral("Hide toolbar"));
}

void formattingCommandsRunFromEveryEntryPoint()
{
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    QQuickItem *editor = shell.item(QStringLiteral("editor"));
    typeText(window, QStringLiteral("word"));
    SourceBuffer *buffer = shell.document()->buffer();
    // A shortcut, with the keys in the text.
    selectRange(editor, 0, 4);
    QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
    QCOMPARE(buffer->text(), QStringLiteral("*word*"));
    // The same key from elsewhere in the window acts on the text's selection.
    shell.item(QStringLiteral("menuButton"))->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
    QCOMPARE(buffer->text(), QStringLiteral("word"));
    QTRY_COMPARE(window->activeFocusItem(), editor);
    // The menu's Format section.
    QObject *menu = shell.object(QStringLiteral("windowMenu"));
    QVERIFY(QMetaObject::invokeMethod(menu, "commandChosen", Q_ARG(QString, QStringLiteral("strikethrough"))));
    QCOMPARE(buffer->text(), QStringLiteral("~~word~~"));
    QObject *format = shell.object(QStringLiteral("formatMenu"));
    QVERIFY(format);
    QStringList titles;
    for (int i = 0; i < format->property("count").toInt(); ++i) {
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(format, "itemAt", Q_RETURN_ARG(QQuickItem *, item), Q_ARG(int, i));
        if (item) titles << item->property("text").toString();
    }
    for (const char *title : {"Bold", "Italic", "Heading", "Code block", "Link", "Footnote", "Table", "Page break"})
        QVERIFY2(titles.contains(QString::fromUtf8(title)), title);
    // Each is one step to undo.
    shell.binding()->undo();
    QCOMPARE(buffer->text(), QStringLiteral("word"));
    // Nothing edits a document that takes no edits; its commands are off.
    QTemporaryDir files;
    const QString bytes = files.filePath(QStringLiteral("bytes.md"));
    writeFile(bytes, QByteArray("caf\xe9\n"));
    shell.backend->openFiles({bytes});
    QCOMPARE(shell.document()->state(), Document::State::ReadOnly);
    QVariant can;
    QMetaObject::invokeMethod(window, "canRun", Q_RETURN_ARG(QVariant, can), Q_ARG(QVariant, QStringLiteral("bold")));
    QVERIFY(!can.toBool());
    const QString before = shell.document()->buffer()->text();
    runIn(window, "bold");
    QTest::keyClick(window, Qt::Key_I, Qt::ControlModifier);
    QCOMPARE(shell.document()->buffer()->text(), before);
}

void wordCountsFollowTextAndSelection()
{
    // English by Unicode's word boundaries, Chinese and Japanese by character.
    QCOMPARE(WordCounter::count(u""), 0);
    QCOMPARE(WordCounter::count(u"Hello, world! It's a **bold** test."), 6);
    QCOMPARE(WordCounter::count(u"# Heading\n\n- item one\n- [ ] item two\n"), 5);
    QCOMPARE(WordCounter::count(u"3.14 and 42"), 3);
    QCOMPARE(WordCounter::count(u"我爱你"), 3);
    QCOMPARE(WordCounter::count(u"これはペンです"), 7);
    QCOMPARE(WordCounter::count(u"Qt 6 は良い"), 5);
    QCOMPARE(WordCounter::count(u"안녕 세상"), 2);
    QCOMPARE(WordCounter::count(u"café naïve été"), 3);

    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 960);
    QTRY_COMPARE(countShown(shell), QStringLiteral("0 words"));
    typeText(window, QStringLiteral("one two three"));
    QTRY_COMPARE(countShown(shell), QStringLiteral("3 words · 1 min"));
    // The selection's words, once there is a selection.
    selectRange(shell.item(QStringLiteral("editor")), 4, 7);
    QTRY_COMPARE(countShown(shell), QStringLiteral("1 word selected · 1 min"));
    QAccessibleInterface *access = QAccessible::queryAccessibleInterface(shell.item(QStringLiteral("wordCount")));
    Q_UNUSED(access);
    selectRange(shell.item(QStringLiteral("editor")), 13, 13);
    QTRY_COMPARE(countShown(shell), QStringLiteral("3 words · 1 min"));
    // Reading time at 200 words a minute.
    shell.document()->buffer()->replace({0, int(shell.document()->buffer()->length())}, QStringLiteral("word ").repeated(201));
    QTRY_COMPARE(countShown(shell), QStringLiteral("201 words · 2 min"));
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    resizeWindow(window, 360);
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));

    // A long text is counted on the worker, never on the UI thread, and a
    // count of older text never replaces a newer one.
    DocumentStore store(AppPaths::under(shell.root.path() + QStringLiteral("/counter")));
    Document *document = store.newDraft();
    WordCounter counter;
    QSemaphore gate;
    // The worker's one thread waits until the test lets it go.
    counter.pool()->start([&gate] { gate.acquire(); });
    const QString longText = QStringLiteral("alpha beta gamma delta ").repeated(2000);
    document->buffer()->replace({0, 0}, longText);
    counter.setDocument(document);
    QTRY_VERIFY(counter.counting());
    QCOMPARE(counter.words(), 0);
    // An edit meanwhile: the count of the text before it is dropped.
    document->buffer()->replace({int(document->buffer()->length()), 0}, QStringLiteral("omega"));
    gate.release();
    QTRY_COMPARE(counter.words(), 8001);
    QVERIFY(!counter.counting());
    QCOMPARE(counter.minutes(), 41);
}

} // namespace EditingTests
