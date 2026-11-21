#include "stylingtests.h"

#include "apppaths.h"
#include "commandregistry.h"
#include "document.h"
#include "documentstore.h"
#include "editorbinding.h"
#include "editordecorations.h"
#include "markdownstyling.h"
#include "mdtree.h"
#include "settings.h"
#include "sourcebuffer.h"
#include "testfiles.h"
#include "theme.h"
#include "windowtesting.h"

#include <QAccessible>
#include <QFontMetricsF>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickTextDocument>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QtTest>
#include <algorithm>
#include <memory>
#include <optional>

namespace {

QTextCharFormat coloured(const QColor &color)
{
    QTextCharFormat format;
    format.setForeground(color);
    return format;
}

// One view of one document, as a window shows it: a theme on a temporary
// folder, a store on temporary paths, and an EditorPane in a window.
struct View {
    QTemporaryDir root;
    QTemporaryDir themeFolder;
    std::unique_ptr<Theme> theme;
    std::unique_ptr<DocumentStore> store;
    std::optional<Revision> styledRevision;
    QQmlEngine engine;
    QQuickWindow window;
    Document *document = nullptr;
    QQuickItem *pane = nullptr;
    QQuickItem *editor = nullptr;
    EditorBinding *binding = nullptr;
    QString error;

    explicit View(const QString &text, int width = 600, int height = 400,
                  const QString &name = QStringLiteral("styled.md"))
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
        pane->setSize(QSizeF(width, height));
        window.resize(width, height);
        if (MarkdownStyling *live = styling())
            QObject::connect(live, &MarkdownStyling::visibleStyled, pane,
                             [this](Revision revision) { styledRevision = revision; });
        pane->setProperty("document", QVariant::fromValue(document));
        editor = pane->findChild<QQuickItem *>(QStringLiteral("editor"));
        binding = pane->findChild<EditorBinding *>();
        window.show();
        if (editor) editor->forceActiveFocus();
        QCoreApplication::processEvents();
    }

    bool ok() const { return document && pane && editor && binding && decorations() && styling(); }
    EditorDecorations *decorations() const
    {
        return pane ? pane->findChild<EditorDecorations *>(QStringLiteral("decorations")) : nullptr;
    }
    MarkdownStyling *styling() const { return pane ? pane->findChild<MarkdownStyling *>(QStringLiteral("styling")) : nullptr; }
    QTextDocument *textDocument() const
    {
        return editor->property("textDocument").value<QQuickTextDocument *>()->textDocument();
    }
    // Waits until the styling has nothing left to do and Qt has every paragraph formatted.
    bool settle() const
    {
        if (!QTest::qWaitFor([&] { return styledRevision == buffer()->revision() && !styling()->busy(); }, 5000)) return false;
        decorations()->flush();
        return true;
    }
    // The styling layer's ranges, those of one style that meet joined, as
    // the engine's runs of the text are.
    std::vector<DecorationSpan> styledRuns() const
    {
        std::vector<DecorationSpan> joined;
        for (const DecorationSpan &span : decorations()->spans(MarkdownStyling::layerName())) {
            if (!joined.empty() && joined.back().end == span.start && joined.back().style == span.style)
                joined.back().end = span.end;
            else
                joined.push_back(span);
        }
        return joined;
    }
    // The formats Qt lays a paragraph out with, as "text:what", each colour
    // named by the theme's token it comes from: text colours as made
    // readable over the background (and the code fill, for code).
    QStringList formats(int blockNumber) const
    {
        const QTextBlock block = textDocument()->findBlockByNumber(blockNumber);
        const QColor background = theme->background(), foreground = theme->foreground();
        const auto readable = [&](const QColor &color, const QList<QColor> &behind) {
            return MarkdownStyling::readable(color, foreground, behind);
        };
        const auto named = [&](const QColor &color) -> QString {
            const QList<std::pair<QColor, const char *>> tokens{
                {theme->dim(), "dim"},
                {readable(theme->accent(), {background}), "accent"},
                {readable(theme->good(), {background, theme->raised()}), "good"},
                {readable(theme->warning(), {background}), "warning"},
                {theme->raised(), "raised"},
                {foreground, "foreground"},
                {Theme::mix(background, theme->warning(), theme->dark() ? .3 : .45), "mark"},
                {readable(Theme::mix(foreground, theme->accent(), .5), {background}), "halfaccent"}};
            for (const auto &[token, name] : tokens)
                if (color == token) return QString::fromLatin1(name);
            return color.name();
        };
        QStringList found;
        for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
            QStringList what;
            if (range.format.hasProperty(QTextFormat::ForegroundBrush)) what << named(range.format.foreground().color());
            if (range.format.fontWeight() >= QFont::Bold) what << QStringLiteral("bold");
            if (range.format.fontItalic()) what << QStringLiteral("italic");
            if (range.format.fontStrikeOut()) what << QStringLiteral("strike");
            if (range.format.hasProperty(QTextFormat::BackgroundBrush))
                what << QStringLiteral("on ") + named(range.format.background().color());
            found << block.text().mid(range.start, range.length) + u':' + what.join(u' ');
        }
        return found;
    }
    // Where the editor draws every position: its caret's rectangle, and each line's place.
    QStringList geometry() const
    {
        QStringList found;
        for (QTextBlock block = textDocument()->begin(); block.isValid(); block = block.next()) {
            QTextLayout *layout = block.layout();
            for (int i = 0; i < layout->lineCount(); ++i) {
                const QTextLine line = layout->lineAt(i);
                found << QStringLiteral("line %1+%2 at %3,%4 %5").arg(block.position() + line.textStart())
                             .arg(line.textLength()).arg(line.x()).arg(line.y()).arg(line.height());
            }
        }
        const int length = editor->property("length").toInt();
        for (int position = 0; position <= length; ++position) {
            QRectF caret;
            QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, caret), Q_ARG(int, position));
            found << QStringLiteral("%1: %2,%3 %4").arg(position).arg(caret.x()).arg(caret.y()).arg(caret.height());
        }
        return found;
    }
    SourceBuffer *buffer() const { return document->buffer(); }
    QString text() const { return buffer()->text(); }
    int at(const QString &what, int from = 0) const { return int(text().indexOf(what, from)); }
    // The ranges of a layer as "text:style", in order.
    QStringList spans(const QString &layer) const
    {
        QStringList found;
        const QString source = text();
        for (const DecorationSpan &span : decorations()->spans(layer))
            found << QStringLiteral("%1:%2").arg(source.mid(span.start, span.end - span.start)).arg(span.style);
        return found;
    }
};

// A style as the tests name it: its weight, slant and strike, its colour
// and its fill, joined with "+".
QString describe(const MarkdownStyle &style)
{
    static const char *const colors[] = {"", "marker", "faded", "link", "code", "math"};
    static const char *const fills[] = {"", "codefill", "markfill"};
    QStringList parts;
    if (style.bold) parts << QStringLiteral("bold");
    if (style.italic) parts << QStringLiteral("italic");
    if (style.strike) parts << QStringLiteral("strike");
    if (style.color != MarkdownStyle::Plain) parts << QString::fromLatin1(colors[style.color]);
    if (style.fill != MarkdownStyle::NoFill) parts << QString::fromLatin1(fills[style.fill]);
    return parts.join(u'+');
}

// The styled runs of a text, as "source|style", in order.
QStringList styled(const QString &text)
{
    const md::Document document = md::parse(text);
    QStringList found;
    for (const DecorationSpan &run : markdownStyleRuns(document))
        found << text.mid(run.start, run.end - run.start) + u'|' + describe(MarkdownStyle::at(run.style));
    return found;
}

// A book of `words` words of Markdown, as a writer's long document has it:
// front matter, a chapter heading every dozen paragraphs of 150 words, bold,
// emphasis, links, code and notes every few words, and now and then a list,
// a task and a quote.
QString markdownBook(int words)
{
    const QStringList prose = QStringLiteral("the quiet writer keeps each ordinary word and reads again").split(u' ');
    QString text = QStringLiteral("---\ntitle: The styled book\n---\n\n");
    int written = 0;
    for (int paragraph = 0; written < words; ++paragraph) {
        if (paragraph % 12 == 0) {
            text += QStringLiteral("## Chapter %1\n\n").arg(paragraph / 12 + 1);
            written += 2;
        }
        for (int i = 0; i < 150 && written < words; ++i, ++written) {
            const QString &word = prose.at(written % prose.size());
            switch (i % 25) {
            case 3:
                text += QStringLiteral("**%1**").arg(word);
                break;
            case 9:
                text += QStringLiteral("*%1*").arg(word);
                break;
            case 14:
                text += QStringLiteral("[%1](https://example.org/%2)").arg(word).arg(written);
                break;
            case 19:
                text += QStringLiteral("`%1`").arg(word);
                break;
            case 22:
                text += word + QStringLiteral("[^n]");
                break;
            default:
                text += word;
            }
            text += i == 149 ? QStringLiteral(".\n\n") : QStringLiteral(" ");
        }
        if (paragraph % 7 == 3) text += QStringLiteral("- a list item with **bold**\n- [ ] a task\n\n> A quote with *emphasis*.\n\n");
    }
    return text + QStringLiteral("\n\n[^n]: The note.\n");
}

struct Timing {
    double median = 0, p95 = 0, max = 0;
};

Timing timing(std::vector<double> times)
{
    if (times.empty()) return {};
    std::sort(times.begin(), times.end());
    return {times[times.size() / 2], times[std::min(times.size() - 1, times.size() * 95 / 100)], times.back()};
}

} // namespace

namespace StylingTests {

void constructsGetTheirStylesOnTheirMarkersAndContent()
{
    // Each construct of the dialect styles its punctuation as markers and
    // its content as what it is, from the engine's tree; plain text has no
    // style at all.
    struct Case {
        const char *text;
        QStringList runs;
    };
    const Case cases[] = {
        {"Plain text, nothing more.", {}},
        // Headings: the marks dimmed, the text in the heading weight.
        {"# Title", {"#|marker", " Title|bold"}},
        {"## Closed ##", {"##|marker", " Closed |bold", "##|marker"}},
        {"Title\n=====", {"Title\n|bold", "=====|marker"}},
        {"# Title [Label]", {"#|marker", " Title |bold", "[Label]|bold+marker"}},
        // Inline formatting, with markers dimmed.
        {"a **strong** b", {"**|marker", "strong|bold", "**|marker"}},
        {"a __strong__ b", {"__|marker", "strong|bold", "__|marker"}},
        {"a *em* and _em_", {"*|marker", "em|italic", "*|marker", "_|marker", "em|italic", "_|marker"}},
        {"a ***both*** b", {"*|marker", "**|italic+marker", "both|bold+italic", "**|italic+marker", "*|marker"}},
        {"a ~~gone~~ b", {"~~|marker", "gone|strike", "~~|marker"}},
        {"a ==mark== b", {"==|marker", "mark|markfill", "==|marker"}},
        {"a `code` b", {"`|marker+codefill", "code|code+codefill", "`|marker+codefill"}},
        {"x^2 and x^(a+b)^ and H~2~O", {"^|marker", "^|marker", "^|marker", "~|marker", "~|marker"}},
        {"a \\*literal\\* b", {"\\|marker", "\\|marker"}},
        // Links and images: the label in the link colour, the rest dimmed.
        {"a [text](https://example.org \"T\") b", {"[|marker", "text|link", "](https://example.org \"T\")|marker"}},
        {"a [text][ref] b\n\n[ref]: /url 'Title'",
         {"[|marker", "text|link", "][ref]|marker", "[ref]:|marker", " /url 'Title'|faded"}},
        {"a ![alt](pic.png) b", {"![|marker", "alt|link", "](pic.png)|marker"}},
        {"a <https://example.org> b", {"<|marker", "https://example.org|link", ">|marker"}},
        {"see www.example.org now", {"www.example.org|link"}},
        {"a **bold [link](u)** b",
         {"**|marker", "bold |bold", "[|bold+marker", "link|bold+link", "](u)|bold+marker", "**|marker"}},
        // Notes, citations and variables.
        {"a[^1] b\n\n[^1]: The note.", {"[^|marker", "1|link", "]|marker", "[^1]:|marker"}},
        {"a[^An inline *note*.] b", {"[^|marker", "*|marker", "note|italic", "*|marker", "]|marker"}},
        {"a [p. 2][#key] b\n\n[#key]: A work.", {"[|marker", "p. 2|link", "][#key]|marker", "[#key]:|marker"}},
        {"a [@key, p. 3] b\n\n[#key]: A work.", {"[|marker", "@key, p. 3|link", "]|marker", "[#key]:|marker"}},
        {"---\ntitle: T\n---\nBy [%title].",
         {"---|marker", "\ntitle: T\n|faded", "---|marker", "[%|marker", "title|link", "]|marker"}},
        // Math, tinted, with its dollars dimmed.
        {"a $x^2$ b", {"$|marker", "x^2|math", "$|marker"}},
        {"$$\nx = y\n$$", {"$$|marker", "\nx = y\n|math", "$$|marker"}},
        // Blocks: list, quote and table punctuation, task boxes, fences.
        {"- item\n- [ ] todo\n- [x] done\n\n12. tenth",
         {"-|marker", "-|marker", "[ ]|marker", "-|marker", "[|marker", "x|link", "]|marker", "12.|marker"}},
        {"> one\n> > two", {">|marker", ">|marker", ">|marker"}},
        {"```js\nlet x;\n```", {"```js|marker", "\nlet x;\n|code", "```|marker"}},
        {"~~~~\nplain\n~~~~~", {"~~~~|marker", "\nplain\n|code", "~~~~~|marker"}},
        {"```mermaid\ngraph TD\n```", {"```mermaid|marker", "\ngraph TD\n|code", "```|marker"}},
        {"Text\n\n    indented code", {"indented code|code"}},
        {"| a | b |\n|---|--:|\n| c | d |",
         {"||marker", "||marker", "||marker", "|---|--:||marker", "||marker", "||marker", "||marker"}},
        {"Term\n: Definition", {":|marker"}},
        // Comments, breaks and directives, dimmed.
        {"<!-- hidden -->", {"<!-- hidden -->|faded"}},
        {"Text\n// a comment\nmore", {"// a comment|faded"}},
        {"a\n\n***\n\nb", {"***|marker"}},
        {"a\n\n+++\n\nb", {"+++|marker"}},
        {"{{TOC}}\n\n# Heading", {"{{TOC}}|marker", "#|marker", " Heading|bold"}},
        {"a <sup>x</sup> b", {"<sup>|marker", "</sup>|marker"}},
        {"<ul>\n<li>raw &amp; <b>bold</b></li>\n</ul>",
         {"<ul>|marker", "<li>|marker", "<b>|marker", "</b></li>|marker", "</ul>|marker"}},
        // Markup the HTML policy does not take is text, in the preview too.
        {"<div>\n<script>x</script>\n</div>", {}},
        {"chapter.md \"Caption\"", {"chapter.md|link", " \"Caption\"|faded"}},
    };
    for (const Case &c : cases) {
        const QString text = QString::fromUtf8(c.text);
        const QStringList runs = styled(text);
        if (runs != c.runs)
            QFAIL(qPrintable(QStringLiteral("%1\n   got:      %2\n   expected: %3")
                                 .arg(text, runs.join(QStringLiteral(" · ")), c.runs.join(QStringLiteral(" · ")))));
    }
}

void stylesFollowCrlfAndUnicodeAndStayFew()
{
    // Runs are source ranges: with CRLF line endings they cover the same
    // text, CR included where a run spans a line break; surrogate pairs and
    // combining marks stay whole. Runs never overlap, and runs of one
    // style that meet are one run.
    const QString text = QStringLiteral(
        "---\ntitle: Été\n---\n# Naïve *café* 😀\n\nA **bold 日本語** and [a 😀 link](https://x.org)[^n] é.\n\n"
        "> - [x] done `cödé`\n> - plain\n\n```py\nprint('é')\n```\n\n[^n]: A note.\n");
    QString crlf = text;
    crlf.replace(u'\n', QStringLiteral("\r\n"));
    const QStringList lf = styled(text);
    QStringList windows = styled(crlf);
    for (QString &run : windows) run.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    QCOMPARE(windows, lf);
    QVERIFY(lf.contains(QStringLiteral("café|bold+italic")));
    QVERIFY(lf.contains(QStringLiteral(" Naïve |bold")));
    QVERIFY(lf.contains(QStringLiteral(" 😀|bold")));
    QVERIFY(lf.contains(QStringLiteral("bold 日本語|bold")));
    QVERIFY(lf.contains(QStringLiteral("a 😀 link|link")));
    QVERIFY(lf.contains(QStringLiteral("cödé|code+codefill")));
    QVERIFY(lf.contains(QStringLiteral("\nprint('é')\n|code")));
    // The link's closing marker and the note's opening one meet: one run.
    QVERIFY(lf.contains(QStringLiteral("](https://x.org)[^|marker")));

    for (const QString &source : {text, crlf}) {
        const md::Document document = md::parse(source);
        const std::vector<DecorationSpan> runs = markdownStyleRuns(document);
        for (size_t i = 0; i < runs.size(); ++i) {
            QVERIFY(runs[i].start < runs[i].end);
            QVERIFY(runs[i].style > 0 && runs[i].style < MarkdownStyle::Count);
            // No run starts or ends inside a surrogate pair or a CRLF pair.
            for (const int edge : {runs[i].start, runs[i].end}) {
                QVERIFY(edge == 0 || edge == source.size() || !source.at(edge).isLowSurrogate());
                QVERIFY(edge == 0 || edge == source.size() || !(source.at(edge - 1) == u'\r' && source.at(edge) == u'\n'));
            }
            if (i > 0) {
                QVERIFY(runs[i - 1].end <= runs[i].start);
                QVERIFY(runs[i - 1].end < runs[i].start || runs[i - 1].style != runs[i].style);
            }
        }
        // The parts the styling is handed over in start at line starts, in
        // order, the first at the start of the text, and one at every block.
        const std::vector<int> parts = markdownStyleParts(document);
        QVERIFY(!parts.empty());
        QCOMPARE(parts.front(), 0);
        for (size_t i = 0; i < parts.size(); ++i) {
            QVERIFY(parts[i] == 0 || source.at(parts[i] - 1) == u'\n');
            if (i > 0) QVERIFY(parts[i - 1] < parts[i]);
        }
        for (const QString &line : {QStringLiteral("# Naïve"), QStringLiteral("A **bold"), QStringLiteral("> - [x]"),
                                    QStringLiteral("> - plain"), QStringLiteral("```py"), QStringLiteral("[^n]:")})
            QVERIFY2(std::find(parts.begin(), parts.end(), int(source.indexOf(line))) != parts.end(), qPrintable(line));
    }

    // A paragraph dense with formatting has a few runs for each construct,
    // never one for each character.
    QString dense;
    for (int i = 0; i < 200; ++i) dense += QStringLiteral("**b%1** *i* [l](u) ").arg(i);
    const std::vector<DecorationSpan> runs = markdownStyleRuns(md::parse(dense));
    QVERIFY2(runs.size() <= 200 * 9, qPrintable(QString::number(runs.size())));
}

void decorationRangesCanGrowAtTheirEdges()
{
    // A layer can let the ranges of some of its styles take what is typed
    // just inside their ends: typing at the end of a heading's text, or
    // just inside the markers of bold text, goes into that text at once,
    // while typing after a closing marker stays outside it.
    View view(QStringLiteral("# Title\n\n**bold** end ab\n"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    EditorDecorations *decorations = view.decorations();
    const QString name = QStringLiteral("growing");
    decorations->defineLayer(name, {coloured(QColor(0xc0, 0x40, 0x40)), coloured(QColor(0x80, 0x80, 0x80))},
                             {.growAtEdges = {true, false}});
    SourceBuffer *buffer = view.buffer();
    const int bold = view.at(QStringLiteral("**bold"));
    const int ab = view.at(QStringLiteral("ab"));
    QVERIFY(decorations->setSpans(name, buffer->revision(),
                                  {{0, 1, 1}, {1, 7, 0}, {bold, bold + 2, 1}, {bold + 2, bold + 6, 0},
                                   {bold + 6, bold + 8, 1}, {ab, ab + 1, 0}, {ab + 1, ab + 2, 0}}));
    QCOMPARE(view.spans(name), (QStringList{"#:1", " Title:0", "**:1", "bold:0", "**:1", "a:0", "b:0"}));

    // At the end of the heading's text, and just inside both markers.
    buffer->replace({7, 0}, QStringLiteral("s"));
    buffer->replace({view.at(QStringLiteral("bold")), 0}, QStringLiteral("<"));
    buffer->replace({view.at(QStringLiteral("bold")) + 4, 0}, QStringLiteral(">"));
    QCOMPARE(view.spans(name), (QStringList{"#:1", " Titles:0", "**:1", "<bold>:0", "**:1", "a:0", "b:0"}));
    // After a closing marker, and before an opening one, nothing takes it.
    buffer->replace({view.at(QStringLiteral(">**")) + 3, 0}, QStringLiteral("."));
    buffer->replace({0, 0}, QStringLiteral("x"));
    QCOMPARE(view.spans(name), (QStringList{"#:1", " Titles:0", "**:1", "<bold>:0", "**:1", "a:0", "b:0"}));
    QCOMPARE(view.text(), QStringLiteral("x# Titles\n\n**<bold>**. end ab\n"));
    // Where one growing range ends and the next starts, the next takes it.
    buffer->replace({view.at(QStringLiteral("ab")) + 1, 0}, QStringLiteral("-"));
    QCOMPARE(view.spans(name).mid(5), (QStringList{"a:0", "-b:0"}));
    // Typing over the text beside a range is not typing at its end.
    buffer->replace({view.at(QStringLiteral("s\n")) + 1, 1}, QStringLiteral(" "));
    QCOMPARE(view.spans(name).at(1), QStringLiteral(" Titles:0"));
    // A range the edits did not touch can be followed to where it is now;
    // one they touched, at an edge too, cannot.
    const Revision then = buffer->revision();
    const SourceRange end{view.at(QStringLiteral("end")), 3};
    buffer->replace({0, 0}, QStringLiteral("> "));
    SourceRange moved = end;
    QVERIFY(decorations->follow(then, &moved));
    QCOMPARE(moved.start, end.start + 2);
    QCOMPARE(moved.length, 3);
    QCOMPARE(view.text().mid(moved.start, moved.length), QStringLiteral("end"));
    buffer->replace({moved.start + 3, 0}, QStringLiteral("s"));
    SourceRange touched = end;
    QVERIFY(!decorations->follow(then, &touched));
    QCOMPARE(touched.start, end.start);
    SourceRange now{moved.start, 4};
    QVERIFY(decorations->follow(buffer->revision(), &now));
    QCOMPARE(now.start, moved.start);
}

void editorStylesMarkdownAsItIsWritten()
{
    // The editor shows its Markdown styled, every colour a theme token, the
    // punctuation in its place and dimmed; what is typed is styled once
    // typing pauses, and text typed at the end of a heading is the
    // heading's at once.
    View view(QStringLiteral("# A room for thought\n\n"
                             "There is a **particular** quiet before the *first* sentence, a [link](https://example.org), "
                             "and `code`.\n\n"
                             "> Begin with ==what== is already here.\n\n"
                             "- [x] Find the thought\n- [ ] Give it ~~room~~ to grow\n\n"
                             "```py\nprint(1)\n```\n"),
              1200, 600);
    QVERIFY2(view.ok(), qPrintable(view.error));
    MarkdownStyling *styling = view.styling();
    QVERIFY(view.settle());
    // The font draws bold and italic as wide as plain text, or they are colours.
    const QString bold = styling->boldShown() ? QStringLiteral("bold") : QStringLiteral("accent");
    const QString italic = styling->italicShown() ? QStringLiteral("italic") : QStringLiteral("halfaccent");
    QCOMPARE(view.formats(0), (QStringList{"#:dim", " A room for thought:" + bold}));
    QCOMPARE(view.formats(2), (QStringList{"**:dim", "particular:" + bold, "**:dim", "*:dim", "first:" + italic, "*:dim",
                                           "[:dim", "link:accent", "](https://example.org):dim", "`:dim on raised",
                                           "code:good on raised", "`:dim on raised"}));
    QCOMPARE(view.formats(4), (QStringList{">:dim", "==:dim", "what:on mark", "==:dim"}));
    QCOMPARE(view.formats(6), (QStringList{"-:dim", "[:dim", "x:accent", "]:dim"}));
    QCOMPARE(view.formats(7), (QStringList{"-:dim", "[ ]:dim", "~~:dim", "room:strike", "~~:dim"}));
    QCOMPARE(view.formats(9), (QStringList{"```py:dim"}));
    QCOMPARE(view.formats(10), (QStringList{"print(1):good"}));
    QCOMPARE(view.formats(11), (QStringList{"```:dim"}));
    QCOMPARE(view.styledRuns(), markdownStyleRuns(md::parse(view.text())));

    // Typing at the end of the heading continues it at once, before any
    // styling is found again.
    view.editor->setProperty("cursorPosition", view.binding->sourceToEditor(view.at(QStringLiteral("\n"))));
    sendKey(&view.window, 's');
    QCOMPARE(view.formats(0), (QStringList{"#:dim", " A room for thoughts:" + bold}));
    // New formatting is styled once typing pauses.
    view.editor->setProperty("cursorPosition", view.binding->sourceToEditor(view.at(QStringLiteral("`.")) + 2));
    typeText(&view.window, QStringLiteral(" Then **new** words."));
    QVERIFY(view.settle());
    QCOMPARE(view.formats(2).mid(12), (QStringList{"**:dim", "new:" + bold, "**:dim"}));
    // Taking a marker away takes its formatting away.
    view.buffer()->replace({view.at(QStringLiteral("~~room")), 2}, QString());
    QVERIFY(view.settle());
    QCOMPARE(view.formats(7), (QStringList{"-:dim", "[ ]:dim"}));
    QCOMPARE(view.styledRuns(), markdownStyleRuns(md::parse(view.text())));
}

void stylingFollowsEditsWithoutStaleResults()
{
    // However typing and the passes of the styling interleave, once typing
    // pauses the view's styling is that of the text as it is now: nothing
    // found for older text is left, in a document with LF line endings or
    // CRLF ones, with surrogate pairs and combining marks in it.
    const QString sample = QStringLiteral(
        "---\ntitle: Été\n---\n\n# Heading *one*\n\nA **bold** line with `code` and [a link](u) 😀 é.\n\n"
        "> - [x] quoted task\n> - plain\n\n```\nfenced\n```\n\n| a | b |\n|---|---|\n| c | d |\n\nLast $x$ line.\n");
    const QStringList pieces{QStringLiteral("*"),  QStringLiteral("**"),   QStringLiteral("`"),   QStringLiteral("["),
                             QStringLiteral("](u)"), QStringLiteral("# "),  QStringLiteral("\n"),  QStringLiteral("\n\n"),
                             QStringLiteral("> "),  QStringLiteral("- "),   QStringLiteral("=="),  QStringLiteral("~~"),
                             QStringLiteral("$"),   QStringLiteral("```\n"), QStringLiteral("word"), QStringLiteral("é"),
                             QStringLiteral("😀"),  QStringLiteral(" "),    QStringLiteral("|"),   QStringLiteral("---\n")};
    for (const bool crlf : {false, true}) {
        QString text = sample;
        if (crlf) text.replace(u'\n', QStringLiteral("\r\n"));
        View view(text, 800, 300);
        QVERIFY2(view.ok(), qPrintable(view.error));
        QVERIFY(view.settle());
        QCOMPARE(view.styledRuns(), markdownStyleRuns(md::parse(view.text())));
        QRandomGenerator random(20261116 + int(crlf));
        int edits = 0;
        for (int i = 0; i < 150; ++i) {
            const int length = view.buffer()->length();
            const int at = int(random.bounded(length + 1));
            SourceBuffer::EditOutcome outcome;
            if (length > 0 && random.bounded(4) == 0)
                outcome = view.buffer()->replace({at, qMin(int(random.bounded(1, 4)), length - at)}, QString());
            else
                outcome = view.buffer()->replace({at, 0}, pieces.at(int(random.bounded(pieces.size()))));
            if (outcome == SourceBuffer::EditOutcome::Applied) ++edits;
            // Pauses shorter and longer than the styling waits for.
            static const int pauses[] = {0, 0, 0, 5, 12, MarkdownStyling::SettleMs + 5, 40};
            const int pause = pauses[random.bounded(int(std::size(pauses)))];
            if (pause > 0) QTest::qWait(pause);
            else QCoreApplication::processEvents();
        }
        QVERIFY(edits > 100);
        QVERIFY(view.settle());
        const QString now = view.text();
        const std::vector<DecorationSpan> expected = markdownStyleRuns(md::parse(now));
        const std::vector<DecorationSpan> shown = view.styledRuns();
        if (shown != expected) {
            QStringList lines;
            for (size_t i = 0; i < std::max(shown.size(), expected.size()); ++i) {
                const auto describeRun = [&](const std::vector<DecorationSpan> &runs) {
                    if (i >= runs.size()) return QStringLiteral("-");
                    return QStringLiteral("%1-%2 %3 \"%4\"").arg(runs[i].start).arg(runs[i].end).arg(runs[i].style)
                        .arg(now.mid(runs[i].start, runs[i].end - runs[i].start));
                };
                lines << describeRun(shown) + QStringLiteral("   |   ") + describeRun(expected);
            }
            QFAIL(qPrintable(QStringLiteral("crlf %1, text:\n%2\nshown | expected:\n%3").arg(crlf).arg(now, lines.join(u'\n'))));
        }
        // A pass after a pause hands over only what changed: here the
        // paragraph added at the end and the last part before it.
        view.buffer()->replace({0, view.buffer()->length()}, text);
        QVERIFY(view.settle());
        view.styling()->resetStats();
        view.buffer()->replace({view.buffer()->length(), 0}, QStringLiteral("\nOne more **line**.\n"));
        QVERIFY(view.settle());
        QCOMPARE(view.styledRuns(), markdownStyleRuns(md::parse(view.text())));
        const MarkdownStyling::Stats &stats = view.styling()->stats();
        const int parts = int(markdownStyleParts(md::parse(view.text())).size());
        QVERIFY2(parts > 8 && stats.partsSent <= 2 && stats.partsSent + stats.partsKept == parts,
                 qPrintable(QStringLiteral("%1 sent, %2 kept of %3").arg(stats.partsSent).arg(stats.partsKept).arg(parts)));
    }
}

void themeChangeRecoloursTheStyling()
{
    // A new theme recolours the styling at once, from its own tokens: the
    // markers, the links, code and its fill, math and the highlighter, over a
    // light background as over a dark one, every tinted text as readable as
    // body text.
    View view(QStringLiteral("# Title\n\nA [link](u), `code`, ==marked== and $x$ text.\n"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.settle());
    const QStringList dark{"[:dim", "link:accent", "](u):dim", "`:dim on raised", "code:good on raised", "`:dim on raised",
                           "==:dim", "marked:on mark", "==:dim", "$:dim", "x:warning", "$:dim"};
    QCOMPARE(view.formats(2), dark);
    // Tinted text reads against what is behind it.
    const auto readable = [&] {
        const QTextBlock block = view.textDocument()->findBlockByNumber(2);
        for (const QTextLayout::FormatRange &range : block.layout()->formats()) {
            const QString text = block.text().mid(range.start, range.length);
            if (text != u"link" && text != u"code" && text != u"x") continue;
            const QColor behind = range.format.hasProperty(QTextFormat::BackgroundBrush) ? range.format.background().color()
                                                                                        : view.theme->background();
            const double ratio = contrastRatio(range.format.foreground().color(), behind);
            if (ratio < 4.5) return QStringLiteral("%1 at %2:1").arg(text).arg(ratio);
        }
        return QString();
    };
    QCOMPARE(readable(), QString());
    const QColor darkDim = view.theme->dim(), darkAccent = view.theme->accent();
    // A light theme whose green and yellow (the fallbacks, made for a dark
    // background) are too pale to read on it as they are.
    replaceTheme(view.themeFolder.path(), "light", "#F8F1E3", "#342B27", "#A7353A");
    QTRY_VERIFY(view.theme->accent() != darkAccent);
    QVERIFY(view.theme->dim() != darkDim);
    QVERIFY(contrastRatio(view.theme->good(), view.theme->background()) < 4.5);
    // The formats are made again once every colour has come, and the view
    // formats its paragraphs with them.
    QTRY_COMPARE(view.formats(2), dark);
    QCOMPARE(view.formats(0).first(), QStringLiteral("#:dim"));
    QCOMPARE(view.styling()->markerColor(), view.theme->dim());
    QCOMPARE(readable(), QString());
    // Back to the dark theme, and its colours again.
    replaceTheme(view.themeFolder.path(), "dark", "#1a1b26", "#a9b1d6", "#7aa2f7");
    QTRY_COMPARE(view.theme->accent(), darkAccent);
    QTRY_COMPARE(view.formats(2), dark);
}

void stylingKeepsEveryGlyphWhereItWas()
{
    // Styling changes colour, weight and slant only, and those only where
    // the editor's font draws them as wide as plain text: every line, every
    // caret position and the selection are where they are in plain text.
    // A font whose bold is wider than its plain text gets colour instead.
    QString text = QStringLiteral(
        "---\ntitle: Geometry\n---\n\n# A heading that is long enough to wrap in a narrow column of text\n\n");
    for (int i = 0; i < 4; ++i)
        text += QStringLiteral("Some **bold words**, *slanted ones*, ***both at once***, ~~struck~~, ==marked==, `code`, "
                               "a [link](https://example.org/%1), $x^2$, a note[^n] and *é 😀 naïve* text.\n\n")
                    .arg(i);
    text += QStringLiteral("> - [x] a **done** task\n\n```\ncode block\n```\n\n[^n]: The *note*.\n");
    for (const QString &family : {QStringLiteral("monospace"), QStringLiteral("sans-serif")}) {
        View view(text, 420, 900);
        QVERIFY2(view.ok(), qPrintable(view.error));
        view.pane->setProperty("fontFamily", family);
        QTRY_COMPARE(view.editor->property("font").value<QFont>().family(), family);
        view.pane->setProperty("liveFormatting", false);
        QTRY_VERIFY(view.decorations()->spans(MarkdownStyling::layerName()).empty());
        view.decorations()->flush();
        QCoreApplication::processEvents();
        view.editor->setProperty("cursorPosition", 0);
        QMetaObject::invokeMethod(view.editor, "select", Q_ARG(int, 30), Q_ARG(int, 300));
        const QStringList plain = view.geometry();
        const QRectF caret = view.editor->property("cursorRectangle").toRectF();
        QVERIFY(view.textDocument()->findBlockByNumber(4).layout()->lineCount() > 1);

        view.pane->setProperty("liveFormatting", true);
        QVERIFY(view.settle());
        QCoreApplication::processEvents();
        MarkdownStyling *styling = view.styling();
        // What the font draws: bold and italic as wide as plain text, or not.
        QFont font = view.textDocument()->defaultFont();
        QFont heavy = font;
        heavy.setBold(true);
        const QString sample = QStringLiteral("Wide words, mmm iii 0123");
        const bool boldFits = QFontMetricsF(font).horizontalAdvance(sample) == QFontMetricsF(heavy).horizontalAdvance(sample);
        if (family == u"sans-serif" && boldFits) QSKIP("This system's sans-serif draws bold as wide as plain text");
        if (!boldFits) QVERIFY(!styling->boldShown());
        // The styling is there, bold included where the font can draw it.
        const QStringList heading = view.formats(4);
        QCOMPARE(heading.size(), 2);
        QCOMPARE(heading.last().endsWith(QStringLiteral(":bold")), styling->boldShown());
        QVERIFY(view.formats(6).size() > 20);
        // And nothing moved.
        QCOMPARE(view.geometry(), plain);
        QCOMPARE(view.editor->property("cursorRectangle").toRectF(), caret);
        QCOMPARE(view.editor->property("selectionStart").toInt(), 30);
        QCOMPARE(view.editor->property("selectionEnd").toInt(), 300);
        QCOMPARE(view.textDocument()->toRawText().size(), view.editor->property("length").toInt());
    }
}

void liveFormattingTurnsOff()
{
    // Without live formatting the source is plain text, with no format at
    // all; turned on again, it is styled at once.
    View view(QStringLiteral("# Title\n\nSome **bold** and a [link](u).\n"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.settle());
    QCOMPARE(view.formats(0).size(), 2);
    view.pane->setProperty("liveFormatting", false);
    QVERIFY(!view.styling()->enabled());
    QVERIFY(view.decorations()->spans(MarkdownStyling::layerName()).empty());
    view.decorations()->flush();
    QCOMPARE(view.formats(0), QStringList());
    QCOMPARE(view.formats(2), QStringList());
    // Typing while it is off styles nothing.
    view.buffer()->replace({0, 0}, QStringLiteral("*Lead* "));
    QTest::qWait(MarkdownStyling::SettleMs * 3);
    QVERIFY(!view.styling()->busy());
    QVERIFY(view.decorations()->spans(MarkdownStyling::layerName()).empty());
    view.pane->setProperty("liveFormatting", true);
    QVERIFY(view.settle());
    QCOMPARE(view.styledRuns(), markdownStyleRuns(md::parse(view.text())));
    QCOMPARE(view.formats(2).size(), 6);
}

void liveFormattingIsAPreferenceAndACommand()
{
    // Live formatting is on until it is turned off, from the command
    // palette's command, the window menu or Preferences, in every window at
    // once, and it stays as it was left.
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    Settings *settings = shell.settings();
    QVERIFY(settings->liveFormatting());
    CommandRegistry *commands = shell.backend->commands();
    QVERIFY(commands->contains(QStringLiteral("liveFormatting")));
    QCOMPARE(commands->title(QStringLiteral("liveFormatting")), QStringLiteral("Live formatting"));
    QCOMPARE(commands->needs(QStringLiteral("liveFormatting")), QString());
    typeText(window, QStringLiteral("# Title"));
    auto *styling = qobject_cast<MarkdownStyling *>(findObject(window, QStringLiteral("styling")));
    auto *decorations = qobject_cast<EditorDecorations *>(findObject(window, QStringLiteral("decorations")));
    QVERIFY(styling && decorations);
    QVERIFY(styling->enabled());
    QTRY_VERIFY(!decorations->spans(MarkdownStyling::layerName()).empty());

    // The command turns it off and on, as the palette and the keys run it.
    const auto run = [&](const char *command) {
        QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
    };
    run("liveFormatting");
    QVERIFY(!settings->liveFormatting());
    QVERIFY(!styling->enabled());
    QVERIFY(decorations->spans(MarkdownStyling::layerName()).empty());
    QVERIFY(!Settings(shell.paths().settingsFile()).liveFormatting());
    // A second window follows.
    shell.backend->newWindow();
    QQuickWindow *second = shell.windowFor(shell.backend->windows().at(1));
    QVERIFY(second);
    auto *secondStyling = qobject_cast<MarkdownStyling *>(findObject(second, QStringLiteral("styling")));
    QVERIFY(secondStyling && !secondStyling->enabled());
    QTRY_VERIFY(second->isActive());
    activate(window);
    run("liveFormatting");
    QVERIFY(settings->liveFormatting());
    QVERIFY(styling->enabled() && secondStyling->enabled());
    QTRY_VERIFY(!decorations->spans(MarkdownStyling::layerName()).empty());

    // The window menu names what its entry does.
    QObject *entry = findObject(window, QStringLiteral("liveFormattingEntry"));
    QVERIFY(entry);
    QCOMPARE(entry->property("text").toString(), QStringLiteral("Turn off live formatting"));
    settings->setLiveFormatting(false);
    QCOMPARE(entry->property("text").toString(), QStringLiteral("Turn on live formatting"));
    settings->setLiveFormatting(true);

    // Preferences has it, named and reached by Tab, and Space turns it off.
    QObject *dialog = shell.object(QStringLiteral("preferencesDialog"));
    run("preferences");
    QTRY_VERIFY(dialog->property("opened").toBool());
    auto *check = qobject_cast<QQuickItem *>(findObject(window, QStringLiteral("liveFormattingChoice")));
    QVERIFY(check);
    QVERIFY(check->property("checked").toBool());
    QVERIFY(check->activeFocusOnTab());
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    check->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Space);
    QVERIFY(!settings->liveFormatting());
    QVERIFY(!check->property("checked").toBool());
    QVERIFY(!styling->enabled());
    // The command turns it on again, and the box follows.
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    run("liveFormatting");
    QVERIFY(check->property("checked").toBool());

    // Kept, and a value that is no choice is ignored.
    settings->setLiveFormatting(false);
    QVERIFY(!Settings(shell.paths().settingsFile()).liveFormatting());
    QTemporaryDir folder;
    const QString damaged = folder.filePath(QStringLiteral("settings.json"));
    writeFile(damaged, R"({"liveFormatting": "yes"})");
    QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("ignoring settings out of range .*: liveFormatting$")));
    QVERIFY(Settings(damaged).liveFormatting());
}

void resettingStylingStatsLeavesAnIdleViewSettled()
{
    View view(QStringLiteral("# Title\n\nSome **bold** text.\n"));
    QVERIFY2(view.ok(), qPrintable(view.error));
    QVERIFY(view.settle());
    MarkdownStyling *styling = view.styling();
    QVERIFY(styling->stats().passes > 0);
    styling->resetStats();
    view.editor->setProperty("cursorPosition", view.binding->sourceToEditor(view.at(QStringLiteral("bold"))));
    const QString state = QStringLiteral("Style passes since reset %1, styling busy %2")
                              .arg(styling->stats().passes).arg(styling->busy());
    QVERIFY2(view.settle(), qPrintable(state));
    QCOMPARE(styling->stats().passes, 0);
    QCOMPARE(view.styledRuns(), markdownStyleRuns(md::parse(view.text())));
}

void typingWithLiveFormattingStaysWithinAFrame()
{
    // In a 100,000-word document dense with Markdown, with live formatting
    // on, each key is handled within a frame at 120 Hz, at the start, the
    // middle and the end; each handing over of styling to the view stays
    // within a frame too; and once typing pauses, the styling of the text in
    // view arrives within about 50 ms.
    const QString book = markdownBook(100000);
    {
        // What a pass costs on its worker, for the record.
        QElapsedTimer timer;
        timer.start();
        const md::Document document = md::parse(book);
        const qint64 parsed = timer.nsecsElapsed();
        const std::vector<DecorationSpan> runs = markdownStyleRuns(document);
        const std::vector<int> parts = markdownStyleParts(document);
        qInfo("styling pass on the worker: %d units, parse %.2f ms, style %.2f ms, %zu runs in %zu parts", int(book.size()),
              parsed / 1e6, (timer.nsecsElapsed() - parsed) / 1e6, runs.size(), parts.size());
    }
    View view(book, 1000, 800);
    QVERIFY2(view.ok(), qPrintable(view.error));
    view.window.requestActivate();
    QVERIFY(QTest::qWaitForWindowActive(&view.window));
    view.editor->forceActiveFocus();
    QVERIFY(view.settle());
    MarkdownStyling *styling = view.styling();
    QVERIFY(view.decorations()->spans(MarkdownStyling::layerName()).size() > 30000);
    // The whole book's styling, handed over as it opens, a few dozen parts at a time.
    std::vector<double> opening;
    for (const qint64 nanoseconds : styling->stats().applyNanoseconds) opening.push_back(nanoseconds / 1e6);
    const Timing opened = timing(opening);
    qInfo("live formatting, opening the book: %zu handings over, p50 %.2f ms, p95 %.2f ms, max %.2f ms", opening.size(),
          opened.median, opened.p95, opened.max);

    const auto place = [&](double where) {
        const int at = int(where * (view.buffer()->length() - 10));
        // Inside a word, at a word's end, as typing goes on in one.
        const int end = int(view.text().indexOf(u' ', at));
        view.editor->setProperty("cursorPosition", view.binding->sourceToEditor(end));
        return view.settle();
    };
    // Keys one after another, as fast as they come.
    std::vector<double> keys;
    for (const double where : {0.0, 0.5, 1.0}) {
        QVERIFY(place(where));
        for (int i = 0; i < 20; ++i) {
            QElapsedTimer timer;
            timer.start();
            sendKey(&view.window, QChar(u'a' + i % 26));
            QCoreApplication::processEvents();
            keys.push_back(timer.nsecsElapsed() / 1e6);
        }
    }
    // Keys with pauses between them, each styled before the next.
    QElapsedTimer timer;
    Revision awaited = 0;
    qint64 arrived = -1;
    // The moment the styling in view is that of the text as it is now.
    QObject::connect(styling, &MarkdownStyling::visibleStyled, &view.window, [&](Revision revision) {
        if (revision == awaited && arrived < 0) arrived = timer.nsecsElapsed();
    });
    std::vector<double> paused, arrivals, handovers;
    const auto measure = [&] {
        paused.clear();
        arrivals.clear();
        handovers.clear();
        for (const double where : {0.0, 0.5, 1.0}) {
            QVERIFY(place(where));
            // Count only handovers from this set of keys.
            if (where == 0.0) styling->resetStats();
            for (int i = 0; i < 10; ++i) {
                arrived = -1;
                timer.start();
                sendKey(&view.window, i % 3 == 0 ? QChar(u'*') : QChar(u'a' + i));
                awaited = view.buffer()->revision();
                QCoreApplication::processEvents();
                paused.push_back(timer.nsecsElapsed() / 1e6);
                // An event loop that takes each event as it comes, as the
                // application's does, rather than one that polls.
                QSignalSpy styled(styling, &MarkdownStyling::visibleStyled);
                while (arrived < 0 && styled.wait(2000)) {}
                QVERIFY(arrived >= 0);
                arrivals.push_back(arrived / 1e6);
            }
        }
        for (const qint64 nanoseconds : styling->stats().applyNanoseconds) handovers.push_back(nanoseconds / 1e6);
    };
    const double frame = timeLimit(1000.0 / 120);
    const double styledLimit = timeLimit(50);
    measure();
    if (QTest::currentTestFailed()) return;
    // Other work on the machine can hold up one set of these keys, as a full
    // run of the tests now and then does. A second set, measured the same
    // way, must then keep within the same limits.
    if (timing(paused).p95 >= frame || timing(handovers).p95 >= frame || timing(arrivals).p95 >= styledLimit) {
        qInfo("live formatting: one set of keys after pauses missed a limit (key to styled view p95 %.2f ms); "
              "measuring a second",
              timing(arrivals).p95);
        measure();
        if (QTest::currentTestFailed()) return;
    }
    const Timing fast = timing(keys), slow = timing(paused), given = timing(handovers), latency = timing(arrivals);
    qInfo("live formatting, keys in a row: p50 %.2f ms, p95 %.2f ms, max %.2f ms", fast.median, fast.p95, fast.max);
    qInfo("live formatting, keys after pauses: p50 %.2f ms, p95 %.2f ms, max %.2f ms", slow.median, slow.p95, slow.max);
    qInfo("live formatting, handing styling over: p50 %.2f ms, p95 %.2f ms, max %.2f ms (%d parts sent, %d kept)",
          given.median, given.p95, given.max, styling->stats().partsSent, styling->stats().partsKept);
    qInfo("live formatting, key to styled view: p50 %.2f ms, p95 %.2f ms, max %.2f ms", latency.median, latency.p95,
          latency.max);
    QVERIFY2(fast.p95 < frame, qPrintable(QString::number(fast.p95)));
    QVERIFY2(slow.p95 < frame, qPrintable(QString::number(slow.p95)));
    QVERIFY2(given.p95 < frame, qPrintable(QString::number(given.p95)));
    QVERIFY2(opened.p95 < frame, qPrintable(QString::number(opened.p95)));
    QVERIFY2(latency.p95 < styledLimit, qPrintable(QString::number(latency.p95)));
    QVERIFY(view.settle());
    QCOMPARE(view.styledRuns(), markdownStyleRuns(md::parse(view.text())));
}

} // namespace StylingTests
