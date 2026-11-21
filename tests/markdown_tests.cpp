#include "markdown_tests.h"
#include "testfiles.h"

#include "frontmatter.h"
#include "mdhtml.h"
#include "mdinclude.h"
#include "mdtree.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <functional>
#include <thread>

// The expectations here are written from docs/markdown-dialect.md, the
// CommonMark 0.31.2 specification and the GFM specification, not from what
// the parser happens to produce.

using md::DiagnosticCode;
using md::Node;
using md::NodeType;

namespace {

QByteArray readFixture(const char *name)
{
    QFile file(QStringLiteral(PARCH_SOURCE_DIR "/tests/fixtures/") + QLatin1StringView(name));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

md::Options writer() { return md::Options::writer(); }
md::Options standard() { return md::Options::standard(); }

// The HTML of a document, without heading ids unless asked for.
QString html(const QString &text, const md::Options &options = writer(), bool headingIds = false)
{
    md::HtmlOptions h;
    h.headingIds = headingIds;
    return md::toHtml(md::parse(text, options), h);
}

// What is inside the single paragraph a text makes; the whole HTML when it
// makes anything else.
QString inner(const QString &text, const md::Options &options = writer())
{
    const QString all = html(text, options);
    if (all.startsWith(QLatin1StringView("<p>")) && all.endsWith(QLatin1StringView("</p>\n"))
        && all.count(QLatin1StringView("<p>")) == 1)
        return all.mid(3, all.size() - 8);
    return all;
}

void collect(const Node &node, NodeType type, std::vector<const Node *> &out)
{
    if (node.type == type) out.push_back(&node);
    for (const Node &child : node.children) collect(child, type, out);
}

std::vector<const Node *> all(const md::Document &doc, NodeType type)
{
    std::vector<const Node *> out;
    collect(doc.root, type, out);
    return out;
}

const Node *first(const md::Document &doc, NodeType type)
{
    const std::vector<const Node *> nodes = all(doc, type);
    return nodes.empty() ? nullptr : nodes.front();
}

int count(const md::Document &doc, DiagnosticCode code)
{
    return int(std::count_if(doc.diagnostics.begin(), doc.diagnostics.end(),
                             [code](const md::Diagnostic &d) { return d.code == code; }));
}

QString describe(const md::Document &doc)
{
    QStringList lines;
    for (const md::Diagnostic &d : doc.diagnostics)
        lines.append(QStringLiteral("%1:[%2,%3) %4").arg(d.file).arg(d.range.start).arg(d.range.end).arg(d.message));
    return lines.join(u'\n');
}

struct Case {
    const char *input;
    const char *expected;
};

// Fails the calling test with the case that differs.
#define CHECK_CASES(cases, render) \
    for (const Case &c_ : cases) { \
        const QString input_ = QString::fromUtf8(c_.input); \
        const QString expected_ = QString::fromUtf8(c_.expected); \
        const QString actual_ = render(input_); \
        if (actual_ != expected_) \
            QFAIL(qPrintable(QStringLiteral("\n   input: %1\nexpected: %2\n  actual: %3").arg(input_, expected_, actual_))); \
    }

QString inWriter(const QString &text) { return inner(text, writer()); }
QString inStandard(const QString &text) { return inner(text, standard()); }
QString blockWriter(const QString &text) { return html(text, writer()); }
QString blockStandard(const QString &text) { return html(text, standard()); }

// Text of ordinary prose with headings, lists, links and a few code blocks.
QString proseDocument(int words)
{
    static const char *const vocabulary[] = {
        "the", "of", "and", "a", "to", "in", "is", "you", "that", "it", "was", "for", "on", "are", "as", "with",
        "his", "they", "at", "be", "this", "have", "from", "or", "one", "had", "by", "word", "but", "not", "what",
        "all", "were", "we", "when", "your", "can", "said", "there", "use", "an", "each", "which", "she", "do",
        "how", "their", "if", "will", "up", "other", "about", "out", "many", "then", "them", "these", "so", "some",
        "her", "would", "make", "like", "him", "into", "time", "has", "look", "two", "more", "write", "go", "see",
        "number", "no", "way", "could", "people", "my", "than", "first", "water", "been", "call", "who", "its",
        "now", "find", "long", "down", "day", "did", "get", "come", "made", "may", "part"};
    const int vocabularySize = int(std::size(vocabulary));
    quint32 seed = 12345;
    auto next = [&seed](int bound) {
        seed = seed * 1664525u + 1013904223u;
        return int((seed >> 8) % quint32(bound));
    };
    QString out;
    int written = 0;
    int section = 0;
    auto sentence = [&]() {
        QString s;
        const int length = 6 + next(13);
        const int special = next(100);
        const int at = next(length);
        for (int i = 0; i < length; ++i) {
            QString word = QLatin1StringView(vocabulary[next(vocabularySize)]);
            if (i == 0) word[0] = word[0].toUpper();
            if (i == at) {
                if (special < 8) word = QStringLiteral("**%1**").arg(word);
                else if (special < 16) word = QStringLiteral("*%1*").arg(word);
                else if (special < 20) word = QStringLiteral("[%1](https://example.org/%1)").arg(word);
                else if (special < 23) word = QStringLiteral("`%1`").arg(word);
            }
            s += word;
            s += i + 1 < length ? u' ' : u'.';
        }
        written += length;
        return s;
    };
    while (written < words) {
        ++section;
        out += QStringLiteral("## Section %1\n\n").arg(section);
        written += 2;
        const int paragraphs = 3 + next(4);
        for (int p = 0; p < paragraphs; ++p) {
            QString line;
            const int sentences = 3 + next(5);
            for (int i = 0; i < sentences; ++i) {
                const QString s = sentence();
                if (!line.isEmpty() && line.size() + s.size() > 80) {
                    out += line;
                    out += u'\n';
                    line.clear();
                }
                if (!line.isEmpty()) line += u' ';
                line += s;
            }
            out += line;
            out += QStringLiteral("\n\n");
        }
        if (section % 3 == 0) {
            for (int i = 0; i < 4; ++i) out += QStringLiteral("- ") + sentence() + u'\n';
            out += u'\n';
        }
        if (section % 7 == 0) out += QStringLiteral("```python\ndef f(x):\n    return x * 2\n```\n\n");
    }
    return out;
}

} // namespace

namespace MarkdownTests {

void commonMarkSpec()
{
    // Every example of the CommonMark 0.31.2 specification, rendered with raw
    // HTML passed through, as the specification's own output does.
    const QJsonArray examples = QJsonDocument::fromJson(readFixture("commonmark-0.31.2.json")).array();
    QCOMPARE(examples.size(), 652);

    auto failures = [&](const md::Options &options) {
        QList<int> failed;
        for (const QJsonValue &value : examples) {
            const QJsonObject example = value.toObject();
            const md::Document doc = md::parse(example[QLatin1StringView("markdown")].toString(), options);
            if (md::toHtml(doc, md::HtmlOptions::commonMark()) != example[QLatin1StringView("html")].toString())
                failed.append(example[QLatin1StringView("example")].toInt());
        }
        return failed;
    };

    // CommonMark itself: the Standard profile without the GFM extension that
    // deliberately reads more text as links than CommonMark does.
    md::Options core = standard();
    core.autolinks = false;
    const QList<int> coreFailures = failures(core);
    if (!coreFailures.isEmpty()) {
        QStringList numbers;
        for (int n : coreFailures) numbers.append(QString::number(n));
        QFAIL(qPrintable(QStringLiteral("CommonMark examples that fail: %1").arg(numbers.join(u' '))));
    }

    // The Standard profile as it is: the only examples that differ are the
    // four whose bare URLs and addresses GFM makes links, as GFM specifies.
    QCOMPARE(failures(standard()), (QList<int>{602, 608, 611, 612}));
    QCOMPARE(inStandard(QStringLiteral("https://example.com")),
             QStringLiteral("<a href=\"https://example.com\">https://example.com</a>"));
    QCOMPARE(inStandard(QStringLiteral("foo@bar.example.com")),
             QStringLiteral("<a href=\"mailto:foo@bar.example.com\">foo@bar.example.com</a>"));
    QCOMPARE(inStandard(QStringLiteral("< https://foo.bar >")),
             QStringLiteral("&lt; <a href=\"https://foo.bar\">https://foo.bar</a> &gt;"));
}

void gfmExtensions()
{
    // The examples of the GFM specification for tables, task list items,
    // strikethrough and autolinks.
    const QJsonArray examples = QJsonDocument::fromJson(readFixture("gfm-extensions.json")).array();
    QCOMPARE(examples.size(), 23);
    QHash<QString, int> sections;
    for (const QJsonValue &value : examples) {
        const QJsonObject example = value.toObject();
        const QString markdown = example[QLatin1StringView("markdown")].toString();
        QString expected = example[QLatin1StringView("html")].toString();
        // The renderer closes the empty input element, as it closes br and img.
        expected.replace(QLatin1StringView("type=\"checkbox\">"), QLatin1StringView("type=\"checkbox\" />"));
        const QString actual = md::toHtml(md::parse(markdown, standard()), md::HtmlOptions::commonMark());
        if (actual != expected)
            QFAIL(qPrintable(QStringLiteral("GFM example %1\n%2\nexpected:\n%3\nactual:\n%4")
                                 .arg(example[QLatin1StringView("example")].toInt())
                                 .arg(markdown, expected, actual)));
        ++sections[example[QLatin1StringView("section")].toString()];
    }
    QCOMPARE(sections.value(QStringLiteral("Tables (extension)")), 8);
    QCOMPARE(sections.value(QStringLiteral("Task list items (extension)")), 2);
    QCOMPARE(sections.value(QStringLiteral("Strikethrough (extension)")), 2);
    QCOMPARE(sections.value(QStringLiteral("Autolinks (extension)")), 11);
    // The same examples hold in the Writer profile, which builds on GFM.
    for (const QJsonValue &value : examples) {
        const QJsonObject example = value.toObject();
        QString expected = example[QLatin1StringView("html")].toString();
        expected.replace(QLatin1StringView("type=\"checkbox\">"), QLatin1StringView("type=\"checkbox\" />"));
        const QString markdown = example[QLatin1StringView("markdown")].toString();
        QCOMPARE(md::toHtml(md::parse(markdown, writer()), md::HtmlOptions::commonMark()), expected);
    }
}

void profilesAndOptions()
{
    // Writer is the default, with every extension on.
    const md::Options defaults;
    QCOMPARE(defaults.profile, md::Profile::Writer);
    QVERIFY(defaults == md::Options::writer());
    QCOMPARE(defaults.enabledExtensions(), md::Options::extensionNames());
    QCOMPARE(defaults.profileName(), QStringLiteral("writer"));
    for (const char *name : {"math", "mermaid", "definition-lists", "footnotes", "citations", "highlight",
                             "super-subscript", "line-comments", "page-breaks", "toc", "variables", "includes",
                             "merged-cells", "heading-labels", "front-matter", "tables", "strikethrough",
                             "task-lists", "autolinks"})
        QVERIFY2(md::Options::extensionNames().contains(QLatin1StringView(name)), name);

    // Standard is CommonMark with the four GFM extensions and nothing else.
    const md::Options plain = md::Options::standard();
    QCOMPARE(plain.profile, md::Profile::Standard);
    QCOMPARE(plain.profileName(), QStringLiteral("standard"));
    QCOMPARE(plain.enabledExtensions(),
             (QStringList{QStringLiteral("tables"), QStringLiteral("strikethrough"), QStringLiteral("task-lists"),
                          QStringLiteral("autolinks")}));
    QVERIFY(md::Options::forProfile(md::Profile::Standard) == plain);
    QVERIFY(md::Options::forProfile(md::Profile::Writer) == defaults);

    // Extensions are switched by name, for command-line use.
    md::Options custom = plain;
    QVERIFY(custom.setExtension(u"math", true));
    QVERIFY(custom.extension(u"math"));
    QVERIFY(custom.math);
    QVERIFY(!custom.setExtension(u"no-such-extension", true));
    QCOMPARE(inner(QStringLiteral("$x$"), custom), QStringLiteral("<span class=\"math\">x</span>"));
    QCOMPARE(inner(QStringLiteral("$x$"), plain), QStringLiteral("$x$"));

    // Changing the profile changes the reading, never the source: the tree
    // of each profile is built from the same text.
    const QString text = QStringLiteral("==marked== and x^2\n");
    QCOMPARE(md::parse(text, writer()).files.front().text, text);
    QCOMPARE(md::parse(text, standard()).files.front().text, text);
    QCOMPARE(md::parse(text, standard()).options.profile, md::Profile::Standard);
    QVERIFY(md::DialectVersion >= 1);
}

void inlineFormatting()
{
    const Case both[] = {
        {"**bold**", "<strong>bold</strong>"},
        {"__bold__", "<strong>bold</strong>"},
        {"*italic*", "<em>italic</em>"},
        {"_italic_", "<em>italic</em>"},
        // Asterisks work inside a word, underscores do not.
        {"in*tra*word", "in<em>tra</em>word"},
        {"in_tra_word", "in_tra_word"},
        {"snake_case_name", "snake_case_name"},
        {"***both***", "<em><strong>both</strong></em>"},
        {"___both___", "<em><strong>both</strong></em>"},
        {"**bold and _italic_**", "<strong>bold and <em>italic</em></strong>"},
        {"*a **b** c*", "<em>a <strong>b</strong> c</em>"},
        {"~~removed~~", "<del>removed</del>"},
        {"`value`", "<code>value</code>"},
        {"`` a ` b ``", "<code>a ` b</code>"},
        {"``` `` ```", "<code>``</code>"},
        {"\\*literal\\*", "*literal*"},
        {"\\_literal\\_", "_literal_"},
        {"\\`not code\\`", "`not code`"},
        // Not formatting.
        {"** not bold **", "** not bold **"},
        {"~~ not removed ~~", "~~ not removed ~~"},
        {"**bold", "**bold"},
        {"~~removed", "~~removed"},
        {"a ~~~three~~~", "a ~~~three~~~"},
        // Runs that do not pair up leave their rest as text.
        {"**foo*", "*<em>foo</em>"},
        {"*foo**", "<em>foo</em>*"},
        {"*foo [bar* baz]", "<em>foo [bar</em> baz]"},
        // Code protects everything in it.
        {"`**a** ~~c~~ <b> &amp;`", "<code>**a** ~~c~~ &lt;b&gt; &amp;amp;</code>"},
    };
    CHECK_CASES(both, inWriter);
    CHECK_CASES(both, inStandard);

    const Case writerOnly[] = {
        {"==marked==", "<mark>marked</mark>"},
        {"**==both==**", "<strong><mark>both</mark></strong>"},
        {"==*a* and ~~b~~==", "<mark><em>a</em> and <del>b</del></mark>"},
        {"a==b==c", "a<mark>b</mark>c"},
        {"== not marked ==", "== not marked =="},
        {"=single=", "=single="},
        {"===three===", "===three==="},
        {"==marked", "==marked"},
        {"\\==a==", "==a=="},
        {"\\~~a~~", "~~a~~"},
        {"==a ~~b== c~~", "<mark>a ~~b</mark> c~~"},
        {"`==a== x^2 x~z $d$ [%e] [^f] {{TOC}}`", "<code>==a== x^2 x~z $d$ [%e] [^f] {{TOC}}</code>"},
        // One tilde is a subscript marker here, not a strikethrough.
        {"~one~", "~one~"},
    };
    CHECK_CASES(writerOnly, inWriter);

    const Case standardOnly[] = {
        {"==marked==", "==marked=="},
        // GFM strikes text through with one tilde as well as with two.
        {"~one~", "<del>one</del>"},
        {"~~two~ mismatched", "~~two~ mismatched"},
    };
    CHECK_CASES(standardOnly, inStandard);

    // The tree: what is emphasised, and by which punctuation.
    const QString text = QStringLiteral("a **bold _it_** ==hi== ~~no~~ `c`");
    const md::Document doc = md::parse(text);
    const Node *strong = first(doc, NodeType::Strong);
    QVERIFY(strong);
    QCOMPARE(doc.source(*strong).toString(), QStringLiteral("**bold _it_**"));
    QCOMPARE(doc.source(0, strong->open).toString(), QStringLiteral("**"));
    QCOMPARE(doc.source(0, strong->close).toString(), QStringLiteral("**"));
    const Node *emphasis = first(doc, NodeType::Emphasis);
    QVERIFY(emphasis);
    QCOMPARE(doc.source(*emphasis).toString(), QStringLiteral("_it_"));
    QCOMPARE(doc.source(*first(doc, NodeType::Highlight)).toString(), QStringLiteral("==hi=="));
    QCOMPARE(doc.source(*first(doc, NodeType::Strikethrough)).toString(), QStringLiteral("~~no~~"));
    const Node *code = first(doc, NodeType::Code);
    QCOMPARE(code->text, QStringLiteral("c"));
    QCOMPARE(doc.source(0, code->open).toString(), QStringLiteral("`"));
    const md::Document escaped = md::parse(QStringLiteral("\\*x"));
    const Node *escape = first(escaped, NodeType::Escape);
    QVERIFY(escape);
    QCOMPARE(escape->text, QStringLiteral("*"));
    QCOMPARE(escape->range, (md::Range{0, 2}));
}

void superscriptAndSubscript()
{
    const Case cases[] = {
        {"100m^2", "100m<sup>2</sup>"},
        {"y^(a+b)^", "y<sup>(a+b)</sup>"},
        {"x~z", "x<sub>z</sub>"},
        {"x~y,z~", "x<sub>y,z</sub>"},
        {"H~2~O", "H<sub>2</sub>O"},
        // The short form takes letters and digits up to space or punctuation.
        {"E = mc^2.", "E = mc<sup>2</sup>."},
        {"x^2 + y^2", "x<sup>2</sup> + y<sup>2</sup>"},
        {"a~i+1", "a<sub>i</sub>+1"},
        {"x^é2 y", "x<sup>é2</sup> y"},
        // The delimited form allows punctuation, needs content and a closing
        // marker, and allows no white space.
        {"x^+^", "x<sup>+</sup>"},
        {"x^a b^", "x<sup>a</sup> b^"},
        {"x^^", "x^^"},
        {"x^(a b)^", "x^(a b)^"},
        // A valid delimited form is preferred to a short run.
        {"x^2+y^2", "x<sup>2+y</sup>2"},
        // Nothing to raise or lower: literal.
        {"x^", "x^"},
        {"x^ 2", "x^ 2"},
        {"x ^2", "x ^2"},
        {"~100 people", "~100 people"},
        {"about ~100", "about ~100"},
        // Escaped markers are literal, outside and inside the expression.
        {"x\\^2", "x^2"},
        {"x\\~z", "x~z"},
        {"x^a\\^b^", "x<sup>a^b</sup>"},
        // Two tildes are a strikethrough before one is a subscript.
        {"a~~b~~c~d~", "a<del>b</del>c<sub>d</sub>"},
        {"~~x~y~~", "<del>x<sub>y</sub></del>"},
        // Literal inside code, link destinations and autolinks.
        {"`x^2 x~z`", "<code>x^2 x~z</code>"},
        {"[a](http://x.org/a~b~c^d^e)", "<a href=\"http://x.org/a~b~c%5Ed%5Ee\">a</a>"},
        {"https://x.org/~user/a^b", "<a href=\"https://x.org/~user/a%5Eb\">https://x.org/~user/a^b</a>"},
        {"<https://x.org/a~b~c>", "<a href=\"https://x.org/a~b~c\">https://x.org/a~b~c</a>"},
    };
    CHECK_CASES(cases, inWriter);

    const Case plain[] = {
        {"100m^2", "100m^2"},
        {"y^(a+b)^", "y^(a+b)^"},
        {"x~z", "x~z"},
    };
    CHECK_CASES(plain, inStandard);

    // The caret of a footnote is not a superscript.
    const QString note = html(QStringLiteral("A[^n] b.\n\n[^n]: N."));
    QVERIFY2(note.contains(QLatin1StringView("footnote-ref")), qPrintable(note));
    QVERIFY2(!note.contains(QLatin1StringView("<sup>n")), qPrintable(note));

    // The short form has no closing marker; the delimited form has both.
    const md::Document doc = md::parse(QStringLiteral("100m^2 and y^(a+b)^"));
    const std::vector<const Node *> raised = all(doc, NodeType::Superscript);
    QCOMPARE(raised.size(), size_t(2));
    QCOMPARE(doc.source(*raised[0]).toString(), QStringLiteral("^2"));
    QCOMPARE(doc.source(0, raised[0]->open).toString(), QStringLiteral("^"));
    QVERIFY(raised[0]->close.isEmpty());
    QCOMPARE(doc.source(*raised[1]).toString(), QStringLiteral("^(a+b)^"));
    QCOMPARE(doc.source(0, raised[1]->close).toString(), QStringLiteral("^"));
    QCOMPARE(md::plainText(*raised[1]), QStringLiteral("(a+b)"));
}

void headingsAndBreaks()
{
    const Case both[] = {
        {"# One", "<h1>One</h1>\n"},
        {"## Two", "<h2>Two</h2>\n"},
        {"### Three", "<h3>Three</h3>\n"},
        {"#### Four", "<h4>Four</h4>\n"},
        {"##### Five", "<h5>Five</h5>\n"},
        {"###### Six", "<h6>Six</h6>\n"},
        {"####### Seven", "<p>####### Seven</p>\n"},
        {"#NoSpace", "<p>#NoSpace</p>\n"},
        {"## Closed ##", "<h2>Closed</h2>\n"},
        {"Title\n=====", "<h1>Title</h1>\n"},
        {"Title\n-----", "<h2>Title</h2>\n"},
        {"Two\nlines\n===", "<h1>Two\nlines</h1>\n"},
        // Under text a row of = or - is an underline; after a blank line it is not.
        {"Text\n==", "<h1>Text</h1>\n"},
        {"Text\n\n==", "<p>Text</p>\n<p>==</p>\n"},
        {"Text\n---", "<h2>Text</h2>\n"},
        {"Text\n\n---", "<p>Text</p>\n<hr />\n"},
        {"---", "<hr />\n"},
        {"* * *", "<hr />\n"},
        {"___", "<hr />\n"},
        {"- - -", "<hr />\n"},
        {"Text\n***", "<p>Text</p>\n<hr />\n"},
        {"Text\n___", "<p>Text</p>\n<hr />\n"},
        {"--", "<p>--</p>\n"},
        // Paragraphs and breaks.
        {"a\nb", "<p>a\nb</p>\n"},
        {"a\n\nb", "<p>a</p>\n<p>b</p>\n"},
        {"a  \nb", "<p>a<br />\nb</p>\n"},
        {"a\\\nb", "<p>a<br />\nb</p>\n"},
        {"a \nb", "<p>a\nb</p>\n"},
        // Quotes.
        {"> quoted", "<blockquote>\n<p>quoted</p>\n</blockquote>\n"},
        {">> nested quotation", "<blockquote>\n<blockquote>\n<p>nested quotation</p>\n</blockquote>\n</blockquote>\n"},
        {"> a\n>\n> b", "<blockquote>\n<p>a</p>\n<p>b</p>\n</blockquote>\n"},
    };
    CHECK_CASES(both, blockWriter);
    CHECK_CASES(both, blockStandard);

    const Case writerOnly[] = {
        // A whole-line underline is a heading; == around text is a highlight.
        {"==marked==\n===", "<h1><mark>marked</mark></h1>\n"},
        {"==marked==", "<p><mark>marked</mark></p>\n"},
        // +++ alone on a line after a blank line is a page break.
        {"a\n\n+++\n\nb", "<p>a</p>\n<div class=\"page-break\"></div>\n<p>b</p>\n"},
        {"+++\n\na", "<div class=\"page-break\"></div>\n<p>a</p>\n"},
        {"a\n\n+++  \nb", "<p>a</p>\n<div class=\"page-break\"></div>\n<p>b</p>\n"},
        {"a\n+++", "<p>a\n+++</p>\n"},
        {"# Title\n+++", "<h1>Title</h1>\n<p>+++</p>\n"},
        {"a\n\n+++ x", "<p>a</p>\n<p>+++ x</p>\n"},
        {"a\n\n++++", "<p>a</p>\n<p>++++</p>\n"},
        {"a\n\n++", "<p>a</p>\n<p>++</p>\n"},
        // Not special in code, and it is not a list.
        {"```\n\n+++\n```", "<pre><code>\n+++\n</code></pre>\n"},
        {"a\n\n    +++", "<p>a</p>\n<pre><code>+++\n</code></pre>\n"},
        {"> a\n>\n> +++", "<blockquote>\n<p>a</p>\n<p>+++</p>\n</blockquote>\n"},
    };
    CHECK_CASES(writerOnly, blockWriter);

    const Case standardOnly[] = {
        {"a\n\n+++\n\nb", "<p>a</p>\n<p>+++</p>\n<p>b</p>\n"},
        {"==marked==\n===", "<h1>==marked==</h1>\n"},
    };
    CHECK_CASES(standardOnly, blockStandard);

    // The heading node: level, kind and the punctuation that makes it.
    const md::Document atx = md::parse(QStringLiteral("## Closed ##\n"));
    const Node *heading = first(atx, NodeType::Heading);
    QVERIFY(heading);
    QCOMPARE(int(heading->level), 2);
    QVERIFY(!heading->setext);
    QCOMPARE(atx.source(0, heading->open).toString(), QStringLiteral("##"));
    QCOMPARE(atx.source(0, heading->close).toString(), QStringLiteral("##"));
    QCOMPARE(atx.source(*heading).toString(), QStringLiteral("## Closed ##"));
    const md::Document setext = md::parse(QStringLiteral("Title\n=====\n"));
    heading = first(setext, NodeType::Heading);
    QVERIFY(heading->setext);
    QCOMPARE(int(heading->level), 1);
    QCOMPARE(setext.source(0, heading->close).toString(), QStringLiteral("====="));
    QCOMPARE(setext.source(*heading).toString(), QStringLiteral("Title\n====="));
    const md::Document paged = md::parse(QStringLiteral("a\n\n+++\n"));
    const Node *page = first(paged, NodeType::PageBreak);
    QVERIFY(page);
    QCOMPARE(page->range, (md::Range{3, 6}));
}

void lists()
{
    const Case both[] = {
        {"* a\n* b", "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n"},
        {"- a\n- b", "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n"},
        {"+ a\n+ b", "<ul>\n<li>a</li>\n<li>b</li>\n</ul>\n"},
        {"- a\n* b", "<ul>\n<li>a</li>\n</ul>\n<ul>\n<li>b</li>\n</ul>\n"},
        {"1. a\n2. b", "<ol>\n<li>a</li>\n<li>b</li>\n</ol>\n"},
        {"1) a\n2) b", "<ol>\n<li>a</li>\n<li>b</li>\n</ol>\n"},
        {"1. a\n1. b\n1. c", "<ol>\n<li>a</li>\n<li>b</li>\n<li>c</li>\n</ol>\n"},
        {"- a\n\n- b", "<ul>\n<li>\n<p>a</p>\n</li>\n<li>\n<p>b</p>\n</li>\n</ul>\n"},
        {"- a\n\n  second", "<ul>\n<li>\n<p>a</p>\n<p>second</p>\n</li>\n</ul>\n"},
        // Nesting: four spaces, a tab, or the width of the parent's marker.
        {"- a\n    - b", "<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n</ul>\n"},
        {"- a\n\t- b", "<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n</ul>\n"},
        {"- a\n  - b", "<ul>\n<li>a\n<ul>\n<li>b</li>\n</ul>\n</li>\n</ul>\n"},
        {"1. one\n   - sub\n2. two", "<ol>\n<li>one\n<ul>\n<li>sub</li>\n</ul>\n</li>\n<li>two</li>\n</ol>\n"},
        {"1. one\n    - sub\n2. two", "<ol>\n<li>one\n<ul>\n<li>sub</li>\n</ul>\n</li>\n<li>two</li>\n</ol>\n"},
        {"- a\n    - b\n        - c",
         "<ul>\n<li>a\n<ul>\n<li>b\n<ul>\n<li>c</li>\n</ul>\n</li>\n</ul>\n</li>\n</ul>\n"},
        // Tasks, ordered and nested too.
        {"- [ ] pending\n- [x] done",
         "<ul>\n<li><input disabled=\"\" type=\"checkbox\" /> pending</li>\n"
         "<li><input checked=\"\" disabled=\"\" type=\"checkbox\" /> done</li>\n</ul>\n"},
        {"1. [ ] pending", "<ol>\n<li><input disabled=\"\" type=\"checkbox\" /> pending</li>\n</ol>\n"},
        {"- [x] outer\n    - [ ] inner",
         "<ul>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\" /> outer\n<ul>\n"
         "<li><input disabled=\"\" type=\"checkbox\" /> inner</li>\n</ul>\n</li>\n</ul>\n"},
        {"- [X] capital", "<ul>\n<li><input checked=\"\" disabled=\"\" type=\"checkbox\" /> capital</li>\n</ul>\n"},
        // Not tasks.
        {"- [] none", "<ul>\n<li>[] none</li>\n</ul>\n"},
        {"- [ ]none", "<ul>\n<li>[ ]none</li>\n</ul>\n"},
        {"- [y] none", "<ul>\n<li>[y] none</li>\n</ul>\n"},
        {"[ ] not in a list", "<p>[ ] not in a list</p>\n"},
        {"- text [x] later", "<ul>\n<li>text [x] later</li>\n</ul>\n"},
    };
    CHECK_CASES(both, blockWriter);
    CHECK_CASES(both, blockStandard);

    // A list that starts at 7 is rendered from 7 in Standard and from 1 in
    // Writer; the source keeps its numbers either way.
    const QString seven = QStringLiteral("7. seven\n8. eight\n");
    QCOMPARE(html(seven, standard()), QStringLiteral("<ol start=\"7\">\n<li>seven</li>\n<li>eight</li>\n</ol>\n"));
    QCOMPARE(html(seven, writer()), QStringLiteral("<ol>\n<li>seven</li>\n<li>eight</li>\n</ol>\n"));
    for (const md::Options &options : {writer(), standard()}) {
        const md::Document doc = md::parse(seven, options);
        const Node *list = first(doc, NodeType::List);
        QVERIFY(list && list->ordered && list->tight);
        QCOMPARE(list->sourceNumber, 7);
        const int start = options.profile == md::Profile::Writer ? 1 : 7;
        QCOMPARE(list->number, start);
        QCOMPARE(list->children.at(0).number, start);
        QCOMPARE(list->children.at(1).number, start + 1);
        QCOMPARE(list->marker, QChar(u'.'));
        QCOMPARE(doc.source(0, list->children.at(1).open).toString(), QStringLiteral("8."));
        QCOMPARE(doc.files.front().text, seven);
    }

    // Task items say so, and the marker is a node that can be edited.
    const QString tasks = QStringLiteral("- [ ] pending\n- [x] done\n- plain\n");
    const md::Document doc = md::parse(tasks);
    const std::vector<const Node *> items = all(doc, NodeType::ListItem);
    QCOMPARE(items.size(), size_t(3));
    QVERIFY(items[0]->task && !items[0]->checked);
    QVERIFY(items[1]->task && items[1]->checked);
    QVERIFY(!items[2]->task);
    const std::vector<const Node *> markers = all(doc, NodeType::TaskMarker);
    QCOMPARE(markers.size(), size_t(2));
    QCOMPARE(doc.source(*markers[0]).toString(), QStringLiteral("[ ]"));
    QCOMPARE(doc.source(*markers[1]).toString(), QStringLiteral("[x]"));
    QVERIFY(markers[1]->checked);
    const Node *bullets = first(doc, NodeType::List);
    QVERIFY(!bullets->ordered);
    QCOMPARE(bullets->marker, QChar(u'-'));
    QCOMPARE(doc.source(0, items[2]->open).toString(), QStringLiteral("-"));
}

void codeBlocks()
{
    const Case cases[] = {
        {"```\ncode\n```", "<pre><code>code\n</code></pre>\n"},
        {"```python\nprint(1)\n```", "<pre><code class=\"language-python\">print(1)\n</code></pre>\n"},
        {"~~~\ncode\n~~~", "<pre><code>code\n</code></pre>\n"},
        {"~~~ruby startline=3\nx\n~~~", "<pre><code class=\"language-ruby\">x\n</code></pre>\n"},
        // A fence closes only with one at least as long, of the same character.
        {"````\n```\ninner\n```\n````", "<pre><code>```\ninner\n```\n</code></pre>\n"},
        {"```\ncode\n~~~\n```", "<pre><code>code\n~~~\n</code></pre>\n"},
        {"```\nunclosed", "<pre><code>unclosed\n</code></pre>\n"},
        // Four spaces or a tab.
        {"    indented", "<pre><code>indented\n</code></pre>\n"},
        {"\tindented", "<pre><code>indented\n</code></pre>\n"},
        {"    a\n\n    b", "<pre><code>a\n\nb\n</code></pre>\n"},
        // Nothing in code is formatting.
        {"```\n**no** ==no== [%no] {{TOC}} $no$ [^no] <b>\n// no\n+++\n```",
         "<pre><code>**no** ==no== [%no] {{TOC}} $no$ [^no] &lt;b&gt;\n// no\n+++\n</code></pre>\n"},
        {"    // kept\n    +++", "<pre><code>// kept\n+++\n</code></pre>\n"},
    };
    CHECK_CASES(cases, blockWriter);
    CHECK_CASES(cases, blockStandard);

    const QString text = QStringLiteral("~~~python title\nprint(1)\n~~~\n");
    const md::Document doc = md::parse(text);
    const Node *code = first(doc, NodeType::CodeBlock);
    QVERIFY(code);
    QVERIFY(code->fenced);
    QCOMPARE(code->label, QStringLiteral("python title"));
    QCOMPARE(code->text, QStringLiteral("print(1)\n"));
    QCOMPARE(code->marker, QChar(u'~'));
    QCOMPARE(doc.source(0, code->open).toString(), QStringLiteral("~~~"));
    QCOMPARE(doc.source(0, code->close).toString(), QStringLiteral("~~~"));
    QCOMPARE(doc.source(0, code->target).toString(), QStringLiteral("python title"));
    QCOMPARE(doc.source(*code).toString(), QStringLiteral("~~~python title\nprint(1)\n~~~"));
    const md::Document plainCode = md::parse(QStringLiteral("    a\n"));
    const Node *indented = first(plainCode, NodeType::CodeBlock);
    QVERIFY(indented && !indented->fenced);
    QCOMPARE(indented->text, QStringLiteral("a\n"));
}

void linksAndImages()
{
    const Case cases[] = {
        {"[label](https://example.org \"title\")", "<a href=\"https://example.org\" title=\"title\">label</a>"},
        {"[label](https://example.org)", "<a href=\"https://example.org\">label</a>"},
        {"[label][ref]\n\n[ref]: https://example.org \"title\"",
         "<a href=\"https://example.org\" title=\"title\">label</a>"},
        {"[ref][]\n\n[ref]: https://example.org", "<a href=\"https://example.org\">ref</a>"},
        {"[ref]\n\n[ref]: https://example.org", "<a href=\"https://example.org\">ref</a>"},
        {"[label][REF]\n\n[ref]: https://example.org", "<a href=\"https://example.org\">label</a>"},
        // Escapes and character references in destinations and titles.
        {"[a](https://example.org/a\\(b\\) \"t &amp; \\\"q\\\"\")",
         "<a href=\"https://example.org/a(b)\" title=\"t &amp; &quot;q&quot;\">a</a>"},
        {"[a](<my file.md>)", "<a href=\"my%20file.md\">a</a>"},
        {"[a](café.md)", "<a href=\"caf%C3%A9.md\">a</a>"},
        {"[a](/abs/path.md#part)", "<a href=\"/abs/path.md#part\">a</a>"},
        {"\\[not a link](x)", "[not a link](x)"},
        // Autolinks, in angle brackets and bare.
        {"<https://example.org>", "<a href=\"https://example.org\">https://example.org</a>"},
        {"<me@example.org>", "<a href=\"mailto:me@example.org\">me@example.org</a>"},
        {"See https://example.org/path. Next", "See <a href=\"https://example.org/path\">https://example.org/path</a>. Next"},
        {"www.example.org", "<a href=\"http://www.example.org\">www.example.org</a>"},
        {"write to me@example.org.", "write to <a href=\"mailto:me@example.org\">me@example.org</a>."},
        {"(https://example.org/a_(b))", "(<a href=\"https://example.org/a_(b)\">https://example.org/a_(b)</a>)"},
        {"`https://example.org`", "<code>https://example.org</code>"},
        // Images.
        {"![alt text](./image.png \"caption\")", "<img src=\"./image.png\" alt=\"alt text\" title=\"caption\" />"},
        {"![remote](https://example.org/i.png)", "<img src=\"https://example.org/i.png\" alt=\"remote\" />"},
        {"![*a* **b**](x.png)", "<img src=\"x.png\" alt=\"a b\" />"},
        {"![alt][pic]\n\n[pic]: pic.png \"t\"", "<img src=\"pic.png\" alt=\"alt\" title=\"t\" />"},
        // No link inside a link.
        {"[a [b](c) d](e)", "[a <a href=\"c\">b</a> d](e)"},
        // An unresolved reference stays visible.
        {"[label][nope]", "[label][nope]"},
        {"[nope][]", "[nope][]"},
    };
    CHECK_CASES(cases, inWriter);
    CHECK_CASES(cases, inStandard);

    // The link node carries its parts and where they are written.
    const QString text = QStringLiteral("See [the *label*](./a%20b.md \"T\") and [ref][Other].\n\n[other]: <x y.md> 'Title'\n");
    const md::Document doc = md::parse(text);
    const std::vector<const Node *> links = all(doc, NodeType::Link);
    QCOMPARE(links.size(), size_t(2));
    QCOMPARE(links[0]->dest, QStringLiteral("./a%20b.md"));
    QCOMPARE(links[0]->title, QStringLiteral("T"));
    QVERIFY(links[0]->label.isEmpty());
    QCOMPARE(doc.source(0, links[0]->open).toString(), QStringLiteral("["));
    QCOMPARE(doc.source(0, links[0]->close).toString(), QStringLiteral("](./a%20b.md \"T\")"));
    QCOMPARE(doc.source(0, links[0]->target).toString(), QStringLiteral("./a%20b.md"));
    QCOMPARE(md::plainText(*links[0]), QStringLiteral("the label"));
    QCOMPARE(links[1]->dest, QStringLiteral("x y.md"));
    QCOMPARE(links[1]->title, QStringLiteral("Title"));
    QCOMPARE(links[1]->label, QStringLiteral("Other"));
    QCOMPARE(doc.source(0, links[1]->target).toString(), QStringLiteral("Other"));
    const Node *definition = first(doc, NodeType::LinkDefinition);
    QVERIFY(definition);
    QCOMPARE(definition->label, QStringLiteral("other"));
    QCOMPARE(definition->dest, QStringLiteral("x y.md"));
    QCOMPARE(definition->title, QStringLiteral("Title"));
    QCOMPARE(doc.source(*definition).toString(), QStringLiteral("[other]: <x y.md> 'Title'"));
    QCOMPARE(doc.source(0, definition->target).toString(), QStringLiteral("x y.md"));
    QCOMPARE(count(doc, DiagnosticCode::UnresolvedReference), 0);

    // An unresolved full reference is reported; plain brackets are not.
    const md::Document broken = md::parse(QStringLiteral("[label][nope] and [just brackets]\n"));
    QCOMPARE(count(broken, DiagnosticCode::UnresolvedReference), 1);
    QCOMPARE(broken.source(0, broken.diagnostics.front().range).toString(), QStringLiteral("[label][nope]"));
    QVERIFY(all(broken, NodeType::Link).empty());

    // Destinations: local paths resolve from the folder of their document,
    // absolute paths are absolute, and other schemes are told apart.
    using Kind = md::Destination::Kind;
    const QString base = QStringLiteral("/docs/book");
    md::Destination d = md::classifyDestination(QStringLiteral("./image.png"), base);
    QCOMPARE(d.kind, Kind::LocalFile);
    QCOMPARE(d.path, QStringLiteral("/docs/book/image.png"));
    QVERIFY(d.absolute);
    d = md::classifyDestination(QStringLiteral("../shared/my%20image.png#top"), base);
    QCOMPARE(d.path, QStringLiteral("/docs/shared/my image.png"));
    QCOMPARE(d.fragment, QStringLiteral("top"));
    d = md::classifyDestination(QStringLiteral("/usr/share/pic.png"), base);
    QCOMPARE(d.kind, Kind::LocalFile);
    QCOMPARE(d.path, QStringLiteral("/usr/share/pic.png"));
    QVERIFY(d.absolute);
    d = md::classifyDestination(QStringLiteral("image.png"), QString());
    QCOMPARE(d.kind, Kind::LocalFile);
    QCOMPARE(d.path, QStringLiteral("image.png"));
    QVERIFY(!d.absolute);
    QCOMPARE(md::classifyDestination(QStringLiteral("https://example.org/i.png"), base).kind, Kind::Remote);
    QCOMPARE(md::classifyDestination(QStringLiteral("HTTP://example.org"), base).kind, Kind::Remote);
    QCOMPARE(md::classifyDestination(QStringLiteral("mailto:me@example.org"), base).kind, Kind::Mail);
    QCOMPARE(md::classifyDestination(QStringLiteral("ftp://example.org/x"), base).kind, Kind::OtherScheme);
    QCOMPARE(md::classifyDestination(QStringLiteral("#a-section"), base).kind, Kind::Anchor);
    QCOMPARE(md::classifyDestination(QStringLiteral("#a-section"), base).fragment, QStringLiteral("a-section"));
    QCOMPARE(md::classifyDestination(QString(), base).kind, Kind::Empty);
    d = md::classifyDestination(QStringLiteral("file:///tmp/a%20b.png"), base);
    QCOMPARE(d.kind, Kind::LocalFile);
    QCOMPARE(d.path, QStringLiteral("/tmp/a b.png"));
    // Paths that the application writes into Markdown read back as the same path.
    for (const QString &path : {QStringLiteral("pics/my image (1).png"), QStringLiteral("100% #1 <draft>.md"),
                                QStringLiteral("caf\u00e9/na\u00efve \"x\".txt"), QStringLiteral("plain.md"),
                                QStringLiteral("a&amp;b [1].png"), QStringLiteral("ab:c.png"), QStringLiteral("mailto:x.md"),
                                QStringLiteral("back\\slash?.md")}) {
        const QString encoded = md::encodeDestination(path);
        QVERIFY2(!encoded.contains(u' ') && !encoded.contains(u'('), qPrintable(encoded));
        const md::Document linked = md::parse(QStringLiteral("[t](%1) ![i](%1 \"title\")").arg(encoded));
        QCOMPARE(md::classifyDestination(first(linked, NodeType::Link)->dest, base).path, base + u'/' + path);
        QCOMPARE(md::classifyDestination(first(linked, NodeType::Image)->dest, base).path, base + u'/' + path);
    }
    QCOMPARE(md::encodeDestination(u"pics/my image (1).png"), QStringLiteral("pics/my%20image%20%281%29.png"));
    QCOMPARE(md::encodeDestination(u"plain.md"), QStringLiteral("plain.md"));
    // An entity or a scheme is never read into a path.
    QCOMPARE(md::encodeDestination(u"a&amp;b:c.md"), QStringLiteral("a%26amp;b%3Ac.md"));

    // A document knows the folder of each of its files.
    md::Source source;
    source.path = QStringLiteral("/docs/book/main.md");
    const md::Document placed = md::parse(QStringLiteral("![x](pics/a.png)\n"), writer(), source);
    QCOMPARE(placed.files.front().directory, QStringLiteral("/docs/book"));
    QCOMPARE(md::classifyDestination(placed, *first(placed, NodeType::Image)).path,
             QStringLiteral("/docs/book/pics/a.png"));
}

void headingLabelsAndSlugs()
{
    // Both Writer forms of a heading reference, as the dialect gives them.
    const QString text = QStringLiteral(
        "# A section [Section Label]\n\n"
        "See [Section Label][] or [jump here][Section Label].\n\n"
        "# Another section\n\n"
        "See [another location][Another section].\n");
    QCOMPARE(html(text, writer(), true),
             QStringLiteral("<h1 id=\"a-section\">A section</h1>\n"
                            "<p>See <a href=\"#a-section\">Section Label</a> or <a href=\"#a-section\">jump here</a>.</p>\n"
                            "<h1 id=\"another-section\">Another section</h1>\n"
                            "<p>See <a href=\"#another-section\">another location</a>.</p>\n"));
    const md::Document doc = md::parse(text);
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));
    const Node *heading = first(doc, NodeType::Heading);
    QCOMPARE(heading->label, QStringLiteral("Section Label"));
    QCOMPARE(heading->dest, QStringLiteral("a-section"));
    const Node *label = first(doc, NodeType::HeadingLabel);
    QVERIFY(label);
    QCOMPARE(doc.source(*label).toString(), QStringLiteral("[Section Label]"));
    QCOMPARE(md::plainText(*heading), QStringLiteral("A section"));

    const Case cases[] = {
        // Matching ignores case and the amount of white space.
        {"# A section [Section Label]\n\n[x][  section   LABEL ]", "<h1>A section</h1>\n<p><a href=\"#a-section\">x</a></p>\n"},
        {"# Another section\n\n[x][ANOTHER  section]", "<h1>Another section</h1>\n<p><a href=\"#another-section\">x</a></p>\n"},
        // A heading's text matches with or without its formatting.
        {"# Some *fine* text\n\n[a][Some fine text] [b][Some *fine* text]",
         "<h1>Some <em>fine</em> text</h1>\n<p><a href=\"#some-fine-text\">a</a> <a href=\"#some-fine-text\">b</a></p>\n"},
        // A declared definition wins over a heading of the same name.
        {"# Notes\n\n[x][Notes]\n\n[Notes]: /url", "<h1>Notes</h1>\n<p><a href=\"/url\">x</a></p>\n"},
        {"# Notes [N]\n\n[x][N]\n\n[N]: /url", "<h1>Notes <a href=\"/url\">N</a></h1>\n<p><a href=\"/url\">x</a></p>\n"},
        // Headings may be referred to before they appear, and from headings.
        {"[x][Later]\n\n# Later", "<p><a href=\"#later\">x</a></p>\n<h1>Later</h1>\n"},
        {"# First\n\n# See [x][First]", "<h1>First</h1>\n<h1>See <a href=\"#first\">x</a></h1>\n"},
        // A bare bracket is not a heading reference, and neither is an image.
        {"# Topic\n\n[Topic] and ![a][Topic]", "<h1>Topic</h1>\n<p>[Topic] and ![a][Topic]</p>\n"},
        // Generated anchors.
        {"# A section\n\n[jump](#a-section)", "<h1>A section</h1>\n<p><a href=\"#a-section\">jump</a></p>\n"},
        // The label needs text before it and a space, and may be escaped.
        {"# [Only]", "<h1>[Only]</h1>\n"},
        {"# Text[Glued]", "<h1>Text[Glued]</h1>\n"},
        {"# Text \\[Not a label\\]", "<h1>Text [Not a label]</h1>\n"},
        {"# Text [a](b)", "<h1>Text <a href=\"b\">a</a></h1>\n"},
        {"Setext [Label]\n===\n\n[x][Label]", "<h1>Setext</h1>\n<p><a href=\"#setext\">x</a></p>\n"},
    };
    CHECK_CASES(cases, blockWriter);

    // None of this is Standard Markdown.
    QCOMPARE(html(QStringLiteral("# A section [Section Label]\n\n[x][Section Label]"), standard()),
             QStringLiteral("<h1>A section [Section Label]</h1>\n<p>[x][Section Label]</p>\n"));

    // An unresolved reference stays visible and is reported.
    const md::Document missing = md::parse(QStringLiteral("# One\n\n[x][Two] and [y](#three) and [z](#one)\n"));
    QCOMPARE(count(missing, DiagnosticCode::UnresolvedReference), 1);
    QCOMPARE(count(missing, DiagnosticCode::UnresolvedAnchor), 1);
    QVERIFY(md::toHtml(missing).contains(QLatin1StringView("[x][Two]")));

    // Two headings with one name: the first is linked, and the reference says so.
    const md::Document twice = md::parse(QStringLiteral("# Intro\n\ntext\n\n# Intro\n\n[x][Intro]\n"));
    QCOMPARE(count(twice, DiagnosticCode::AmbiguousHeadingReference), 1);
    QCOMPARE(first(twice, NodeType::Link)->dest, QStringLiteral("#intro"));
    // Duplicate headings that nothing refers to are no problem.
    QCOMPARE(md::parse(QStringLiteral("# Intro\n\n# Intro\n")).diagnostics.size(), size_t(0));
    // An explicit label settles it.
    const md::Document labelled = md::parse(QStringLiteral("# Intro\n\n# Intro [Second]\n\n[x][Second]\n"));
    QVERIFY2(labelled.diagnostics.empty(), qPrintable(describe(labelled)));
    QCOMPARE(first(labelled, NodeType::Link)->dest, QStringLiteral("#intro-2"));

    // Slugs: lower case, Unicode letters and digits kept, punctuation and
    // formatting removed, each run of white space one hyphen, duplicates
    // numbered. The examples are a fixture, so that the preview and the
    // exports can be held to the same ids.
    const QJsonObject fixture = QJsonDocument::fromJson(readFixture("heading-slugs.json")).object();
    const QJsonArray examples = fixture[QLatin1StringView("headings")].toArray();
    QVERIFY(examples.size() >= 12);
    for (const QJsonValue &value : examples) {
        const QString heading = value[QLatin1StringView("text")].toString();
        const QString slug = value[QLatin1StringView("slug")].toString();
        QVERIFY2(md::slugify(heading) == slug, qPrintable(heading + QStringLiteral(" -> ") + md::slugify(heading)));
    }
    const QJsonObject whole = fixture[QLatin1StringView("document")].toObject();
    const md::Document many = md::parse(whole[QLatin1StringView("markdown")].toString());
    QStringList ids;
    for (const md::Heading &h : many.headings) ids.append(h.slug);
    QStringList expectedIds;
    for (const QJsonValue &value : whole[QLatin1StringView("slugs")].toArray()) expectedIds.append(value.toString());
    QCOMPARE(ids, expectedIds);
    QCOMPARE(ids.size(), 11);
    // The ids are those of the rendered headings.
    const QString rendered = md::toHtml(many);
    for (const QString &id : std::as_const(ids)) QVERIFY2(rendered.contains(QStringLiteral(" id=\"%1\">").arg(id)), qPrintable(id));
    QCOMPARE(html(QStringLiteral("# Intro\n\n# Intro"), writer(), true),
             QStringLiteral("<h1 id=\"intro\">Intro</h1>\n<h1 id=\"intro-2\">Intro</h1>\n"));
}

void tableOfContents()
{
    const QString text = QStringLiteral(
        "{{TOC}}\n\n# One\n\n## Two\n\n### Three\n\n## Four [Label]\n\n# *Five* `5`\n");
    QCOMPARE(html(text, writer(), true),
             QStringLiteral("<nav class=\"toc\">\n"
                            "<ul>\n"
                            "<li><a href=\"#one\">One</a>\n"
                            "<ul>\n"
                            "<li><a href=\"#two\">Two</a>\n"
                            "<ul>\n"
                            "<li><a href=\"#three\">Three</a></li>\n"
                            "</ul>\n"
                            "</li>\n"
                            "<li><a href=\"#four\">Four</a></li>\n"
                            "</ul>\n"
                            "</li>\n"
                            "<li><a href=\"#five-5\">Five 5</a></li>\n"
                            "</ul>\n"
                            "</nav>\n"
                            "<h1 id=\"one\">One</h1>\n"
                            "<h2 id=\"two\">Two</h2>\n"
                            "<h3 id=\"three\">Three</h3>\n"
                            "<h2 id=\"four\">Four</h2>\n"
                            "<h1 id=\"five-5\"><em>Five</em> <code>5</code></h1>\n"));

    // The contents come from the outline and add nothing to it.
    const md::Document doc = md::parse(text);
    QCOMPARE(doc.headings.size(), size_t(5));
    const Node *toc = first(doc, NodeType::TableOfContents);
    QVERIFY(toc);
    QCOMPARE(doc.source(*toc).toString(), QStringLiteral("{{TOC}}"));
    QCOMPARE(toc->children.size(), size_t(1));
    QVERIFY(toc->children.front().generated);
    QCOMPARE(all(doc, NodeType::Heading).size(), size_t(5));

    const Case cases[] = {
        // Only alone in its own paragraph.
        {"See {{TOC}} here\n\n# H", "<p>See {{TOC}} here</p>\n<h1>H</h1>\n"},
        {"{{TOC}}\nmore\n\n# H", "<p>{{TOC}}\nmore</p>\n<h1>H</h1>\n"},
        {"{{toc}}\n\n# H", "<p>{{toc}}</p>\n<h1>H</h1>\n"},
        // Literal in code.
        {"`{{TOC}}`\n\n# H", "<p><code>{{TOC}}</code></p>\n<h1>H</h1>\n"},
        {"```\n{{TOC}}\n```\n\n# H", "<pre><code>{{TOC}}\n</code></pre>\n<h1>H</h1>\n"},
        {"    {{TOC}}\n\n# H", "<pre><code>{{TOC}}\n</code></pre>\n<h1>H</h1>\n"},
        // It may come after the headings, and levels may skip.
        {"### Deep\n\n# Top\n\n{{TOC}}",
         "<h3>Deep</h3>\n<h1>Top</h1>\n<nav class=\"toc\">\n<ul>\n<li><a href=\"#deep\">Deep</a></li>\n"
         "<li><a href=\"#top\">Top</a></li>\n</ul>\n</nav>\n"},
        // Without headings there is nothing to list.
        {"{{TOC}}\n\ntext", "<p>text</p>\n"},
    };
    CHECK_CASES(cases, blockWriter);
    QCOMPARE(html(QStringLiteral("{{TOC}}\n\n# H"), standard()), QStringLiteral("<p>{{TOC}}</p>\n<h1>H</h1>\n"));
}

void tables()
{
    const QString sample = QStringLiteral(
        "| Item | Center | Amount |\n"
        "| :--- | :----: | -----: |\n"
        "| One  | middle | 12     |\n");
    const QString expected = QStringLiteral(
        "<table>\n<thead>\n<tr>\n"
        "<th align=\"left\">Item</th>\n<th align=\"center\">Center</th>\n<th align=\"right\">Amount</th>\n"
        "</tr>\n</thead>\n<tbody>\n<tr>\n"
        "<td align=\"left\">One</td>\n<td align=\"center\">middle</td>\n<td align=\"right\">12</td>\n"
        "</tr>\n</tbody>\n</table>\n");
    QCOMPARE(html(sample, writer()), expected);
    QCOMPARE(html(sample, standard()), expected);
    // Padding does not change the alignment or the content.
    QCOMPARE(html(QStringLiteral("|Item|Center|Amount|\n|:-|:-:|-:|\n|One|middle|12|\n")), expected);
    // The outer pipes may be left out.
    QCOMPARE(html(QStringLiteral("Item | Center | Amount\n:--- | :----: | -----:\nOne | middle | 12\n")), expected);

    const Case both[] = {
        // An escaped pipe is text, in a cell and in code inside a cell.
        {"| a \\| b |\n| --- |\n| `c \\| d` |",
         "<table>\n<thead>\n<tr>\n<th>a | b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td><code>c | d</code></td>\n</tr>\n</tbody>\n</table>\n"},
        // Inline formatting in cells.
        {"| **b** | *i* |\n| --- | --- |\n| [l](u) | ~~s~~ |",
         "<table>\n<thead>\n<tr>\n<th><strong>b</strong></th>\n<th><em>i</em></th>\n</tr>\n</thead>\n<tbody>\n<tr>\n"
         "<td><a href=\"u\">l</a></td>\n<td><del>s</del></td>\n</tr>\n</tbody>\n</table>\n"},
        // The header and the delimiter row must agree.
        {"| a | b |\n| --- |\n| c |", "<p>| a | b |\n| --- |\n| c |</p>\n"},
        {"| a | b |\n| c | d |", "<p>| a | b |\n| c | d |</p>\n"},
        // Short rows are filled; a blank line ends the table.
        {"| a | b |\n| - | - |\n| c |\n\nafter",
         "<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>c</td>\n<td></td>\n</tr>\n"
         "</tbody>\n</table>\n<p>after</p>\n"},
        // A cell of spaces is an empty cell, never a merge.
        {"| a | b | c |\n| - | - | - |\n| x |  | z |",
         "<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n<th>c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>x</td>\n<td></td>\n"
         "<td>z</td>\n</tr>\n</tbody>\n</table>\n"},
    };
    CHECK_CASES(both, blockWriter);
    CHECK_CASES(both, blockStandard);

    // A pipe inside a code span: the grammar decides. In Writer it belongs
    // to the code; GFM splits the row there unless it is escaped.
    const QString codePipe = QStringLiteral("| `a|b` | c |\n| --- | --- |\n| `d|e` | f |\n");
    QCOMPARE(html(codePipe, writer()),
             QStringLiteral("<table>\n<thead>\n<tr>\n<th><code>a|b</code></th>\n<th>c</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n"
                            "<td><code>d|e</code></td>\n<td>f</td>\n</tr>\n</tbody>\n</table>\n"));
    QCOMPARE(html(codePipe, standard()), QStringLiteral("<p>| <code>a|b</code> | c |\n| --- | --- |\n| <code>d|e</code> | f |</p>\n"));

    // Merged cells: a cell followed by an extra delimiter spans one more column.
    const QString merged = QStringLiteral("| a | b | c |\n| --- | --- | --- |\n| joined || third |\n| all |||\n| x | wide ||\n");
    QCOMPARE(html(merged, writer()),
             QStringLiteral("<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n<th>c</th>\n</tr>\n</thead>\n<tbody>\n"
                            "<tr>\n<td colspan=\"2\">joined</td>\n<td>third</td>\n</tr>\n"
                            "<tr>\n<td colspan=\"3\">all</td>\n</tr>\n"
                            "<tr>\n<td>x</td>\n<td colspan=\"2\">wide</td>\n</tr>\n"
                            "</tbody>\n</table>\n"));
    // Standard keeps the ordinary GFM reading: an empty cell.
    QCOMPARE(html(QStringLiteral("| a | b | c |\n| --- | --- | --- |\n| joined || third |\n"), standard()),
             QStringLiteral("<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n<th>c</th>\n</tr>\n</thead>\n<tbody>\n"
                            "<tr>\n<td>joined</td>\n<td></td>\n<td>third</td>\n</tr>\n</tbody>\n</table>\n"));
    md::Options noMerge = writer();
    noMerge.mergedCells = false;
    QVERIFY(html(merged, noMerge).contains(QLatin1StringView("<td>joined</td>\n<td></td>\n<td>third</td>")));

    const md::Document doc = md::parse(merged);
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));
    const Node *table = first(doc, NodeType::Table);
    QCOMPARE(table->columns, 3);
    QCOMPARE(doc.source(0, table->open).toString(), QStringLiteral("| --- | --- | --- |"));
    const std::vector<const Node *> rows = all(doc, NodeType::TableRow);
    QCOMPARE(rows.size(), size_t(4));
    QVERIFY(rows[0]->header && !rows[1]->header);
    QCOMPARE(rows[1]->children.size(), size_t(2));
    QCOMPARE(rows[1]->children[0].span, 2);
    QCOMPARE(rows[1]->children[0].column, 0);
    QCOMPARE(rows[1]->children[1].span, 1);
    QCOMPARE(rows[1]->children[1].column, 2);
    QCOMPARE(doc.source(rows[1]->children[0]).toString(), QStringLiteral("joined"));
    QCOMPARE(rows[1]->markers.size(), size_t(4));

    // A merged header counts the columns it spans.
    QVERIFY(html(QStringLiteral("| wide || c |\n| - | - | - |\n")).contains(QLatin1StringView("<th colspan=\"2\">wide</th>")));

    // A row whose spans do not fit stays in the source, is reported, and
    // loses none of the cells that do fit.
    const md::Document tooWide = md::parse(QStringLiteral("| a | b |\n| - | - |\n| one || two | three |\n"));
    QCOMPARE(count(tooWide, DiagnosticCode::MalformedTableRow), 1);
    QCOMPARE(tooWide.source(0, tooWide.diagnostics.front().range).toString(), QStringLiteral("| one || two | three |"));
    QVERIFY(md::toHtml(tooWide).contains(QLatin1StringView("<td colspan=\"2\">one</td>")));
    QCOMPARE(tooWide.files.front().text, QStringLiteral("| a | b |\n| - | - |\n| one || two | three |\n"));
    const md::Document leading = md::parse(QStringLiteral("| a | b |\n| - | - |\n|| x | y |\n"));
    QCOMPARE(count(leading, DiagnosticCode::MalformedTableRow), 1);
    // Standard rows with too many cells are ordinary GFM: the excess is ignored.
    QCOMPARE(md::parse(QStringLiteral("| a | b |\n| - | - |\n| 1 | 2 | 3 |\n"), standard()).diagnostics.size(), size_t(0));

    // Alignment is on every cell of its column.
    const md::Document aligned = md::parse(sample);
    const std::vector<const Node *> cells = all(aligned, NodeType::TableCell);
    QCOMPARE(cells.size(), size_t(6));
    QCOMPARE(cells[0]->align, md::Align::Left);
    QCOMPARE(cells[4]->align, md::Align::Center);
    QCOMPARE(cells[5]->align, md::Align::Right);
    QVERIFY(cells[0]->header && !cells[3]->header);
}

void definitionLists()
{
    const Case cases[] = {
        {"Term\n: Definition", "<dl>\n<dt>Term</dt>\n<dd>Definition</dd>\n</dl>\n"},
        {"Term 1\nTerm 2\n: Definition a\n: Definition b",
         "<dl>\n<dt>Term 1</dt>\n<dt>Term 2</dt>\n<dd>Definition a</dd>\n<dd>Definition b</dd>\n</dl>\n"},
        // A blank line before the definition makes it a paragraph.
        {"Term\n\n: Definition", "<dl>\n<dt>Term</dt>\n<dd>\n<p>Definition</p>\n</dd>\n</dl>\n"},
        // Indented blocks continue a definition.
        {"Term\n: First\n\n    Second paragraph",
         "<dl>\n<dt>Term</dt>\n<dd>\n<p>First</p>\n<p>Second paragraph</p>\n</dd>\n</dl>\n"},
        {"Term\n:   Definition\n\n        code",
         "<dl>\n<dt>Term</dt>\n<dd>\n<p>Definition</p>\n<pre><code>code\n</code></pre>\n</dd>\n</dl>\n"},
        {"Term\n: - one\n  - two", "<dl>\n<dt>Term</dt>\n<dd>\n<ul>\n<li>one</li>\n<li>two</li>\n</ul>\n</dd>\n</dl>\n"},
        // Formatting stays structured.
        {"**Term**\n: A *definition* with `code`",
         "<dl>\n<dt><strong>Term</strong></dt>\n<dd>A <em>definition</em> with <code>code</code></dd>\n</dl>\n"},
        // Groups make one list.
        {"A\n: a\n\nB\n: b", "<dl>\n<dt>A</dt>\n<dd>a</dd>\n<dt>B</dt>\n<dd>b</dd>\n</dl>\n"},
        {"A\n: a\n\n: second\n\nafter",
         "<dl>\n<dt>A</dt>\n<dd>a</dd>\n<dd>\n<p>second</p>\n</dd>\n</dl>\n<p>after</p>\n"},
        // A line that follows without indentation continues the definition.
        {"Term\n: Definition\ncontinues", "<dl>\n<dt>Term</dt>\n<dd>Definition\ncontinues</dd>\n</dl>\n"},
        // Not definitions.
        {": no term", "<p>: no term</p>\n"},
        {"Term\n:no space", "<p>Term\n:no space</p>\n"},
        {"# Heading\n: after a heading", "<h1>Heading</h1>\n<p>: after a heading</p>\n"},
        {"```\nTerm\n: Definition\n```", "<pre><code>Term\n: Definition\n</code></pre>\n"},
    };
    CHECK_CASES(cases, blockWriter);
    QCOMPARE(html(QStringLiteral("Term\n: Definition"), standard()), QStringLiteral("<p>Term\n: Definition</p>\n"));

    const QString text = QStringLiteral("Term 1\nTerm 2\n: Definition a\n\n: Definition b\n");
    const md::Document doc = md::parse(text);
    const Node *list = first(doc, NodeType::DefinitionList);
    QVERIFY(list);
    QCOMPARE(list->children.size(), size_t(4));
    QCOMPARE(list->children[0].type, NodeType::DefinitionTerm);
    QCOMPARE(list->children[1].type, NodeType::DefinitionTerm);
    QCOMPARE(list->children[2].type, NodeType::DefinitionDescription);
    QVERIFY(list->children[2].tight);
    QVERIFY(!list->children[3].tight);
    QCOMPARE(doc.source(list->children[1]).toString(), QStringLiteral("Term 2"));
    QCOMPARE(doc.source(0, list->children[2].open).toString(), QStringLiteral(":"));
    QCOMPARE(doc.source(list->children[2]).toString(), QStringLiteral(": Definition a"));
    QCOMPARE(doc.source(*list).toString(), text.trimmed());
}

void footnotes()
{
    const QString back = QStringLiteral("↩");
    auto ref = [](int number, int occurrence = 1) {
        const QString id = occurrence > 1 ? QStringLiteral("%1-%2").arg(number).arg(occurrence) : QString::number(number);
        return QStringLiteral("<sup class=\"footnote-ref\"><a href=\"#fn-%1\" id=\"fnref-%2\">%1</a></sup>").arg(number).arg(id);
    };
    auto backref = [&back](int number, int occurrence = 1) {
        const QString id = occurrence > 1 ? QStringLiteral("%1-%2").arg(number).arg(occurrence) : QString::number(number);
        return QStringLiteral("<a href=\"#fnref-%1\" class=\"footnote-backref\">%2</a>")
            .arg(id, occurrence > 1 ? back + QString::number(occurrence) : back);
    };
    auto notes = [](const QStringList &items) {
        QString out = QStringLiteral("<section class=\"footnotes\">\n<ol>\n");
        for (int i = 0; i < items.size(); ++i)
            out += QStringLiteral("<li id=\"fn-%1\">\n%2\n</li>\n").arg(i + 1).arg(items[i]);
        return out + QStringLiteral("</ol>\n</section>\n");
    };

    // A reference note and a Writer inline note, as the dialect gives them.
    QCOMPARE(html(QStringLiteral("A statement[^1].\n\n[^1]: The note text.\n")),
             QStringLiteral("<p>A statement%1.</p>\n").arg(ref(1))
                 + notes({QStringLiteral("<p>The note text. %1</p>").arg(backref(1))}));
    QCOMPARE(html(QStringLiteral("A statement[^The note itself.].\n")),
             QStringLiteral("<p>A statement%1.</p>\n").arg(ref(1))
                 + notes({QStringLiteral("<p>The note itself. %1</p>").arg(backref(1))}));

    // Numbers follow first use, not the order of the definitions.
    QCOMPARE(html(QStringLiteral("First[^b] then[^a].\n\n[^a]: Note A.\n[^b]: Note B.\n")),
             QStringLiteral("<p>First%1 then%2.</p>\n").arg(ref(1), ref(2))
                 + notes({QStringLiteral("<p>Note B. %1</p>").arg(backref(1)),
                          QStringLiteral("<p>Note A. %1</p>").arg(backref(2))}));

    // Repeated references share one note; each inline note is its own.
    QCOMPARE(html(QStringLiteral("One[^n] two[^n] three[^Same text.] four[^Same text.].\n\n[^n]: Shared.\n")),
             QStringLiteral("<p>One%1 two%2 three%3 four%4.</p>\n").arg(ref(1), ref(1, 2), ref(2), ref(3))
                 + notes({QStringLiteral("<p>Shared. %1 %2</p>").arg(backref(1), backref(1, 2)),
                          QStringLiteral("<p>Same text. %1</p>").arg(backref(2)),
                          QStringLiteral("<p>Same text. %1</p>").arg(backref(3))}));

    // A definition continues over indented paragraphs and may hold formatting.
    QCOMPARE(html(QStringLiteral("Text[^long].\n\n[^long]: First *paragraph*.\n\n    Second paragraph.\n\nAfter.\n")),
             QStringLiteral("<p>Text%1.</p>\n<p>After.</p>\n").arg(ref(1))
                 + notes({QStringLiteral("<p>First <em>paragraph</em>.</p>\n<p>Second paragraph. %1</p>").arg(backref(1))}));

    // Space just inside the brackets of an inline note is not part of it.
    QCOMPARE(html(QStringLiteral("Text[^ A spaced note. ].\n")),
             QStringLiteral("<p>Text%1.</p>\n").arg(ref(1)) + notes({QStringLiteral("<p>A spaced note. %1</p>").arg(backref(1))}));

    // An inline note may hold formatting, links and brackets.
    QCOMPARE(html(QStringLiteral("Text[^See *this* [page](https://example.org) [sic].].\n")),
             QStringLiteral("<p>Text%1.</p>\n").arg(ref(1))
                 + notes({QStringLiteral("<p>See <em>this</em> <a href=\"https://example.org\">page</a> [sic]. %1</p>")
                              .arg(backref(1))}));

    // Telling the two apart when nothing is defined: one word is a reference
    // that is missing; anything with space or other punctuation is a note.
    const md::Document missing = md::parse(QStringLiteral("A[^missing] b[^also-missing:1] c[^word.] d[^A short note.]\n"));
    QCOMPARE(count(missing, DiagnosticCode::MissingFootnote), 2);
    QCOMPARE(all(missing, NodeType::InlineNote).size(), size_t(2));
    const std::vector<const Node *> references = all(missing, NodeType::FootnoteReference);
    QCOMPARE(references.size(), size_t(2));
    QVERIFY(!references[0]->resolved && references[0]->number == 0);
    QCOMPARE(references[0]->label, QStringLiteral("missing"));
    QCOMPARE(missing.source(0, missing.diagnostics.front().range).toString(), QStringLiteral("[^missing]"));
    const QString missingHtml = md::toHtml(missing);
    QVERIFY2(missingHtml.startsWith(QStringLiteral("<p>A[^missing] b[^also-missing:1] c%1 d%2</p>").arg(ref(1), ref(2))),
             qPrintable(missingHtml));

    // A matching definition always makes a reference, whatever the label.
    const md::Document spaced = md::parse(QStringLiteral("A[^my note] b[^Word.]\n\n[^my note]: Defined.\n[^word.]: Also defined.\n"));
    QCOMPARE(all(spaced, NodeType::FootnoteReference).size(), size_t(2));
    QCOMPARE(all(spaced, NodeType::InlineNote).size(), size_t(0));
    QVERIFY2(spaced.diagnostics.empty(), qPrintable(describe(spaced)));

    // Nothing is a note in code or in a comment; an escaped bracket is text.
    const Case literal[] = {
        {"`[^1]` and `[^An inline note.]`\n\n[^1]: N.", "<p><code>[^1]</code> and <code>[^An inline note.]</code></p>\n"},
        {"```\n[^1]\n```\n\n[^1]: N.", "<pre><code>[^1]\n</code></pre>\n"},
        {"\\[^1] and \\[^Not a note.]\n\n[^1]: N.", "<p>[^1] and [^Not a note.]</p>\n"},
        {"<!-- [^1] -->\n\n// [^1] and [^A note.]\n\n[^1]: N.", ""},
        {"Empty[^] and blank[^ ]", "<p>Empty[^] and blank[^ ]</p>\n"},
    };
    CHECK_CASES(literal, blockWriter);

    // An exclamation mark before a note is not an image.
    QVERIFY(html(QStringLiteral("Wow![^1]\n\n[^1]: N.\n")).startsWith(QStringLiteral("<p>Wow!%1</p>").arg(ref(1))));

    // A note used only inside another note is numbered after it.
    const md::Document nested = md::parse(QStringLiteral("Text[^a] more[^c].\n\n[^a]: See also[^b].\n[^b]: Inner.\n[^c]: Last.\n"));
    QCOMPARE(nested.footnotes.size(), size_t(3));
    QCOMPARE(nested.footnotes[0].label, QStringLiteral("a"));
    QCOMPARE(nested.footnotes[1].label, QStringLiteral("c"));
    QCOMPARE(nested.footnotes[2].label, QStringLiteral("b"));
    QCOMPARE(nested.footnotes[2].number, 3);

    // The list of notes points at their content; unused and duplicate
    // definitions are reported.
    const md::Document doc = md::parse(QStringLiteral("A[^1] b[^1] c[^Inline.]\n\n[^1]: One.\n[^1]: Again.\n[^unused]: Never.\n"));
    QCOMPARE(doc.footnotes.size(), size_t(2));
    QCOMPARE(doc.footnotes[0].number, 1);
    QCOMPARE(doc.footnotes[0].label, QStringLiteral("1"));
    QCOMPARE(doc.footnotes[0].references, 2);
    QCOMPARE(doc.footnotes[0].content->type, NodeType::FootnoteDefinition);
    QCOMPARE(md::plainText(*doc.footnotes[0].content), QStringLiteral("One."));
    QCOMPARE(doc.footnotes[1].content->type, NodeType::InlineNote);
    QCOMPARE(doc.source(0, doc.footnotes[1].range).toString(), QStringLiteral("[^Inline.]"));
    QCOMPARE(count(doc, DiagnosticCode::DuplicateFootnote), 1);
    QCOMPARE(count(doc, DiagnosticCode::UnusedFootnote), 1);
    const Node *definition = first(doc, NodeType::FootnoteDefinition);
    QCOMPARE(definition->label, QStringLiteral("1"));
    QCOMPARE(definition->number, 1);
    QCOMPARE(doc.source(0, definition->open).toString(), QStringLiteral("[^1]:"));

    // Standard has no footnotes; with the extension on it has references only.
    QCOMPARE(html(QStringLiteral("A[^1].\n\n[^1]: The note."), standard()),
             QStringLiteral("<p>A[^1].</p>\n<p>[^1]: The note.</p>\n"));
    md::Options withNotes = standard();
    withNotes.footnotes = true;
    QVERIFY(html(QStringLiteral("A[^1].\n\n[^1]: N."), withNotes).startsWith(QStringLiteral("<p>A%1.</p>").arg(ref(1))));
    QCOMPARE(html(QStringLiteral("A[^An inline note.] b[^missing]"), withNotes),
             QStringLiteral("<p>A[^An inline note.] b[^missing]</p>\n"));
}

void citations()
{
    // The Writer examples of the dialect.
    const QString text = QStringLiteral(
        "A statement[p. 23][#Doe:2006].\n"
        "Another statement[][#Doe:2006].\n\n"
        "[#Doe:2006]: John Doe. *A Sample Book*. Example Press, 2006.\n");
    QCOMPARE(html(text),
             QStringLiteral("<p>A statement<span class=\"citation\">[<a href=\"#cite-1\">1</a>, p. 23]</span>.\n"
                            "Another statement<span class=\"citation\">[<a href=\"#cite-1\">1</a>]</span>.</p>\n"
                            "<section class=\"bibliography\">\n<ol>\n"
                            "<li id=\"cite-1\">John Doe. <em>A Sample Book</em>. Example Press, 2006.</li>\n"
                            "</ol>\n</section>\n"));
    const md::Document doc = md::parse(text);
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));
    const std::vector<const Node *> cites = all(doc, NodeType::Citation);
    QCOMPARE(cites.size(), size_t(2));
    // The key and the locator are kept as written.
    QCOMPARE(cites[0]->label, QStringLiteral("Doe:2006"));
    QCOMPARE(cites[0]->text, QStringLiteral("p. 23"));
    QCOMPARE(cites[0]->number, 1);
    QCOMPARE(doc.source(*cites[0]).toString(), QStringLiteral("[p. 23][#Doe:2006]"));
    QCOMPARE(doc.source(0, cites[0]->target).toString(), QStringLiteral("Doe:2006"));
    QCOMPARE(cites[1]->text, QString());
    QCOMPARE(doc.bibliography.size(), size_t(1));
    QCOMPARE(doc.bibliography[0].key, QStringLiteral("Doe:2006"));
    QCOMPARE(doc.bibliography[0].number, 1);
    QVERIFY(doc.bibliography[0].definition);
    QCOMPARE(doc.bibliography[0].definition->type, NodeType::CitationDefinition);
    QCOMPARE(md::plainText(*doc.bibliography[0].definition), QStringLiteral("John Doe. A Sample Book. Example Press, 2006."));

    // Numbers follow first use; the Pandoc form feeds the same citations.
    const QString mixed = QStringLiteral(
        "Second defined, first cited [@smith04, chap. 1]. Then [see][#Doe:2006] and [@Doe:2006, p. 5] and [@smith04].\n\n"
        "[#Doe:2006]: Doe.\n[#smith04]: Smith.\n");
    const md::Document both = md::parse(mixed);
    QVERIFY2(both.diagnostics.empty(), qPrintable(describe(both)));
    QCOMPARE(both.bibliography.size(), size_t(2));
    QCOMPARE(both.bibliography[0].key, QStringLiteral("smith04"));
    QCOMPARE(both.bibliography[1].key, QStringLiteral("Doe:2006"));
    QVERIFY(md::toHtml(both).startsWith(QStringLiteral(
        "<p>Second defined, first cited <span class=\"citation\">[<a href=\"#cite-1\">1</a>, chap. 1]</span>. "
        "Then <span class=\"citation\">[<a href=\"#cite-2\">2</a>, see]</span> and "
        "<span class=\"citation\">[<a href=\"#cite-2\">2</a>, p. 5]</span> and "
        "<span class=\"citation\">[<a href=\"#cite-1\">1</a>]</span>.</p>\n")));

    // Several works in one Pandoc citation.
    const md::Document group = md::parse(QStringLiteral("Both [@a, p. 1; @b].\n\n[#a]: A.\n[#b]: B.\n"));
    QVERIFY(md::toHtml(group).startsWith(QStringLiteral(
        "<p>Both <span class=\"citation\">[<a href=\"#cite-1\">1</a>, p. 1; <a href=\"#cite-2\">2</a>]</span>.</p>")));
    QCOMPARE(group.source(*first(group, NodeType::Citation)).toString(), QStringLiteral("[@a, p. 1; @b]"));

    // A key may be any label text.
    const md::Document odd = md::parse(QStringLiteral("See [][#A key, with: punctuation!].\n\n[#a KEY, with: punctuation!]: Entry.\n"));
    QVERIFY2(odd.diagnostics.empty(), qPrintable(describe(odd)));
    QCOMPARE(first(odd, NodeType::Citation)->label, QStringLiteral("A key, with: punctuation!"));

    // A missing key stays visible and is reported.
    const md::Document missing = md::parse(QStringLiteral("See [p. 1][#Nobody] and [@nobody, p. 2].\n"));
    QCOMPARE(count(missing, DiagnosticCode::MissingCitation), 2);
    QCOMPARE(md::toHtml(missing), QStringLiteral("<p>See [p. 1][#Nobody] and [@nobody, p. 2].</p>\n"));
    QVERIFY(!first(missing, NodeType::Citation)->resolved);
    QVERIFY(missing.bibliography.empty());

    // A key of an external bibliography needs no definition in the text; a
    // definition in the text comes first.
    md::Source source;
    source.externalCitationKeys = {QStringLiteral("knuth84"), QStringLiteral("local")};
    const md::Document external = md::parse(
        QStringLiteral("See [@knuth84, p. 3] and [][#local] and [@unknown].\n\n[#local]: Defined here.\n"), writer(), source);
    QCOMPARE(count(external, DiagnosticCode::MissingCitation), 1);
    QCOMPARE(external.bibliography.size(), size_t(2));
    QVERIFY(external.bibliography[0].external);
    QCOMPARE(external.bibliography[0].key, QStringLiteral("knuth84"));
    QVERIFY(!external.bibliography[0].definition);
    QVERIFY(!external.bibliography[1].external);
    QVERIFY(external.bibliography[1].definition);

    const Case literal[] = {
        // Not citations: code, a link whose text starts with @, an escaped bracket.
        {"`[p. 1][#Doe]` and `[@doe]`\n\n[#Doe]: D.", "<p><code>[p. 1][#Doe]</code> and <code>[@doe]</code></p>\n"},
        {"[@handle](https://example.org/handle)", "<p><a href=\"https://example.org/handle\">@handle</a></p>\n"},
        {"\\[@doe] and [not@doe]\n\n[#doe]: D.", "<p>[@doe] and [not@doe]</p>\n"},
        {"[@]", "<p>[@]</p>\n"},
    };
    CHECK_CASES(literal, blockWriter);
    // Standard reads `[#key]: text` as an ordinary link definition.
    QCOMPARE(html(QStringLiteral("[x][#k]\n\n[#k]: /url"), standard()), QStringLiteral("<p><a href=\"/url\">x</a></p>\n"));
    QCOMPARE(html(QStringLiteral("[@doe]"), standard()), QStringLiteral("<p>[@doe]</p>\n"));
}

void citationPrefixes()
{
    const QString works = QStringLiteral("\n\n[#doe99]: Doe.\n[#smith04]: Smith.\n");
    // Words before a key are its prefix, item by item, and the locator
    // after it stays where it was; each item's node starts at its `@`.
    const md::Document group =
        md::parse(QStringLiteral("Blah [see @doe99, pp. 33-35; also @smith04, chap. 1].") + works);
    QVERIFY2(group.diagnostics.empty(), qPrintable(describe(group)));
    const std::vector<const Node *> cites = all(group, NodeType::Citation);
    QCOMPARE(cites.size(), size_t(3));
    QCOMPARE(group.source(*cites[0]).toString(), QStringLiteral("[see @doe99, pp. 33-35; also @smith04, chap. 1]"));
    QCOMPARE(cites[1]->label, QStringLiteral("doe99"));
    QCOMPARE(cites[1]->title, QStringLiteral("see"));
    QCOMPARE(cites[1]->text, QStringLiteral("pp. 33-35"));
    QCOMPARE(group.source(*cites[1]).toString(), QStringLiteral("@doe99, pp. 33-35"));
    QCOMPARE(group.source(0, cites[1]->open).toString(), QStringLiteral("@"));
    QCOMPARE(group.source(0, cites[1]->target).toString(), QStringLiteral("doe99"));
    QCOMPARE(cites[2]->label, QStringLiteral("smith04"));
    QCOMPARE(cites[2]->title, QStringLiteral("also"));
    QCOMPARE(cites[2]->text, QStringLiteral("chap. 1"));
    QVERIFY(md::toHtml(group).startsWith(QStringLiteral(
        "<p>Blah <span class=\"citation\">[see <a href=\"#cite-1\">1</a>, pp. 33-35; "
        "also <a href=\"#cite-2\">2</a>, chap. 1]</span>.</p>")));

    // One work, with a prefix, a suppressed author or both.
    const md::Document one = md::parse(QStringLiteral("A [see @doe99] and [cf. -@smith04, p. 3].") + works);
    QVERIFY2(one.diagnostics.empty(), qPrintable(describe(one)));
    const std::vector<const Node *> single = all(one, NodeType::Citation);
    QCOMPARE(single.size(), size_t(2));
    QCOMPARE(single[0]->title, QStringLiteral("see"));
    QCOMPARE(single[0]->marker, QChar());
    QCOMPARE(one.source(0, single[0]->open).toString(), QStringLiteral("["));
    QCOMPARE(one.source(0, single[0]->close).toString(), QStringLiteral("]"));
    QCOMPARE(one.source(0, single[0]->target).toString(), QStringLiteral("doe99"));
    QCOMPARE(single[1]->title, QStringLiteral("cf."));
    QCOMPARE(single[1]->marker, QChar(u'-'));
    QCOMPARE(single[1]->text, QStringLiteral("p. 3"));
    QVERIFY(md::toHtml(one).startsWith(QStringLiteral(
        "<p>A <span class=\"citation\">[see <a href=\"#cite-1\">1</a>]</span> and "
        "<span class=\"citation\">[cf. <a href=\"#cite-2\">2</a>, p. 3]</span>.</p>")));

    // A key nothing defines is still a citation inside brackets, and reported.
    const md::Document missing = md::parse(QStringLiteral("See [see @nobody, p. 2]."));
    QCOMPARE(count(missing, DiagnosticCode::MissingCitation), 1);
    QVERIFY(!first(missing, NodeType::Citation)->resolved);
    QCOMPARE(md::toHtml(missing), QStringLiteral("<p>See [see @nobody, p. 2].</p>\n"));

    // In the text, `@key` names the author: only a key that is defined, so
    // that a handle or a name after an @ stays text. Punctuation after the
    // key ends it, and a locator in brackets may follow.
    const md::Document inText = md::parse(QStringLiteral(
        "@doe99 says so, and @smith04 [p. 33] agrees. So does @doe99.\n\nAsk @nobody or @doe99x.") + works);
    QVERIFY2(inText.diagnostics.empty(), qPrintable(describe(inText)));
    const std::vector<const Node *> named = all(inText, NodeType::Citation);
    QCOMPARE(named.size(), size_t(3));
    QCOMPARE(inText.source(*named[0]).toString(), QStringLiteral("@doe99"));
    QCOMPARE(named[0]->marker, QChar(u'@'));
    QCOMPARE(named[0]->label, QStringLiteral("doe99"));
    QCOMPARE(named[0]->text, QString());
    QCOMPARE(inText.source(0, named[0]->open).toString(), QStringLiteral("@"));
    QCOMPARE(inText.source(0, named[0]->target).toString(), QStringLiteral("doe99"));
    QCOMPARE(inText.source(*named[1]).toString(), QStringLiteral("@smith04 [p. 33]"));
    QCOMPARE(named[1]->text, QStringLiteral("p. 33"));
    QCOMPARE(inText.source(0, named[1]->close).toString(), QStringLiteral("]"));
    QCOMPARE(inText.source(*named[2]).toString(), QStringLiteral("@doe99"));
    QVERIFY(md::toHtml(inText).startsWith(QStringLiteral(
        "<p><span class=\"citation\">[<a href=\"#cite-1\">1</a>]</span> says so, and "
        "<span class=\"citation\">[<a href=\"#cite-2\">2</a>, p. 33]</span> agrees. So does "
        "<span class=\"citation\">[<a href=\"#cite-1\">1</a>]</span>.</p>\n<p>Ask @nobody or @doe99x.</p>")));
    // An external bibliography's keys count as defined.
    md::Source source;
    source.externalCitationKeys = {QStringLiteral("knuth84")};
    const md::Document external = md::parse(QStringLiteral("As @knuth84 shows."), writer(), source);
    QCOMPARE(all(external, NodeType::Citation).size(), size_t(1));
    QCOMPARE(first(external, NodeType::Citation)->marker, QChar(u'@'));

    // E-mail addresses, links and code stay as they were, defined keys or not.
    const Case unchanged[] = {
        {"Write to jane@doe99.org.\n\n[#doe99]: D.",
         "<p>Write to <a href=\"mailto:jane@doe99.org\">jane@doe99.org</a>.</p>\n"},
        {"[mail jane@doe99.org]\n\n[#doe99]: D.",
         "<p>[mail <a href=\"mailto:jane@doe99.org\">jane@doe99.org</a>]</p>\n"},
        {"[see @doe99](https://example.org)\n\n[#doe99]: D.",
         "<p><a href=\"https://example.org\">see @doe99</a></p>\n"},
        {"[@doe99 said](https://example.org)\n\n[#doe99]: D.",
         "<p><a href=\"https://example.org\">@doe99 said</a></p>\n"},
        {"[see @doe99][ref]\n\n[ref]: https://example.org\n[#doe99]: D.",
         "<p><a href=\"https://example.org\">see @doe99</a></p>\n"},
        {"[x-@doe99] and [see\\@doe99] and \\@doe99\n\n[#doe99]: D.",
         "<p>[x-@doe99] and [see@doe99] and @doe99</p>\n"},
        {"[see `@doe99`] and `@doe99` and [see ` @doe99 `]\n\n[#doe99]: D.",
         "<p>[see <code>@doe99</code>] and <code>@doe99</code> and [see <code>@doe99</code>]</p>\n"},
        {"<https://example.org/@doe99> and https://example.org/@doe99\n\n[#doe99]: D.",
         "<p><a href=\"https://example.org/@doe99\">https://example.org/@doe99</a> and "
         "<a href=\"https://example.org/@doe99\">https://example.org/@doe99</a></p>\n"},
        // Every item of a group needs a key; and inside brackets that are
        // no citation, `@key` stays text.
        {"[a; b @doe99]\n\n[#doe99]: D.", "<p>[a; b @doe99]</p>\n"},
    };
    CHECK_CASES(unchanged, blockWriter);
    // Standard Markdown has no citations at all.
    QCOMPARE(html(QStringLiteral("[see @doe] and @doe"), standard()), QStringLiteral("<p>[see @doe] and @doe</p>\n"));
}

void comments()
{
    const Case cases[] = {
        // HTML comments stay in the source and are left out of what is read.
        {"Before <!-- private note --> after", "<p>Before  after</p>\n"},
        {"<!-- private note -->", ""},
        {"<!--\nseveral\nlines\n-->\n\nText", "<p>Text</p>\n"},
        // A line that starts at column zero with // is a comment.
        {"// a comment", ""},
        {"//no space either", ""},
        {"Text\n\n// a comment\n\nMore", "<p>Text</p>\n<p>More</p>\n"},
        // It does not interrupt what it is written in.
        {"Line one\n// hidden\nline two", "<p>Line one\nline two</p>\n"},
        {"Line one\n// hidden", "<p>Line one</p>\n"},
        {"- one\n// hidden\n- two", "<ul>\n<li>one</li>\n<li>two</li>\n</ul>\n"},
        {"| a |\n| - |\n// hidden\n| b |", "<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>b</td>\n</tr>\n</tbody>\n</table>\n"},
        {"| a |\n// hidden\n| - |\n| b |", "<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>b</td>\n</tr>\n</tbody>\n</table>\n"},
        {"Term\n// hidden\n: Definition", "<dl>\n<dt>Term</dt>\n<dd>Definition</dd>\n</dl>\n"},
        {"Title\n// hidden\n=====", "<h1>Title</h1>\n"},
        {"- one\n\n  two\n// hidden\n\n- three", "<ul>\n<li>\n<p>one</p>\n<p>two</p>\n</li>\n<li>\n<p>three</p>\n</li>\n</ul>\n"},
        // The // of a URL is not a comment, and neither is // further along a line.
        {"https://example.org", "<p><a href=\"https://example.org\">https://example.org</a></p>\n"},
        {"See https://example.org // not a comment", "<p>See <a href=\"https://example.org\">https://example.org</a> // not a comment</p>\n"},
        {" // one space in", "<p>// one space in</p>\n"},
        {"> // in a quote", "<blockquote>\n<p>// in a quote</p>\n</blockquote>\n"},
        // Code keeps its comment markers.
        {"    // indented code", "<pre><code>// indented code\n</code></pre>\n"},
        {"```\n// fenced code\n```", "<pre><code>// fenced code\n</code></pre>\n"},
        {"~~~js\nlet a = 1;\n// fenced\n~~~", "<pre><code class=\"language-js\">let a = 1;\n// fenced\n</code></pre>\n"},
        {"`// in a code span`", "<p><code>// in a code span</code></p>\n"},
        {"A `code\n// span` over lines", "<p>A <code>code // span</code> over lines</p>\n"},
        // Nothing in a comment is expanded.
        {"// **bold** [%var] $x$ [^note text]", ""},
    };
    CHECK_CASES(cases, blockWriter);

    // Standard has HTML comments only, which pass through its plain rendering.
    QCOMPARE(html(QStringLiteral("// a comment"), standard()), QStringLiteral("<p>// a comment</p>\n"));
    QCOMPARE(html(QStringLiteral("Before <!-- note --> after"), standard()), QStringLiteral("<p>Before  after</p>\n"));

    // Comments are nodes, so that they can be styled and skipped.
    const QString text = QStringLiteral("// first\nLine one\n// hidden\nline two <!-- inline -->\n\n<!-- block -->\n");
    const md::Document doc = md::parse(text);
    const std::vector<const Node *> lines = all(doc, NodeType::CommentLine);
    QCOMPARE(lines.size(), size_t(2));
    QCOMPARE(doc.source(*lines[0]).toString(), QStringLiteral("// first"));
    QCOMPARE(lines[0]->text, QStringLiteral("first"));
    QCOMPARE(doc.source(*lines[1]).toString(), QStringLiteral("// hidden"));
    const Node *inlineComment = first(doc, NodeType::HtmlComment);
    QVERIFY(inlineComment);
    QCOMPARE(doc.source(*inlineComment).toString(), QStringLiteral("<!-- inline -->"));
    const Node *block = first(doc, NodeType::HtmlBlock);
    QVERIFY(block);
    QCOMPARE(block->text, QStringLiteral("<!-- block -->"));
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));
    QCOMPARE(doc.plainText(), QStringLiteral("Line one line two"));
}

void rawHtmlPolicy()
{
    const Case cases[] = {
        // The semantic subset is kept.
        {"line<br>break", "<p>line<br />break</p>\n"},
        {"line<br/>break", "<p>line<br />break</p>\n"},
        {"<em>a</em> <strong>b</strong> <i>c</i> <b>d</b>", "<p><em>a</em> <strong>b</strong> <i>c</i> <b>d</b></p>\n"},
        {"<code>x</code> <kbd>Ctrl</kbd>", "<p><code>x</code> <kbd>Ctrl</kbd></p>\n"},
        {"x<sup>2</sup> H<sub>2</sub>O", "<p>x<sup>2</sup> H<sub>2</sub>O</p>\n"},
        {"<del>a</del> <ins>b</ins> <mark>c</mark> <u>d</u> <s>e</s>",
         "<p><del>a</del> <ins>b</ins> <mark>c</mark> <u>d</u> <s>e</s></p>\n"},
        {"<a href=\"https://example.org\" title=\"T\">link</a>", "<p><a href=\"https://example.org\" title=\"T\">link</a></p>\n"},
        {"<img src=\"a.png\" alt=\"A\" width=\"10\">", "<p><img src=\"a.png\" alt=\"A\" width=\"10\" /></p>\n"},
        // Markdown inside inline markup is still Markdown.
        {"<sup>**b** and `c`</sup>", "<p><sup><strong>b</strong> and <code>c</code></sup></p>\n"},
        // Blocks: lists and tables.
        {"<ul>\n<li>one</li>\n<li>two\n</ul>", "<ul>\n<li>one</li>\n<li>two\n</li></ul>\n"},
        {"<table>\n<tr><th>a</th><td colspan=\"2\" align=\"right\">b</td></tr>\n</table>",
         "<table>\n<tr><th>a</th><td colspan=\"2\" align=\"right\">b</td></tr>\n</table>\n"},
        {"<ol start=\"3\"><li>three</li></ol>", "<ol start=\"3\"><li>three</li></ol>\n"},
        // Everything else is shown as the text it is, and its content is kept.
        {"<font color=\"red\">red</font>", "<p>&lt;font color=&quot;red&quot;&gt;red&lt;/font&gt;</p>\n"},
        {"a <span class=\"x\">b</span> c", "<p>a &lt;span class=&quot;x&quot;&gt;b&lt;/span&gt; c</p>\n"},
        {"<div align=\"center\">\ncentred\n</div>", "<p>&lt;div align=&quot;center&quot;&gt;\ncentred\n&lt;/div&gt;</p>\n"},
        {"<center>x</center>", "<p>&lt;center&gt;x&lt;/center&gt;</p>\n"},
        // Scripts, styles and frames never reach the output as markup.
        {"<script>alert(1)</script>", "<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>\n"},
        {"a <script>alert(1)</script> b", "<p>a &lt;script&gt;alert(1)&lt;/script&gt; b</p>\n"},
        {"<style>p { color: red }</style>", "<p>&lt;style&gt;p { color: red }&lt;/style&gt;</p>\n"},
        {"<iframe src=\"https://example.org\"></iframe>", "<p>&lt;iframe src=&quot;https://example.org&quot;&gt;&lt;/iframe&gt;</p>\n"},
        // Event and style attributes are dropped from elements that are kept.
        {"<b onclick=\"alert(1)\" style=\"color:red\">x</b>", "<p><b>x</b></p>\n"},
        {"<img src=\"a.png\" onerror=\"alert(1)\">", "<p><img src=\"a.png\" /></p>\n"},
        {"<a href=\"x\" onmouseover=\"alert(1)\">x</a>", "<p><a href=\"x\">x</a></p>\n"},
        // Script URLs are dropped, in markup and in Markdown.
        {"<a href=\"javascript:alert(1)\">x</a>", "<p><a>x</a></p>\n"},
        {"<a href=\" JaVa\tScript:alert(1)\">x</a>", "<p><a>x</a></p>\n"},
        {"[x](javascript:alert(1))", "<p><a href=\"\">x</a></p>\n"},
        {"[x](vbscript:msgbox)", "<p><a href=\"\">x</a></p>\n"},
        {"![x](javascript:alert(1))", "<p><img src=\"\" alt=\"x\" /></p>\n"},
        {"[x](data:text/html;base64,PHNjcmlwdD4=)", "<p><a href=\"\">x</a></p>\n"},
        {"<javascript:alert(1)>", "<p><a href=\"\">javascript:alert(1)</a></p>\n"},
        // A tag without its partner is text.
        {"an <em>unclosed tag", "<p>an &lt;em&gt;unclosed tag</p>\n"},
        {"a stray </em> tag", "<p>a stray &lt;/em&gt; tag</p>\n"},
        {"*a <sup>b* c</sup>", "<p><em>a &lt;sup&gt;b</em> c&lt;/sup&gt;</p>\n"},
        // Text is always escaped.
        {"1 < 2 & 3 > 2 \"quoted\"", "<p>1 &lt; 2 &amp; 3 &gt; 2 &quot;quoted&quot;</p>\n"},
    };
    CHECK_CASES(cases, blockWriter);
    CHECK_CASES(cases, blockStandard);

    // Whatever the input, the safe rendering has no script, frame or handler.
    const QString hostile = QStringLiteral(
        "<script>alert(1)</script>\n\n<SCRIPT src=x></SCRIPT>\n\n<img src=x onerror=alert(1)>\n\n"
        "<iframe src=javascript:alert(1)>\n\n<a href='javascript:alert(1)' onclick='x()'>a</a>\n\n"
        "<svg onload=alert(1)>\n\n<object data=x></object> <embed src=x>\n\n<p onclick=\"x()\">p</p>\n\n"
        "<table onmouseover=x()><tr><td onclick=x()>c</td></tr></table>\n\n"
        "[a](javascript:alert(1)) ![b](vbscript:x) <javascript:alert(1)>\n\n"
        "<a href=\"&#106;avascript:alert(1)\">entity</a>\n");
    static const QRegularExpression tag(QStringLiteral("<[^>]*>"));
    static const QRegularExpression handler(QStringLiteral("\\son[a-z]+\\s*="), QRegularExpression::CaseInsensitiveOption);
    for (const md::Options &options : {writer(), standard()}) {
        const QString safe = html(hostile, options);
        // Text is always escaped, so every tag found here is real markup.
        auto tags = tag.globalMatch(safe);
        int found = 0;
        while (tags.hasNext()) {
            const QString markup = tags.next().captured(0);
            ++found;
            for (const char *forbidden : {"<script", "<iframe", "<svg", "<object", "<embed", "<style", "javascript:",
                                          "vbscript:", "style="})
                QVERIFY2(!markup.contains(QLatin1StringView(forbidden), Qt::CaseInsensitive), qPrintable(markup));
            QVERIFY2(!handler.match(markup).hasMatch(), qPrintable(markup));
        }
        QVERIFY(found > 10);
        // The text is still there to be read.
        QVERIFY(safe.contains(QLatin1StringView("&lt;script&gt;alert(1)&lt;/script&gt;")));
        QVERIFY(safe.contains(QLatin1StringView("&lt;svg onload=alert(1)&gt;")));
        QVERIFY(safe.contains(QLatin1StringView("<td>c</td>")));
        QVERIFY(safe.contains(QLatin1StringView("<a>entity</a>")));
    }

    // Passthrough, for checking the CommonMark examples, changes nothing.
    QCOMPARE(md::toHtml(md::parse(QStringLiteral("a <span class=\"x\">b</span> <!-- c -->\n\n<div>\nd\n</div>\n"), standard()),
                        md::HtmlOptions::commonMark()),
             QStringLiteral("<p>a <span class=\"x\">b</span> <!-- c --></p>\n<div>\nd\n</div>\n"));

    // The tree says which markup is understood.
    const QString text = QStringLiteral("x<sup>2</sup> <font>y</font> <img src=\"a.png\" alt=\"A\" onerror=\"x\">\n");
    const md::Document doc = md::parse(text);
    const std::vector<const Node *> elements = all(doc, NodeType::HtmlElement);
    QCOMPARE(elements.size(), size_t(2));
    QCOMPARE(elements[0]->label, QStringLiteral("sup"));
    QCOMPARE(doc.source(*elements[0]).toString(), QStringLiteral("<sup>2</sup>"));
    QCOMPARE(doc.source(0, elements[0]->open).toString(), QStringLiteral("<sup>"));
    QCOMPARE(doc.source(0, elements[0]->close).toString(), QStringLiteral("</sup>"));
    QCOMPARE(md::plainText(*elements[0]), QStringLiteral("2"));
    QCOMPARE(elements[1]->label, QStringLiteral("img"));
    const int src = int(text.indexOf(QLatin1StringView("a.png")));
    const int alt = int(text.indexOf(QLatin1StringView("A")));
    QCOMPARE(elements[1]->attributes,
             (std::vector<md::Attribute>{{QStringLiteral("src"), QStringLiteral("a.png"), {src, src + 5}, u'"'},
                                         {QStringLiteral("alt"), QStringLiteral("A"), {alt, alt + 1}, u'"'}}));
    QCOMPARE(all(doc, NodeType::HtmlInline).size(), size_t(2));
    // Reported: the two tags that are shown as text, and the attribute that was dropped.
    QCOMPARE(count(doc, DiagnosticCode::UnsupportedHtml), 3);
    QCOMPARE(doc.source(0, doc.diagnostics[0].range).toString(), QStringLiteral("<font>"));
    QCOMPARE(doc.source(0, doc.diagnostics[2].range).toString(), QStringLiteral("<img src=\"a.png\" alt=\"A\" onerror=\"x\">"));
    QCOMPARE(doc.diagnostics[0].severity, md::Severity::Info);
    const md::Document dropped = md::parse(QStringLiteral("<table style=\"x\"><tr><td>c</td></tr></table>\n"));
    QCOMPARE(count(dropped, DiagnosticCode::UnsupportedHtml), 1);

    // A block of supported markup is read into elements with their own ranges.
    const QString blockText = QStringLiteral("<ul>\n<li>one &amp; two</li>\n</ul>\n");
    const md::Document blockDoc = md::parse(blockText);
    const Node *block = first(blockDoc, NodeType::HtmlBlock);
    QVERIFY(block);
    QCOMPARE(block->text, QStringLiteral("<ul>\n<li>one &amp; two</li>\n</ul>"));
    const Node *item = nullptr;
    for (const Node *element : all(blockDoc, NodeType::HtmlElement))
        if (element->label == QLatin1StringView("li")) item = element;
    QVERIFY(item);
    QCOMPARE(blockDoc.source(*item).toString(), QStringLiteral("<li>one &amp; two</li>"));
    QCOMPARE(md::plainText(*item), QStringLiteral("one & two"));
    QVERIFY2(blockDoc.diagnostics.empty(), qPrintable(describe(blockDoc)));
}

void math()
{
    const Case cases[] = {
        {"Inline $e^{i\\pi}+1=0$ here", "<p>Inline <span class=\"math\">e^{i\\pi}+1=0</span> here</p>\n"},
        {"$$x_{1,2}=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}$$",
         "<div class=\"math display\">x_{1,2}=\\frac{-b\\pm\\sqrt{b^2-4ac}}{2a}</div>\n"},
        // A display block may stand over several lines.
        {"$$\na = b\n  + c\n$$", "<div class=\"math display\">a = b\n  + c</div>\n"},
        {"Before\n\n$$\n\\int_0^1 x\\,dx\n$$\n\nAfter",
         "<p>Before</p>\n<div class=\"math display\">\\int_0^1 x\\,dx</div>\n<p>After</p>\n"},
        {"$$ a < b $$", "<div class=\"math display\">a &lt; b</div>\n"},
        // Display math inside a paragraph.
        {"Text $$x^2$$ more", "<p>Text <span class=\"math display\">x^2</span> more</p>\n"},
        // Currency stays prose.
        {"$5 and $10", "<p>$5 and $10</p>\n"},
        {"It costs $5, or $10 for two.", "<p>It costs $5, or $10 for two.</p>\n"},
        // Ambiguous: a price and a dollar sign after a number do make math;
        // the next dollar sign after an opening one is the only candidate.
        {"Prices: $5, $10 and 15$.", "<p>Prices: $5, <span class=\"math\">10 and 15</span>.</p>\n"},
        {"Prices: \\$5, \\$10 and 15$.", "<p>Prices: $5, $10 and 15$.</p>\n"},
        {"I have $5 and you have $10, in all $15$.", "<p>I have $5 and you have $10, in all <span class=\"math\">15</span>.</p>\n"},
        {"$x and y$ are $z$", "<p><span class=\"math\">x and y</span> are <span class=\"math\">z</span></p>\n"},
        {"From $5-$10 or US$5 and US$10.", "<p>From $5-$10 or US$5 and US$10.</p>\n"},
        {"A single $ sign", "<p>A single $ sign</p>\n"},
        // The content may not start or end with a space, and the dollars
        // need space or punctuation outside them.
        {"$ x$ and $x $", "<p>$ x$ and $x $</p>\n"},
        {"a$x$b", "<p>a$x$b</p>\n"},
        {"($x$), $y$.", "<p>(<span class=\"math\">x</span>), <span class=\"math\">y</span>.</p>\n"},
        {"$x$2", "<p>$x$2</p>\n"},
        // Escaped dollars are literal.
        {"\\$x\\$ and \\$5", "<p>$x$ and $5</p>\n"},
        {"$a \\$ b$", "<p><span class=\"math\">a \\$ b</span></p>\n"},
        // Markdown is not read inside math, and math is not read inside
        // code, URLs or comments.
        {"$a_1 * b_2 * c$", "<p><span class=\"math\">a_1 * b_2 * c</span></p>\n"},
        {"`$x$` and ``$$y$$``", "<p><code>$x$</code> and <code>$$y$$</code></p>\n"},
        {"```\n$$\nx\n$$\n```", "<pre><code>$$\nx\n$$\n</code></pre>\n"},
        {"[a](https://example.org/$x$)", "<p><a href=\"https://example.org/$x$\">a</a></p>\n"},
        {"<https://example.org/$x$>", "<p><a href=\"https://example.org/$x$\">https://example.org/$x$</a></p>\n"},
        {"https://example.org/$x$ and $y$",
         "<p><a href=\"https://example.org/$x$\">https://example.org/$x$</a> and <span class=\"math\">y</span></p>\n"},
        {"<!-- $x$ -->\n\n// $x$ and $$y$$", ""},
        // Unclosed math stays readable source.
        {"An unclosed $x here", "<p>An unclosed $x here</p>\n"},
        {"$$\nnever closed\n\nNext", "<p>$$\nnever closed</p>\n<p>Next</p>\n"},
    };
    CHECK_CASES(cases, blockWriter);
    QCOMPARE(html(QStringLiteral("$x$ and\n\n$$\ny\n$$"), standard()), QStringLiteral("<p>$x$ and</p>\n<p>$$\ny\n$$</p>\n"));

    const md::Document unclosed = md::parse(QStringLiteral("$$\nnever closed\n\nNext\n"));
    QCOMPARE(count(unclosed, DiagnosticCode::UnclosedMath), 1);
    QCOMPARE(unclosed.source(0, unclosed.diagnostics.front().range).toString(), QStringLiteral("$$"));

    const QString text = QStringLiteral("Inline $a+b$ and\n\n$$\nc = d\n$$\n");
    const md::Document doc = md::parse(text);
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));
    const Node *inlineMath = first(doc, NodeType::MathInline);
    QVERIFY(inlineMath && !inlineMath->display);
    QCOMPARE(inlineMath->text, QStringLiteral("a+b"));
    QCOMPARE(doc.source(*inlineMath).toString(), QStringLiteral("$a+b$"));
    QCOMPARE(doc.source(0, inlineMath->open).toString(), QStringLiteral("$"));
    const Node *block = first(doc, NodeType::MathBlock);
    QVERIFY(block);
    QCOMPARE(block->text, QStringLiteral("c = d"));
    QCOMPARE(doc.source(*block).toString(), QStringLiteral("$$\nc = d\n$$"));
    QCOMPARE(doc.source(0, block->open).toString(), QStringLiteral("$$"));
    QCOMPARE(doc.source(0, block->close).toString(), QStringLiteral("$$"));
}

void mermaid()
{
    const QString text = QStringLiteral("```mermaid\ngraph TD;\n  A-->B;\n\n  B-->C;\n```\n");
    QCOMPARE(html(text), QStringLiteral("<pre class=\"mermaid\">graph TD;\n  A--&gt;B;\n\n  B--&gt;C;\n</pre>\n"));
    const md::Document doc = md::parse(text);
    const Node *diagram = first(doc, NodeType::MermaidBlock);
    QVERIFY(diagram);
    // The source of the diagram is kept exactly.
    QCOMPARE(diagram->text, QStringLiteral("graph TD;\n  A-->B;\n\n  B-->C;\n"));
    QCOMPARE(diagram->label, QStringLiteral("mermaid"));
    QVERIFY(all(doc, NodeType::CodeBlock).empty());
    QVERIFY(first(md::parse(QStringLiteral("~~~Mermaid\npie\n~~~\n")), NodeType::MermaidBlock));
    // Any other language, or an indented block, is code.
    QVERIFY(!first(md::parse(QStringLiteral("```mermaidjs\nx\n```\n")), NodeType::MermaidBlock));
    QVERIFY(!first(md::parse(QStringLiteral("    mermaid\n")), NodeType::MermaidBlock));
    // Standard does not pretend diagrams are part of GFM.
    QCOMPARE(html(QStringLiteral("```mermaid\ngraph TD;\n```"), standard()),
             QStringLiteral("<pre><code class=\"language-mermaid\">graph TD;\n</code></pre>\n"));
}

void frontMatter()
{
    const QString yaml = QStringLiteral(
        "Customer: A. Example\n"
        "Me: The author\n"
        "date: 2026-11-16\n"
        "count: 3\n"
        "quoted: \"a: b # not a comment\\n\"\n"
        "single: 'it''s'\n"
        "tags:\n"
        "  - one\n"
        "  - \"two words\"\n"
        "flow: [a, \"b, c\", d]\n"
        "author:\n"
        "  name: Jane\n"
        "  contact:\n"
        "    email: jane@example.org\n"
        "empty:\n"
        "# a comment\n"
        "summary: |\n"
        "  First line\n"
        "    indented line\n"
        "folded: >\n"
        "  one\n"
        "  two\n"
        "people:\n"
        "  - name: A\n"
        "    age: 1\n"
        "  - name: B\n"
        "inline: {a: 1, b: [x, y]}\n"
        "unknown_field: kept # trailing comment\n");
    const QString text = QStringLiteral("---\n") + yaml + QStringLiteral("---\n\nBody.\n");
    const md::Document doc = md::parse(text);
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));
    const md::FrontMatter &fm = doc.frontMatter();
    QVERIFY(fm.present);
    // The text of the block is kept exactly, unknown fields included.
    QCOMPARE(fm.source, yaml);
    QCOMPARE(doc.files.front().text, text);
    QCOMPARE(text.mid(fm.start, fm.end - fm.start), QStringLiteral("---\n") + yaml + QStringLiteral("---"));
    QCOMPARE(text.mid(fm.contentStart, fm.contentEnd - fm.contentStart), yaml);
    QVERIFY(fm.data.isMap());

    // Scalars are looked up without regard to case.
    QCOMPARE(fm.scalar(u"Customer").value_or(QString()), QStringLiteral("A. Example"));
    QCOMPARE(fm.scalar(u"customer").value_or(QString()), QStringLiteral("A. Example"));
    QCOMPARE(fm.scalar(u"CUSTOMER").value_or(QString()), QStringLiteral("A. Example"));
    QCOMPARE(fm.scalar(u"me").value_or(QString()), QStringLiteral("The author"));
    QCOMPARE(fm.scalar(u"date").value_or(QString()), QStringLiteral("2026-11-16"));
    QCOMPARE(fm.scalar(u"count").value_or(QString()), QStringLiteral("3"));
    QCOMPARE(fm.scalar(u"quoted").value_or(QString()), QStringLiteral("a: b # not a comment\n"));
    QCOMPARE(fm.scalar(u"single").value_or(QString()), QStringLiteral("it's"));
    QCOMPARE(fm.scalar(u"summary").value_or(QString()), QStringLiteral("First line\n  indented line\n"));
    QCOMPARE(fm.scalar(u"folded").value_or(QString()), QStringLiteral("one two\n"));
    QCOMPARE(fm.scalar(u"unknown_field").value_or(QString()), QStringLiteral("kept"));
    QVERIFY(!fm.scalar(u"missing"));
    QVERIFY(!fm.scalar(u"empty"));
    // Lists and mappings are data, not scalars.
    QVERIFY(!fm.scalar(u"tags"));
    const md::YamlValue *tags = fm.data.find(u"tags");
    QVERIFY(tags && tags->isList());
    QCOMPARE(tags->list.size(), size_t(2));
    QCOMPARE(tags->list[0].scalar, QStringLiteral("one"));
    QCOMPARE(tags->list[1].scalar, QStringLiteral("two words"));
    const md::YamlValue *flow = fm.data.find(u"flow");
    QVERIFY(flow && flow->isList());
    QCOMPARE(flow->list.size(), size_t(3));
    QCOMPARE(flow->list[1].scalar, QStringLiteral("b, c"));
    const md::YamlValue *author = fm.data.find(u"author");
    QVERIFY(author && author->isMap());
    QCOMPARE(author->find(u"name")->scalar, QStringLiteral("Jane"));
    QCOMPARE(author->find(u"contact")->find(u"email")->scalar, QStringLiteral("jane@example.org"));
    const md::YamlValue *people = fm.data.find(u"people");
    QVERIFY(people && people->isList());
    QCOMPARE(people->list.size(), size_t(2));
    QCOMPARE(people->list[0].find(u"age")->scalar, QStringLiteral("1"));
    QCOMPARE(people->list[1].find(u"name")->scalar, QStringLiteral("B"));
    const md::YamlValue *map = fm.data.find(u"inline");
    QVERIFY(map && map->isMap());
    QCOMPARE(map->find(u"a")->scalar, QStringLiteral("1"));
    QCOMPARE(map->find(u"b")->list.size(), size_t(2));
    // The order of the keys is that of the source.
    QCOMPARE(fm.data.map.front().first, QStringLiteral("Customer"));
    QCOMPARE(fm.data.map.back().first, QStringLiteral("unknown_field"));

    // In the tree the block is a node that is not rendered.
    const Node *node = first(doc, NodeType::FrontMatter);
    QVERIFY(node);
    QCOMPARE(node->range, (md::Range{fm.start, fm.end}));
    QCOMPARE(node->text, yaml);
    QCOMPARE(doc.source(0, node->open).toString(), QStringLiteral("---"));
    QCOMPARE(doc.source(0, node->close).toString(), QStringLiteral("---"));
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>Body.</p>\n"));

    // Tags are never acted on: the value is read as data and the tag reported.
    const md::Document tagged = md::parse(QStringLiteral(
        "---\nrun: !!python/object/apply:os.system ['rm -rf /']\nplain: !custom tagged\nkeep: yes\n---\n"));
    QCOMPARE(count(tagged, DiagnosticCode::FrontMatter), 2);
    QVERIFY(tagged.frontMatter().data.find(u"run")->isList());
    QCOMPARE(tagged.frontMatter().scalar(u"plain").value_or(QString()), QStringLiteral("tagged"));
    QCOMPARE(tagged.frontMatter().scalar(u"keep").value_or(QString()), QStringLiteral("yes"));

    // Variants of the block itself.
    QVERIFY(md::parse(QStringLiteral("---\n---\nBody\n")).frontMatter().present);
    QVERIFY(md::parse(QStringLiteral("--- \ntitle: T\n---  \nBody\n")).frontMatter().present);
    QVERIFY(md::parse(QStringLiteral("---\ntitle: T\n...\nBody\n")).frontMatter().present);
    const md::Document bom = md::parse(QStringLiteral("﻿---\ntitle: T\n---\nBody\n"));
    QVERIFY(bom.frontMatter().present);
    QCOMPARE(bom.frontMatter().start, 1);
    QCOMPARE(bom.frontMatter().scalar(u"title").value_or(QString()), QStringLiteral("T"));
    QCOMPARE(md::toHtml(bom), QStringLiteral("<p>Body</p>\n"));
    const md::Document crlf = md::parse(QStringLiteral("---\r\ntitle: T\r\nlist:\r\n  - a\r\n---\r\nBody\r\n"));
    QVERIFY(crlf.frontMatter().present);
    QCOMPARE(crlf.frontMatter().scalar(u"title").value_or(QString()), QStringLiteral("T"));
    QCOMPARE(crlf.frontMatter().data.find(u"list")->list.size(), size_t(1));
    QCOMPARE(md::toHtml(crlf), QStringLiteral("<p>Body</p>\n"));

    // Anything else that looks like it is ordinary document content.
    const Case notMetadata[] = {
        {"Text\n\n---\na: b\n---\n", "<p>Text</p>\n<hr />\n<h2>a: b</h2>\n"},
        {"\n---\na: b\n---\n", "<hr />\n<h2>a: b</h2>\n"},
        {"---\na: b\n\ntext\n", "<hr />\n<p>a: b</p>\n<p>text</p>\n"},
        {"----\na: b\n----\n", "<hr />\n<h2>a: b</h2>\n"},
        {" ---\na: b\n---\n", "<hr />\n<h2>a: b</h2>\n"},
    };
    CHECK_CASES(notMetadata, blockWriter);
    QVERIFY(!md::parse(QStringLiteral("Text\n\n---\na: b\n---\n")).frontMatter().present);
    // A block that opens like metadata and is never closed says so.
    const md::Document unclosed = md::parse(QStringLiteral("---\ntitle: T\n\ntext\n"));
    QCOMPARE(count(unclosed, DiagnosticCode::FrontMatter), 1);
    QCOMPARE(unclosed.source(0, unclosed.diagnostics.front().range).toString(), QStringLiteral("---"));
    QCOMPARE(md::parse(QStringLiteral("---\n\ntext\n")).diagnostics.size(), size_t(0));
    QCOMPARE(md::parse(QStringLiteral("---\ntitle: T\n\ntext\n"), standard()).diagnostics.size(), size_t(0));
    QVERIFY(!first(md::parse(QStringLiteral("---\na: b\n\ntext\n")), NodeType::FrontMatter));

    // Standard is CommonMark, where the same lines are a rule and a heading;
    // the extension can be switched on for it.
    QCOMPARE(html(QStringLiteral("---\ntitle: T\n---\nBody\n"), standard()), QStringLiteral("<hr />\n<h2>title: T</h2>\n<p>Body</p>\n"));
    md::Options withMetadata = standard();
    withMetadata.frontMatter = true;
    QCOMPARE(html(QStringLiteral("---\ntitle: T\n---\nBody\n"), withMetadata), QStringLiteral("<p>Body</p>\n"));

    // The reader on its own.
    std::vector<md::FrontMatterIssue> issues;
    const md::YamlValue list = md::parseYaml(u"- a\n- - nested\n  - deep\n- key: value\n  other: 2\n", &issues);
    QVERIFY(list.isList());
    QCOMPARE(list.list.size(), size_t(3));
    QCOMPARE(list.list[1].list.size(), size_t(2));
    QCOMPARE(list.list[2].find(u"other")->scalar, QStringLiteral("2"));
    QVERIFY(issues.empty());
    const md::YamlValue odd = md::parseYaml(u"title: Multi\n  line value\nurl: https://example.org/a#b\nnull_value: ~\n\"quoted key\": v\n");
    QCOMPARE(odd.find(u"title")->scalar, QStringLiteral("Multi line value"));
    QCOMPARE(odd.find(u"url")->scalar, QStringLiteral("https://example.org/a#b"));
    QCOMPARE(odd.find(u"null_value")->kind, md::YamlValue::Kind::Null);
    QCOMPARE(odd.find(u"quoted key")->scalar, QStringLiteral("v"));
    md::parseYaml(u"good: 1\nnot a key line\n\"unclosed: x\n", &issues);
    QVERIFY(!issues.empty());
}

void variables()
{
    // The example of the dialect.
    QCOMPARE(html(QStringLiteral(
                 "---\nCustomer: A. Example\nMe: The author\n---\n\nDear [%customer],\n\nSincerely,\\\n[%me]\n")),
             QStringLiteral("<p>Dear A. Example,</p>\n<p>Sincerely,<br />\nThe author</p>\n"));

    const QString metadata = QStringLiteral(
        "---\nName: Jane *Doe* [%other] <b> &amp;\nOther: X\nlist: [a, b]\nnum: 42\nmap:\n  k: v\n---\n\n");
    auto render = [&](const QString &body) { return html(metadata + body); };
    const Case cases[] = {
        // Substituted once, as literal text: no formatting, no second look-up.
        {"[%name]", "<p>Jane *Doe* [%other] &lt;b&gt; &amp;amp;</p>\n"},
        // Keys are matched without regard to case or surrounding space.
        {"[%NAME] is [% other ]", "<p>Jane *Doe* [%other] &lt;b&gt; &amp;amp; is X</p>\n"},
        {"[%num] items", "<p>42 items</p>\n"},
        // Inside formatting, headings and link text.
        {"**[%other]**", "<p><strong>X</strong></p>\n"},
        {"# [%other] title", "<h1>X title</h1>\n"},
        {"[the [%other] page](u)", "<p><a href=\"u\">the X page</a></p>\n"},
        // Not in code, comments or link destinations.
        {"`[%other]`", "<p><code>[%other]</code></p>\n"},
        {"```\n[%other]\n```", "<pre><code>[%other]\n</code></pre>\n"},
        {"    [%other]", "<pre><code>[%other]\n</code></pre>\n"},
        {"<!-- [%other] -->\n\n// [%other]", ""},
        {"[link]([%other])", "<p><a href=\"%5B%25other%5D\">link</a></p>\n"},
        {"[link](<[%other]>)", "<p><a href=\"%5B%25other%5D\">link</a></p>\n"},
        {"<https://example.org/[%other]>", "<p><a href=\"https://example.org/%5B%25other%5D\">https://example.org/[%other]</a></p>\n"},
        // Escaped, it is text.
        {"\\[%other]", "<p>[%other]</p>\n"},
        // Not variables at all.
        {"[%] and [% ]", "<p>[%] and [% ]</p>\n"},
        {"[%20 off](u)", "<p><a href=\"u\">%20 off</a></p>\n"},
    };
    CHECK_CASES(cases, render);

    // What cannot be substituted stays visible and is reported.
    const md::Document missing = md::parse(metadata + QStringLiteral("[%missing] [%list] [%map] [%other]\n"));
    QCOMPARE(md::toHtml(missing), QStringLiteral("<p>[%missing] [%list] [%map] X</p>\n"));
    QCOMPARE(count(missing, DiagnosticCode::UnresolvedVariable), 3);
    const std::vector<const Node *> nodes = all(missing, NodeType::Variable);
    QCOMPARE(nodes.size(), size_t(4));
    QVERIFY(!nodes[0]->resolved && !nodes[1]->resolved && !nodes[2]->resolved && nodes[3]->resolved);
    QCOMPARE(nodes[3]->label, QStringLiteral("other"));
    QCOMPARE(nodes[3]->text, QStringLiteral("X"));
    QCOMPARE(missing.source(*nodes[3]).toString(), QStringLiteral("[%other]"));
    QCOMPARE(missing.source(0, nodes[3]->open).toString(), QStringLiteral("[%"));
    QCOMPARE(missing.source(0, nodes[3]->close).toString(), QStringLiteral("]"));
    QCOMPARE(missing.source(0, missing.diagnostics.front().range).toString(), QStringLiteral("[%missing]"));

    // The slug of a heading is made from the value.
    QCOMPARE(md::parse(metadata + QStringLiteral("# [%other] title\n")).headings.front().slug, QStringLiteral("x-title"));
    // Without metadata every variable is unresolved; in Standard it is text.
    QCOMPARE(count(md::parse(QStringLiteral("[%name]\n")), DiagnosticCode::UnresolvedVariable), 1);
    const md::Document plain = md::parse(QStringLiteral("[%name]\n"), standard());
    QCOMPARE(md::toHtml(plain), QStringLiteral("<p>[%name]</p>\n"));
    QVERIFY(plain.diagnostics.empty());
}

void includes()
{
    class CountingResolver : public md::MemoryIncludeResolver {
    public:
        QString canonicalPath(const QString &path) override { ++lookups; return md::MemoryIncludeResolver::canonicalPath(path); }
        int lookups = 0;
    } files;
    files.add(QStringLiteral("/lib/book/chapters/one.md"), QStringLiteral("## Chapter one\n\nText of *one*.\n"));
    files.add(QStringLiteral("/lib/shared/two.md"), QStringLiteral("## Chapter two\n\nText of two.\n"));
    files.add(QStringLiteral("/lib/book/my notes.txt"), QStringLiteral("Plain *notes*.\n"));
    files.add(QStringLiteral("/lib/book/part.md"), QStringLiteral("Part.\n\nchapters/one.md\n"));
    files.add(QStringLiteral("/lib/book/chapters/deep.md"), QStringLiteral("sub/leaf.markdown\n"));
    files.add(QStringLiteral("/lib/book/chapters/sub/leaf.markdown"), QStringLiteral("Leaf.\n"));
    files.add(QStringLiteral("/lib/book/chapters/pics.md"), QStringLiteral("![p](img/a.png) and [l](../other.md)\n"));
    files.add(QStringLiteral("/lib/book/meta.md"),
              QStringLiteral("---\ntitle: Chapter title\nlocal: L\n---\n\n[%title] [%local] [%rootonly]\n"));
    md::Source source;
    source.path = QStringLiteral("/lib/book/main.md");
    source.libraryRoot = QStringLiteral("/lib");
    source.resolver = &files;

    QCOMPARE(md::parse(QStringLiteral("An ordinary paragraph."), writer(), source).resourceFolders.size(), 0);
    QCOMPARE(files.lookups, 0);
    QCOMPARE(md::parse(QStringLiteral("<img src=\"local.png\"> ![other](local.png)"), writer(), source).resourceFolders.size(), 2);
    QCOMPARE(files.lookups, 2);

    // Relative to the document, rooted at the library, with ./ and with a
    // quoted path; with and without a caption.
    const QString text = QStringLiteral(
        "# Book\n\nchapters/one.md\n\n/shared/two.md \"Second\"\n\n./chapters/one.md\n\n\"my notes.txt\" 'Notes'\n\nEnd.\n");
    const md::Document doc = md::parse(text, writer(), source);
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));
    QVERIFY(!doc.incomplete);
    QCOMPARE(md::toHtml(doc),
             QStringLiteral("<h1 id=\"book\">Book</h1>\n"
                            "<h2 id=\"chapter-one\">Chapter one</h2>\n<p>Text of <em>one</em>.</p>\n"
                            "<h2 id=\"chapter-two\">Chapter two</h2>\n<p>Text of two.</p>\n"
                            "<h2 id=\"chapter-one-2\">Chapter one</h2>\n<p>Text of <em>one</em>.</p>\n"
                            "<p>Plain <em>notes</em>.</p>\n"
                            "<p>End.</p>\n"));

    // Every file is in the document once, with its own id, text and folder.
    QCOMPARE(doc.files.size(), size_t(4));
    QCOMPARE(doc.files[0].path, QStringLiteral("/lib/book/main.md"));
    QCOMPARE(doc.files[1].path, QStringLiteral("/lib/book/chapters/one.md"));
    QCOMPARE(doc.files[1].directory, QStringLiteral("/lib/book/chapters"));
    QCOMPARE(doc.files[1].includedFrom, 0);
    QCOMPARE(doc.files[2].path, QStringLiteral("/lib/shared/two.md"));
    QCOMPARE(doc.files[3].path, QStringLiteral("/lib/book/my notes.txt"));

    const std::vector<const Node *> nodes = all(doc, NodeType::Include);
    QCOMPARE(nodes.size(), size_t(4));
    QCOMPARE(nodes[0]->dest, QStringLiteral("chapters/one.md"));
    QVERIFY(nodes[0]->resolved);
    QCOMPARE(nodes[0]->includedFile, 1);
    QCOMPARE(nodes[0]->file, 0);
    QCOMPARE(doc.source(*nodes[0]).toString(), QStringLiteral("chapters/one.md"));
    QCOMPARE(nodes[1]->dest, QStringLiteral("/shared/two.md"));
    QCOMPARE(nodes[1]->title, QStringLiteral("Second"));
    QCOMPARE(nodes[1]->includedFile, 2);
    QCOMPARE(doc.source(*nodes[1]).toString(), QStringLiteral("/shared/two.md \"Second\""));
    QCOMPARE(doc.source(0, nodes[1]->target).toString(), QStringLiteral("/shared/two.md"));
    // The same file again is the same file.
    QCOMPARE(nodes[2]->includedFile, 1);
    QCOMPARE(nodes[3]->dest, QStringLiteral("my notes.txt"));
    QCOMPARE(nodes[3]->title, QStringLiteral("Notes"));
    QCOMPARE(nodes[3]->includedFile, 3);

    // Included nodes carry the id of their file and ranges into its text.
    const Node &heading = nodes[0]->children.front();
    QCOMPARE(heading.type, NodeType::Heading);
    QCOMPARE(heading.file, 1);
    QCOMPARE(doc.source(heading).toString(), QStringLiteral("## Chapter one"));
    const Node *emphasis = first(doc, NodeType::Emphasis);
    QCOMPARE(emphasis->file, 1);
    QCOMPARE(doc.source(*emphasis).toString(), QStringLiteral("*one*"));

    // The outline runs through the included files in order.
    QCOMPARE(doc.headings.size(), size_t(4));
    QCOMPARE(doc.headings[1].text, QStringLiteral("Chapter one"));
    QCOMPARE(doc.headings[1].file, 1);
    QCOMPARE(doc.headings[2].file, 2);
    QCOMPARE(doc.headings[3].slug, QStringLiteral("chapter-one-2"));
    QCOMPARE(doc.headings[3].file, 1);

    // An included file's own includes are found from its own folder.
    QCOMPARE(md::toHtml(md::parse(QStringLiteral("part.md\n\nchapters/deep.md\n"), writer(), source)),
             QStringLiteral("<p>Part.</p>\n<h2 id=\"chapter-one\">Chapter one</h2>\n<p>Text of <em>one</em>.</p>\n<p>Leaf.</p>\n"));

    // So are its images and links, whatever includes it.
    const md::Document assets = md::parse(QStringLiteral("chapters/pics.md\n\n![r](img/a.png)\n"), writer(), source);
    const std::vector<const Node *> images = all(assets, NodeType::Image);
    QCOMPARE(images.size(), size_t(2));
    QCOMPARE(md::classifyDestination(assets, *images[0]).path, QStringLiteral("/lib/book/chapters/img/a.png"));
    QCOMPARE(md::classifyDestination(assets, *images[1]).path, QStringLiteral("/lib/book/img/a.png"));
    QCOMPARE(md::classifyDestination(assets, *first(assets, NodeType::Link)).path, QStringLiteral("/lib/book/other.md"));

    // The contents list the included sections.
    QVERIFY(md::toHtml(md::parse(QStringLiteral("{{TOC}}\n\n# Book\n\nchapters/one.md\n"), writer(), source))
                .startsWith(QStringLiteral("<nav class=\"toc\">\n<ul>\n<li><a href=\"#book\">Book</a>\n<ul>\n"
                                           "<li><a href=\"#chapter-one\">Chapter one</a></li>\n</ul>\n</li>\n</ul>\n</nav>\n")));

    // Metadata: a file's own values first, then the root's; the included
    // file's front matter is kept and changes nothing of the root's.
    const md::Document meta = md::parse(
        QStringLiteral("---\ntitle: Root title\nrootonly: R\n---\n\n[%title] [%local]\n\nmeta.md\n"), writer(), source);
    QCOMPARE(md::toHtml(meta), QStringLiteral("<p>Root title [%local]</p>\n<p>Chapter title L R</p>\n"));
    QCOMPARE(meta.frontMatter().scalar(u"title").value_or(QString()), QStringLiteral("Root title"));
    QCOMPARE(meta.files[1].frontMatter.scalar(u"local").value_or(QString()), QStringLiteral("L"));
    QCOMPARE(all(meta, NodeType::FrontMatter).size(), size_t(2));
    QCOMPARE(count(meta, DiagnosticCode::UnresolvedVariable), 1);

    // Without a resolver an include is recognised and left as it is.
    const md::Document unexpanded = md::parse(text);
    QCOMPARE(all(unexpanded, NodeType::Include).size(), size_t(4));
    QVERIFY(!first(unexpanded, NodeType::Include)->resolved);
    QCOMPARE(first(unexpanded, NodeType::Include)->includedFile, -1);
    QVERIFY(unexpanded.diagnostics.empty());
    QVERIFY(!unexpanded.incomplete);
    QVERIFY(md::toHtml(unexpanded).contains(QLatin1StringView("<p>/shared/two.md &quot;Second&quot;</p>")));

    // What is not an include.
    const int readsBefore = files.reads();
    const Case notIncludes[] = {
        {"See chapters/one.md", "<p>See chapters/one.md</p>\n"},
        {"chapters/one.md\nmore text", "<p>chapters/one.md\nmore text</p>\n"},
        {"chapters/one.md trailing", "<p>chapters/one.md trailing</p>\n"},
        {"`chapters/one.md`", "<p><code>chapters/one.md</code></p>\n"},
        {"    chapters/one.md", "<pre><code>chapters/one.md\n</code></pre>\n"},
        {"```\nchapters/one.md\n```", "<pre><code>chapters/one.md\n</code></pre>\n"},
        {"- chapters/one.md", "<ul>\n<li>chapters/one.md</li>\n</ul>\n"},
        {"> chapters/one.md", "<blockquote>\n<p>chapters/one.md</p>\n</blockquote>\n"},
        {"https://example.org/a.md", "<p><a href=\"https://example.org/a.md\">https://example.org/a.md</a></p>\n"},
        {"chapters/one.pdf", "<p>chapters/one.pdf</p>\n"},
        {"**chapters/one.md**", "<p><strong>chapters/one.md</strong></p>\n"},
        {"// chapters/one.md", ""},
    };
    auto render = [&](const QString &body) { return md::toHtml(md::parse(body, writer(), source)); };
    CHECK_CASES(notIncludes, render);
    QCOMPARE(files.reads(), readsBefore);
    QCOMPARE(md::toHtml(md::parse(QStringLiteral("chapters/one.md\n"), standard(), source)),
             QStringLiteral("<p>chapters/one.md</p>\n"));
    md::Options off = writer();
    off.includes = false;
    QCOMPARE(md::toHtml(md::parse(QStringLiteral("chapters/one.md\n"), off, source)), QStringLiteral("<p>chapters/one.md</p>\n"));
}

void includeGuards()
{
    md::MemoryIncludeResolver files;
    files.add(QStringLiteral("/b/a.md"), QStringLiteral("In A.\n\nb.md\n"));
    files.add(QStringLiteral("/b/b.md"), QStringLiteral("In B.\n\n./a.md\n"));
    files.add(QStringLiteral("/b/self.md"), QStringLiteral("Self.\n\nself.md\n"));
    files.add(QStringLiteral("/b/back.md"), QStringLiteral("Back.\n\nmain.md\n"));
    files.add(QStringLiteral("/b/main.md"), QStringLiteral("The document itself, as it is on disk.\n"));
    md::Source source;
    source.path = QStringLiteral("/b/main.md");
    source.resolver = &files;

    // A cycle is found by canonical path and shown as the chain that closes it.
    md::Document doc = md::parse(QStringLiteral("a.md\n\nAfter.\n"), writer(), source);
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>In A.</p>\n<p>In B.</p>\n<p>./a.md</p>\n<p>After.</p>\n"));
    QCOMPARE(count(doc, DiagnosticCode::IncludeCycle), 1);
    QVERIFY(doc.incomplete);
    const md::Diagnostic cycle = doc.diagnostics.front();
    QCOMPARE(cycle.severity, md::Severity::Error);
    QVERIFY2(cycle.message.contains(QStringLiteral("main.md → a.md → b.md → a.md")), qPrintable(cycle.message));
    // The diagnostic is on the line that closes the cycle, in its own file.
    QCOMPARE(cycle.file, 2);
    QCOMPARE(doc.source(cycle.file, cycle.range).toString(), QStringLiteral("./a.md"));
    const std::vector<const Node *> nodes = all(doc, NodeType::Include);
    QCOMPARE(nodes.size(), size_t(3));
    QVERIFY(nodes[0]->resolved && nodes[1]->resolved && !nodes[2]->resolved);

    doc = md::parse(QStringLiteral("self.md\n"), writer(), source);
    QCOMPARE(count(doc, DiagnosticCode::IncludeCycle), 1);
    QVERIFY(doc.diagnostics.front().message.contains(QStringLiteral("main.md → self.md → self.md")));
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>Self.</p>\n<p>self.md</p>\n"));
    // Back to the document itself.
    doc = md::parse(QStringLiteral("back.md\n"), writer(), source);
    QCOMPARE(count(doc, DiagnosticCode::IncludeCycle), 1);
    QVERIFY(doc.diagnostics.front().message.contains(QStringLiteral("main.md → back.md → main.md")));
    // The same file twice in a row is no cycle.
    files.add(QStringLiteral("/b/leaf.md"), QStringLiteral("Leaf.\n"));
    doc = md::parse(QStringLiteral("leaf.md\n\nleaf.md\n"), writer(), source);
    QVERIFY(doc.diagnostics.empty());
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>Leaf.</p>\n<p>Leaf.</p>\n"));

    // Depth: 32 levels by default, and as configured.
    for (int i = 0; i < 40; ++i)
        files.add(QStringLiteral("/b/n%1.md").arg(i), QStringLiteral("Level %1.\n\nn%2.md\n").arg(i).arg(i + 1));
    files.add(QStringLiteral("/b/n40.md"), QStringLiteral("Bottom.\n"));
    doc = md::parse(QStringLiteral("n0.md\n"), writer(), source);
    QCOMPARE(count(doc, DiagnosticCode::IncludeDepthLimit), 1);
    QVERIFY(doc.incomplete);
    QCOMPARE(doc.diagnostics.front().severity, md::Severity::Error);
    QCOMPARE(all(doc, NodeType::Paragraph).size(), size_t(32));
    QVERIFY(md::toHtml(doc).contains(QLatin1StringView("<p>Level 31.</p>\n<p>n32.md</p>")));
    md::Source shallow = source;
    shallow.maxIncludeDepth = 3;
    doc = md::parse(QStringLiteral("n0.md\n"), writer(), shallow);
    QCOMPARE(count(doc, DiagnosticCode::IncludeDepthLimit), 1);
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>Level 0.</p>\n<p>Level 1.</p>\n<p>Level 2.</p>\n<p>n3.md</p>\n"));
    md::Source deep = source;
    deep.maxIncludeDepth = 64;
    doc = md::parse(QStringLiteral("n0.md\n"), writer(), deep);
    QVERIFY(doc.diagnostics.empty());
    QVERIFY(!doc.incomplete);
    QVERIFY(md::toHtml(doc).endsWith(QLatin1StringView("<p>Bottom.</p>\n")));

    // Size: the document and everything it includes, in UTF-8 bytes, each
    // inclusion counted.
    files.add(QStringLiteral("/b/forty.md"), QString(39, u'x') + u'\n');
    files.add(QStringLiteral("/b/accents.md"), QString(45, QChar(0xe9)) + u'\n'); // 91 bytes
    md::Source small = source;
    small.maxExpandedBytes = 110;
    doc = md::parse(QStringLiteral("forty.md\n\nforty.md\n\nforty.md\n"), writer(), small); // 29 + 40 + 40, then 40 more
    QCOMPARE(count(doc, DiagnosticCode::IncludeSizeLimit), 1);
    QVERIFY(doc.incomplete);
    QCOMPARE(all(doc, NodeType::Include).at(2)->resolved, false);
    QCOMPARE(all(doc, NodeType::Paragraph).size(), size_t(2));
    doc = md::parse(QStringLiteral("accents.md\n\nleaf.md\n"), writer(), small); // 20 + 91 is too much; 20 + 6 is not
    QCOMPARE(count(doc, DiagnosticCode::IncludeSizeLimit), 1);
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>accents.md</p>\n<p>Leaf.</p>\n"));
    QVERIFY(md::parse(QStringLiteral("forty.md\n\nforty.md\n\nforty.md\n"), writer(), source).diagnostics.empty());

    // Count: a guard against files that include each other many times over.
    md::Source few = source;
    few.maxIncludeCount = 2;
    doc = md::parse(QStringLiteral("leaf.md\n\nleaf.md\n\nleaf.md\n"), writer(), few);
    QCOMPARE(count(doc, DiagnosticCode::IncludeCountLimit), 1);
    QVERIFY(doc.incomplete);
    for (int i = 0; i < 24; ++i)
        files.add(QStringLiteral("/b/fan%1.md").arg(i), QStringLiteral("fan%1.md\n\nfan%1.md\n").arg(i + 1));
    files.add(QStringLiteral("/b/fan24.md"), QStringLiteral("x\n"));
    QElapsedTimer timer;
    timer.start();
    doc = md::parse(QStringLiteral("fan0.md\n"), writer(), source); // 2^24 leaves without the guard
    QVERIFY(count(doc, DiagnosticCode::IncludeCountLimit) >= 1);
    QVERIFY(doc.incomplete);
    QVERIFY2(timer.elapsed() < timeLimit(5000), qPrintable(QString::number(timer.elapsed())));

    // A path that cannot be placed, and a file that is not there.
    doc = md::parse(QStringLiteral("/shared/two.md\n"), writer(), source);
    QCOMPARE(count(doc, DiagnosticCode::IncludeNoLibrary), 1);
    QVERIFY(doc.incomplete);
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>/shared/two.md</p>\n"));
    md::Source unsaved;
    unsaved.resolver = &files;
    doc = md::parse(QStringLiteral("leaf.md\n"), writer(), unsaved);
    QCOMPARE(count(doc, DiagnosticCode::IncludeNoBaseFolder), 1);
    unsaved.directory = QStringLiteral("/b");
    doc = md::parse(QStringLiteral("leaf.md\n"), writer(), unsaved);
    QVERIFY(doc.diagnostics.empty());
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>Leaf.</p>\n"));
    doc = md::parse(QStringLiteral("Text.\n\nmissing.md \"Caption\"\n"), writer(), source);
    QCOMPARE(count(doc, DiagnosticCode::IncludeNotFound), 1);
    QCOMPARE(doc.diagnostics.front().severity, md::Severity::Error);
    QCOMPARE(doc.source(0, doc.diagnostics.front().range).toString(), QStringLiteral("missing.md \"Caption\""));
    QVERIFY(doc.incomplete);
    QCOMPARE(md::toHtml(doc), QStringLiteral("<p>Text.</p>\n<p>missing.md &quot;Caption&quot;</p>\n"));
}

void includeScopes()
{
    md::MemoryIncludeResolver files;
    files.add(QStringLiteral("/b/a.md"),
              QStringLiteral("Chapter A[^1], [ref], [rootref], [onlyb], [][#k].\n\n"
                             "[^1]: Note from A.\n\n[ref]: /from-a\n\n[#k]: Work of A.\n"));
    files.add(QStringLiteral("/b/b.md"),
              QStringLiteral("Chapter B[^1], [ref], [onlyb], [][#k], [][#rootkey], inline[^An inline note of B.].\n\n"
                             "[^1]: Note from B.\n\n[onlyb]: /only-b\n\n[#k]: Work of B.\n"));
    files.add(QStringLiteral("/b/c.md"), QStringLiteral("Chapter C[^1], [][#k].\n"));
    md::Source source;
    source.path = QStringLiteral("/b/main.md");
    source.resolver = &files;
    const QString root = QStringLiteral(
        "Root[^1], [ref], [onlyb].\n\na.md\n\nb.md\n\nc.md\n\na.md\n\n"
        "[^1]: Note from root.\n\n[ref]: /from-root\n[rootref]: /root-only\n\n[#k]: Work of root.\n[#rootkey]: Root only.\n");
    const md::Document doc = md::parse(root, writer(), source);
    md::HtmlOptions plain;
    plain.endnotes = false;
    auto note = [](int n, int occurrence = 1) {
        const QString id = occurrence > 1 ? QStringLiteral("%1-%2").arg(n).arg(occurrence) : QString::number(n);
        return QStringLiteral("<sup class=\"footnote-ref\"><a href=\"#fn-%1\" id=\"fnref-%2\">%1</a></sup>").arg(n).arg(id);
    };
    auto cite = [](int n) { return QStringLiteral("<span class=\"citation\">[<a href=\"#cite-%1\">%1</a>]</span>").arg(n); };
    QCOMPARE(md::toHtml(doc, plain),
             // The root uses its own definitions; one of a chapter's is not visible to it.
             QStringLiteral("<p>Root%1, <a href=\"/from-root\">ref</a>, [onlyb].</p>\n").arg(note(1))
                 // A chapter uses its own first, then the root's, never a sibling's.
                 + QStringLiteral("<p>Chapter A%1, <a href=\"/from-a\">ref</a>, <a href=\"/root-only\">rootref</a>, [onlyb], %2.</p>\n")
                       .arg(note(2), cite(1))
                 + QStringLiteral("<p>Chapter B%1, <a href=\"/from-root\">ref</a>, <a href=\"/only-b\">onlyb</a>, %2, %3, inline%4.</p>\n")
                       .arg(note(3), cite(2), cite(3), note(4))
                 // A chapter without definitions inherits the root's.
                 + QStringLiteral("<p>Chapter C%1, %2.</p>\n").arg(note(1, 2), cite(4))
                 // The same chapter again shares its notes and citations with its first inclusion.
                 + QStringLiteral("<p>Chapter A%1, <a href=\"/from-a\">ref</a>, <a href=\"/root-only\">rootref</a>, [onlyb], %2.</p>\n")
                       .arg(note(2, 2), cite(1)));

    // One note per definition, numbered across the whole document by first use.
    QCOMPARE(doc.footnotes.size(), size_t(4));
    QCOMPARE(doc.footnotes[0].file, 0);
    QCOMPARE(doc.footnotes[0].references, 2);
    QCOMPARE(md::plainText(*doc.footnotes[0].content), QStringLiteral("Note from root."));
    QCOMPARE(doc.footnotes[1].file, 1);
    QCOMPARE(doc.footnotes[1].references, 2);
    QCOMPARE(md::plainText(*doc.footnotes[1].content), QStringLiteral("Note from A."));
    QCOMPARE(doc.footnotes[2].file, 2);
    QCOMPARE(md::plainText(*doc.footnotes[2].content), QStringLiteral("Note from B."));
    QCOMPARE(doc.footnotes[3].content->type, NodeType::InlineNote);
    QCOMPARE(doc.footnotes[3].file, 2);
    QCOMPARE(doc.source(2, doc.footnotes[3].range).toString(), QStringLiteral("[^An inline note of B.]"));

    QCOMPARE(doc.bibliography.size(), size_t(4));
    QCOMPARE(md::plainText(*doc.bibliography[0].definition), QStringLiteral("Work of A."));
    QCOMPARE(md::plainText(*doc.bibliography[1].definition), QStringLiteral("Work of B."));
    QCOMPARE(md::plainText(*doc.bibliography[2].definition), QStringLiteral("Root only."));
    QCOMPARE(md::plainText(*doc.bibliography[3].definition), QStringLiteral("Work of root."));
    QCOMPARE(doc.bibliography[0].file, 1);
    QCOMPARE(doc.bibliography[3].file, 0);

    // Nothing is reported: `[onlyb]` is a plain bracket where it is not defined.
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));

    // An inline note is known by its file and place: included twice, it is
    // one note that is referred to twice.
    files.add(QStringLiteral("/b/inline.md"), QStringLiteral("Text[^The same note.] and[^Another note.].\n"));
    const md::Document twice = md::parse(QStringLiteral("inline.md\n\ninline.md\n\nRoot[^The same note.].\n"), writer(), source);
    QCOMPARE(twice.footnotes.size(), size_t(3));
    QCOMPARE(twice.footnotes[0].references, 2);
    QCOMPARE(twice.footnotes[1].references, 2);
    QCOMPARE(twice.footnotes[2].references, 1);
    QCOMPARE(twice.footnotes[2].file, 0);
    QCOMPARE(md::toHtml(twice, plain),
             QStringLiteral("<p>Text%1 and%2.</p>\n<p>Text%3 and%4.</p>\n<p>Root%5.</p>\n")
                 .arg(note(1), note(2), note(1, 2), note(2, 2), note(3)));

    // Headings: a label resolves within its own file before the whole
    // document, and ids stay unique across files.
    files.add(QStringLiteral("/b/h1.md"), QStringLiteral("# Intro [L]\n\n[x][L] [y][Intro]\n"));
    files.add(QStringLiteral("/b/h2.md"), QStringLiteral("# Intro [L]\n\n[x][L] [far][Only in root]\n"));
    const md::Document headings = md::parse(
        QStringLiteral("h1.md\n\nh2.md\n\n# Only in root\n\n[z][L] [w][Only in root]\n"), writer(), source);
    QCOMPARE(md::toHtml(headings),
             QStringLiteral("<h1 id=\"intro\">Intro</h1>\n<p><a href=\"#intro\">x</a> <a href=\"#intro\">y</a></p>\n"
                            "<h1 id=\"intro-2\">Intro</h1>\n<p><a href=\"#intro-2\">x</a> <a href=\"#only-in-root\">far</a></p>\n"
                            "<h1 id=\"only-in-root\">Only in root</h1>\n"
                            "<p><a href=\"#intro\">z</a> <a href=\"#only-in-root\">w</a></p>\n"));
    // Only the reference from outside the two files cannot tell them apart.
    QCOMPARE(count(headings, DiagnosticCode::AmbiguousHeadingReference), 1);
    QCOMPARE(headings.diagnostics.size(), size_t(1));
    QCOMPARE(headings.diagnostics.front().file, 0);
    QCOMPARE(headings.source(0, headings.diagnostics.front().range).toString(), QStringLiteral("[z][L]"));

    // A missing note in a chapter is reported in that chapter's file.
    files.add(QStringLiteral("/b/broken.md"), QStringLiteral("Text[^nowhere] and [%novalue].\n"));
    const md::Document broken = md::parse(QStringLiteral("broken.md\n"), writer(), source);
    QCOMPARE(broken.diagnostics.size(), size_t(2));
    QCOMPARE(broken.diagnostics[0].file, 1);
    QCOMPARE(broken.diagnostics[0].code, DiagnosticCode::MissingFootnote);
    QCOMPARE(broken.source(1, broken.diagnostics[0].range).toString(), QStringLiteral("[^nowhere]"));
    QCOMPARE(broken.diagnostics[1].code, DiagnosticCode::UnresolvedVariable);
}

void includesFromDisk()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = QFileInfo(folder.path()).canonicalFilePath();
    auto write = [&](const QString &name, const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(base + u'/' + name).path());
        QFile file(base + u'/' + name);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QVERIFY(write(QStringLiteral("book/main.md"), "# Book\n\nchapters/one.md\n"));
    QVERIFY(write(QStringLiteral("book/chapters/one.md"), "Caf\xc3\xa9 text.\n\n../link.md\n\n/rooted.md\n"));
    QVERIFY(write(QStringLiteral("book/target.md"), "Through a link.\n\nchapters/one.md\n"));
    QVERIFY(QFile::link(base + QStringLiteral("/book/target.md"), base + QStringLiteral("/book/link.md")));
    QVERIFY(write(QStringLiteral("rooted.md"), "From the library root.\n"));
    QVERIFY(write(QStringLiteral("book/latin1.md"), "Caf\xe9\n"));
    QVERIFY(QDir().mkpath(base + QStringLiteral("/book/folder.md")));

    md::FileIncludeResolver resolver;
    md::Source source;
    source.path = base + QStringLiteral("/book/main.md");
    source.libraryRoot = base;
    source.resolver = &resolver;
    QFile main(source.path);
    QVERIFY(main.open(QIODevice::ReadOnly));
    const md::Document doc = md::parse(QString::fromUtf8(main.readAll()), writer(), source);

    // Files are read as UTF-8 and known by their canonical paths, so the
    // cycle that runs through a symbolic link is found.
    QCOMPARE(md::toHtml(doc),
             QStringLiteral("<h1 id=\"book\">Book</h1>\n<p>Café text.</p>\n<p>Through a link.</p>\n<p>chapters/one.md</p>\n"
                            "<p>From the library root.</p>\n"));
    QCOMPARE(count(doc, DiagnosticCode::IncludeCycle), 1);
    QCOMPARE(doc.diagnostics.size(), size_t(1));
    QVERIFY2(doc.diagnostics.front().message.contains(
                 QStringLiteral("main.md → chapters/one.md → target.md → chapters/one.md")),
             qPrintable(doc.diagnostics.front().message));
    QCOMPARE(doc.files.size(), size_t(4));
    QCOMPARE(doc.files[1].path, base + QStringLiteral("/book/chapters/one.md"));
    QCOMPARE(doc.files[2].path, base + QStringLiteral("/book/target.md"));
    QCOMPARE(doc.files[2].directory, base + QStringLiteral("/book"));
    QCOMPARE(doc.files[3].path, base + QStringLiteral("/rooted.md"));

    // What cannot be read says why.
    const md::Document unreadable = md::parse(QStringLiteral("latin1.md\n\nfolder.md\n\nnone.md\n"), writer(), source);
    QCOMPARE(count(unreadable, DiagnosticCode::IncludeUnreadable), 2);
    QCOMPARE(count(unreadable, DiagnosticCode::IncludeNotFound), 1);
    QVERIFY(unreadable.diagnostics[0].message.contains(QLatin1StringView("UTF-8")));
    QVERIFY(unreadable.incomplete);

    // The size guard is applied before a file is read.
    QVERIFY(write(QStringLiteral("book/large.md"), QByteArray(4096, 'x')));
    md::Source small = source;
    small.maxExpandedBytes = 1024;
    QCOMPARE(count(md::parse(QStringLiteral("large.md\n"), writer(), small), DiagnosticCode::IncludeSizeLimit), 1);
    const md::IncludeResolver::File refused = resolver.read(base + QStringLiteral("/book/large.md"), 1024);
    QVERIFY(!refused.ok && refused.tooLarge && refused.text.isEmpty());

    QCOMPARE(md::includeTarget(QStringLiteral("a/../b.md"), QStringLiteral("/x/y"), QString()), QStringLiteral("/x/y/b.md"));
    QCOMPARE(md::includeTarget(QStringLiteral("/b.md"), QStringLiteral("/x/y"), QStringLiteral("/lib")), QStringLiteral("/lib/b.md"));
    QCOMPARE(md::includeTarget(QStringLiteral("/b.md"), QStringLiteral("/x/y"), QString()), QString());
    QCOMPARE(md::includeTarget(QStringLiteral("b.md"), QString(), QStringLiteral("/lib")), QString());
}

namespace {

// A document with every kind of construct, for the tests of ranges.
QString mixedDocument()
{
    return QStringLiteral(
        "---\ntitle: Mixed\n---\n"
        "\n"
        "# Heading *one* [Label]\n"
        "\n"
        "Plain **bold _nested_** text with `code`, a [link](https://example.org \"t\"),\n"
        "an ![image](pic.png), ==mark==, ~~gone~~, x^2 and y^(a+b)^, H~2~O, \\* and &amp;.  \n"
        "Second line with $a+b$, [%title], a note[^n], an inline[^Inline *note*.],\n"
        "a cite[p. 2][#key], [@key, p. 3], <https://example.org>, www.example.org,\n"
        "me@example.org, <sup>up</sup> and <!-- hidden --> and [ref][Label].\n"
        "// a comment line\n"
        "Last line of the paragraph.\n"
        "\n"
        "Setext heading\n"
        "--------------\n"
        "\n"
        "> A quote with *emphasis\n"
        "> over two lines* and\n"
        "lazy continuation.\n"
        ">\n"
        "> - quoted item\n"
        "\n"
        "1. First item\n"
        "2. [x] Task item\n"
        "\n"
        "   Second paragraph of the item.\n"
        "   - nested `code`\n"
        "\n"
        "| Left | Right |\n"
        "| :--- | ----: |\n"
        "| a \\| b | **c** |\n"
        "| wide ||\n"
        "\n"
        "Term\n"
        ": Definition with *text*\n"
        "\n"
        "```python\n"
        "print(1)\n"
        "```\n"
        "\n"
        "    indented code\n"
        "\n"
        "$$\n"
        "x = y\n"
        "$$\n"
        "\n"
        "<ul>\n"
        "<li>raw &amp; <b>bold</b></li>\n"
        "</ul>\n"
        "\n"
        "+++\n"
        "\n"
        "{{TOC}}\n"
        "\n"
        "chapter.md \"Caption\"\n"
        "\n"
        "* * *\n"
        "\n"
        "[^n]: The note, with a [link](x).\n"
        "\n"
        "    Its second paragraph.\n"
        "\n"
        "[#key]: A *work*.\n"
        "\n"
        "[label2]: https://example.org/two 'Title'\n");
}

// What must hold of every node: its range lies in its file and in its
// parent, its markers lie at its ends, its children are in order and do
// not overlap, and text is exactly the source it covers.
void checkRanges(const md::Document &doc, const Node &node, const Node *parent, QStringList &errors)
{
    auto fail = [&](const QString &what) {
        errors.append(QStringLiteral("%1 [%2,%3) in file %4: %5")
                          .arg(QLatin1StringView(md::typeName(node.type))).arg(node.range.start).arg(node.range.end)
                          .arg(node.file).arg(what));
    };
    const int size = int(doc.files[node.file].text.size());
    if (node.range.start < 0 || node.range.end < node.range.start || node.range.end > size) fail(QStringLiteral("outside its file"));
    if (!node.generated) {
        if (parent && parent->file == node.file && !parent->generated && parent->type != NodeType::Include
            && (node.range.start < parent->range.start || node.range.end > parent->range.end))
            fail(QStringLiteral("outside its parent"));
        for (const md::Range marker : {node.open, node.close}) {
            if (marker.start > marker.end || marker.start < node.range.start || marker.end > node.range.end)
                fail(QStringLiteral("a marker outside the node"));
        }
        if (!node.open.isEmpty() && node.open.start != node.range.start && node.type != NodeType::Table)
            fail(QStringLiteral("the opening marker is not at the start"));
        if (!node.close.isEmpty() && node.close.end != node.range.end) fail(QStringLiteral("the closing marker is not at the end"));
        if (node.open.isEmpty() && node.open.start != node.range.start && node.type != NodeType::Table)
            fail(QStringLiteral("an empty opening marker is not at the start"));
        if (node.close.isEmpty() && node.close.end != node.range.end) fail(QStringLiteral("an empty closing marker is not at the end"));
        if (node.type == NodeType::Text && doc.source(node) != node.text)
            fail(QStringLiteral("text \"%1\" is not its source \"%2\"").arg(node.text, doc.source(node)));
        if (node.type == NodeType::Text && node.text.isEmpty()) fail(QStringLiteral("empty text"));
        if (!node.target.isEmpty() && (node.target.start < node.range.start || node.target.end > node.range.end))
            fail(QStringLiteral("the target is outside the node"));
        for (const md::Range marker : node.markers)
            if (marker.start < node.range.start || marker.end > node.range.end) fail(QStringLiteral("a marker outside the node"));
    }
    int last = node.range.start;
    for (const Node &child : node.children) {
        if (child.file == node.file && !child.generated && !node.generated) {
            if (child.range.start < last) fail(QStringLiteral("children overlap or are out of order"));
            last = child.range.end;
        }
        checkRanges(doc, child, &node, errors);
    }
}

int treeDepth(const Node &node)
{
    int deepest = 0;
    for (const Node &child : node.children) deepest = qMax(deepest, treeDepth(child));
    return deepest + 1;
}

} // namespace

void sourceRanges()
{
    md::MemoryIncludeResolver files;
    files.add(QStringLiteral("/d/chapter.md"), QStringLiteral("## Included *heading*\n\n> Included [text](a.md) with a note[^1].\n\n[^1]: Chapter note.\n"));
    md::Source source;
    source.path = QStringLiteral("/d/main.md");
    source.resolver = &files;

    const QString lf = mixedDocument();
    QString crlf = lf;
    crlf.replace(QLatin1StringView("\n"), QLatin1StringView("\r\n"));
    // The same document as the content of a list item inside a quote, so
    // that every line carries a prefix that is not part of its content.
    QString nested = QStringLiteral("> - ") + lf;
    nested.replace(QLatin1StringView("\n"), QLatin1StringView("\n>   "));
    for (const QString &text : {lf, crlf, nested}) {
        for (const md::Options &options : {writer(), standard()}) {
            const md::Document doc = md::parse(text, options, source);
            QCOMPARE(doc.root.range, (md::Range{0, int(text.size())}));
            QStringList errors;
            checkRanges(doc, doc.root, nullptr, errors);
            QVERIFY2(errors.isEmpty(), qPrintable(errors.join(u'\n')));
        }
    }

    // The source each node of the document covers, in document order.
    const md::Document doc = md::parse(lf, writer(), source);
    QVERIFY2(doc.diagnostics.empty(), qPrintable(describe(doc)));
    auto sources = [&](NodeType type) {
        QStringList out;
        for (const Node *node : all(doc, type))
            if (node->file == 0 && !node->generated) out.append(doc.source(*node).toString());
        return out;
    };
    QCOMPARE(sources(NodeType::FrontMatter), (QStringList{QStringLiteral("---\ntitle: Mixed\n---")}));
    QCOMPARE(sources(NodeType::Heading),
             (QStringList{QStringLiteral("# Heading *one* [Label]"), QStringLiteral("Setext heading\n--------------")}));
    QCOMPARE(sources(NodeType::HeadingLabel), (QStringList{QStringLiteral("[Label]")}));
    QCOMPARE(sources(NodeType::Strong), (QStringList{QStringLiteral("**bold _nested_**"), QStringLiteral("**c**")}));
    QCOMPARE(sources(NodeType::Emphasis),
             (QStringList{QStringLiteral("*one*"), QStringLiteral("_nested_"), QStringLiteral("*note*"),
                          QStringLiteral("*emphasis\n> over two lines*"), QStringLiteral("*text*"), QStringLiteral("*work*")}));
    QCOMPARE(sources(NodeType::Code), (QStringList{QStringLiteral("`code`"), QStringLiteral("`code`")}));
    QCOMPARE(sources(NodeType::Link),
             (QStringList{QStringLiteral("[link](https://example.org \"t\")"), QStringLiteral("[ref][Label]"),
                          QStringLiteral("[link](x)")}));
    QCOMPARE(sources(NodeType::Image), (QStringList{QStringLiteral("![image](pic.png)")}));
    QCOMPARE(sources(NodeType::Highlight), (QStringList{QStringLiteral("==mark==")}));
    QCOMPARE(sources(NodeType::Strikethrough), (QStringList{QStringLiteral("~~gone~~")}));
    QCOMPARE(sources(NodeType::Superscript), (QStringList{QStringLiteral("^2"), QStringLiteral("^(a+b)^")}));
    QCOMPARE(sources(NodeType::Subscript), (QStringList{QStringLiteral("~2~")}));
    QCOMPARE(sources(NodeType::Escape), (QStringList{QStringLiteral("\\*")}));
    QCOMPARE(sources(NodeType::Entity), (QStringList{QStringLiteral("&amp;"), QStringLiteral("&amp;")}));
    QCOMPARE(sources(NodeType::HardBreak), (QStringList{QStringLiteral("  \n")}));
    QCOMPARE(sources(NodeType::MathInline), (QStringList{QStringLiteral("$a+b$")}));
    QCOMPARE(sources(NodeType::Variable), (QStringList{QStringLiteral("[%title]")}));
    QCOMPARE(sources(NodeType::FootnoteReference), (QStringList{QStringLiteral("[^n]")}));
    QCOMPARE(sources(NodeType::InlineNote), (QStringList{QStringLiteral("[^Inline *note*.]")}));
    QCOMPARE(sources(NodeType::Citation), (QStringList{QStringLiteral("[p. 2][#key]"), QStringLiteral("[@key, p. 3]")}));
    QCOMPARE(sources(NodeType::Autolink),
             (QStringList{QStringLiteral("<https://example.org>"), QStringLiteral("www.example.org"),
                          QStringLiteral("me@example.org")}));
    QCOMPARE(sources(NodeType::HtmlComment), (QStringList{QStringLiteral("<!-- hidden -->")}));
    QCOMPARE(sources(NodeType::CommentLine), (QStringList{QStringLiteral("// a comment line")}));
    QCOMPARE(sources(NodeType::BlockQuote),
             (QStringList{QStringLiteral("> A quote with *emphasis\n> over two lines* and\nlazy continuation.\n>\n> - quoted item")}));
    QCOMPARE(sources(NodeType::ListItem),
             (QStringList{QStringLiteral("- quoted item"), QStringLiteral("1. First item"),
                          QStringLiteral("2. [x] Task item\n\n   Second paragraph of the item.\n   - nested `code`"),
                          QStringLiteral("- nested `code`")}));
    QCOMPARE(sources(NodeType::TaskMarker), (QStringList{QStringLiteral("[x]")}));
    QCOMPARE(sources(NodeType::Table),
             (QStringList{QStringLiteral("| Left | Right |\n| :--- | ----: |\n| a \\| b | **c** |\n| wide ||")}));
    QCOMPARE(sources(NodeType::TableRow),
             (QStringList{QStringLiteral("| Left | Right |"), QStringLiteral("| a \\| b | **c** |"), QStringLiteral("| wide ||")}));
    QCOMPARE(sources(NodeType::TableCell),
             (QStringList{QStringLiteral("Left"), QStringLiteral("Right"), QStringLiteral("a \\| b"), QStringLiteral("**c**"),
                          QStringLiteral("wide")}));
    QCOMPARE(sources(NodeType::DefinitionList), (QStringList{QStringLiteral("Term\n: Definition with *text*")}));
    QCOMPARE(sources(NodeType::DefinitionTerm), (QStringList{QStringLiteral("Term")}));
    QCOMPARE(sources(NodeType::DefinitionDescription), (QStringList{QStringLiteral(": Definition with *text*")}));
    QCOMPARE(sources(NodeType::CodeBlock), (QStringList{QStringLiteral("```python\nprint(1)\n```"), QStringLiteral("indented code")}));
    QCOMPARE(sources(NodeType::MathBlock), (QStringList{QStringLiteral("$$\nx = y\n$$")}));
    QCOMPARE(sources(NodeType::HtmlBlock), (QStringList{QStringLiteral("<ul>\n<li>raw &amp; <b>bold</b></li>\n</ul>")}));
    QCOMPARE(sources(NodeType::HtmlElement),
             (QStringList{QStringLiteral("<sup>up</sup>"), QStringLiteral("<ul>\n<li>raw &amp; <b>bold</b></li>\n</ul>"),
                          QStringLiteral("<li>raw &amp; <b>bold</b></li>"), QStringLiteral("<b>bold</b>")}));
    QCOMPARE(sources(NodeType::PageBreak), (QStringList{QStringLiteral("+++")}));
    QCOMPARE(sources(NodeType::TableOfContents), (QStringList{QStringLiteral("{{TOC}}")}));
    QCOMPARE(sources(NodeType::Include), (QStringList{QStringLiteral("chapter.md \"Caption\"")}));
    QCOMPARE(sources(NodeType::ThematicBreak), (QStringList{QStringLiteral("* * *")}));
    QCOMPARE(sources(NodeType::FootnoteDefinition),
             (QStringList{QStringLiteral("[^n]: The note, with a [link](x).\n\n    Its second paragraph.")}));
    QCOMPARE(sources(NodeType::CitationDefinition), (QStringList{QStringLiteral("[#key]: A *work*.")}));
    QCOMPARE(sources(NodeType::LinkDefinition), (QStringList{QStringLiteral("[label2]: https://example.org/two 'Title'")}));

    // Text nodes are the literal runs between the punctuation, each exactly
    // its source; here those of the first paragraph's first line.
    const Node *paragraph = first(doc, NodeType::Paragraph);
    QVERIFY(paragraph);
    QStringList runs;
    for (const Node &child : paragraph->children) {
        if (child.type == NodeType::SoftBreak) break;
        if (child.type == NodeType::Text) runs.append(child.text);
    }
    QCOMPARE(runs, (QStringList{QStringLiteral("Plain "), QStringLiteral(" text with "), QStringLiteral(", a "), QStringLiteral(",")}));

    // The included file's nodes have ranges in the included file.
    const Node *included = nullptr;
    for (const Node *heading : all(doc, NodeType::Heading))
        if (heading->file == 1) included = heading;
    QVERIFY(included);
    QCOMPARE(doc.source(*included).toString(), QStringLiteral("## Included *heading*"));
    for (const Node *quote : all(doc, NodeType::BlockQuote)) {
        if (quote->file != 1) continue;
        QCOMPARE(doc.source(*quote).toString(), QStringLiteral("> Included [text](a.md) with a note[^1]."));
    }

    // With CRLF line endings the ranges are those of the CRLF text.
    const md::Document windows = md::parse(crlf, writer(), source);
    const Node *bold = first(windows, NodeType::Strong);
    QCOMPARE(windows.source(*bold).toString(), QStringLiteral("**bold _nested_**"));
    const Node *hard = first(windows, NodeType::HardBreak);
    QCOMPARE(windows.source(*hard).toString(), QStringLiteral("  \r\n"));
    QCOMPARE(windows.source(*first(windows, NodeType::BlockQuote)).toString(),
             QStringLiteral("> A quote with *emphasis\r\n> over two lines* and\r\nlazy continuation.\r\n>\r\n> - quoted item"));
    QCOMPARE(md::toHtml(windows), md::toHtml(doc));
}

void markerRanges()
{
    struct Marker {
        const char *text;
        NodeType type;
        const char *open;
        const char *close;
    };
    const Marker markers[] = {
        {"a **strong** b", NodeType::Strong, "**", "**"},
        {"a __strong__ b", NodeType::Strong, "__", "__"},
        {"a *em* b", NodeType::Emphasis, "*", "*"},
        {"a ***both*** b", NodeType::Emphasis, "*", "*"},
        {"a ***both*** b", NodeType::Strong, "**", "**"},
        {"a ~~strike~~ b", NodeType::Strikethrough, "~~", "~~"},
        {"a ==mark== b", NodeType::Highlight, "==", "=="},
        {"x^2 b", NodeType::Superscript, "^", ""},
        {"x^(2)^ b", NodeType::Superscript, "^", "^"},
        {"x~2 b", NodeType::Subscript, "~", ""},
        {"x~2,3~ b", NodeType::Subscript, "~", "~"},
        {"a ``co`de`` b", NodeType::Code, "``", "``"},
        {"a [text](url \"title\") b", NodeType::Link, "[", "](url \"title\")"},
        {"a [text][ref] b\n\n[ref]: /u", NodeType::Link, "[", "][ref]"},
        {"a [ref][] b\n\n[ref]: /u", NodeType::Link, "[", "][]"},
        {"a [ref] b\n\n[ref]: /u", NodeType::Link, "[", "]"},
        {"a ![alt](pic.png) b", NodeType::Image, "![", "](pic.png)"},
        {"a <https://example.org> b", NodeType::Autolink, "<", ">"},
        {"a https://example.org b", NodeType::Autolink, "", ""},
        {"a $x$ b", NodeType::MathInline, "$", "$"},
        {"a $$x$$ b", NodeType::MathInline, "$$", "$$"},
        {"a[^1] b\n\n[^1]: n", NodeType::FootnoteReference, "[^", "]"},
        {"a[^An inline note.] b", NodeType::InlineNote, "[^", "]"},
        {"a[p. 1][#k] b\n\n[#k]: w", NodeType::Citation, "[", "][#k]"},
        {"a [@k, p. 1] b\n\n[#k]: w", NodeType::Citation, "[", "]"},
        {"---\nk: v\n---\na [%k] b", NodeType::Variable, "[%", "]"},
        {"a <sup>x</sup> b", NodeType::HtmlElement, "<sup>", "</sup>"},
        {"a \\* b", NodeType::Escape, "\\", ""},
        {"## Heading ##", NodeType::Heading, "##", "##"},
        {"Heading\n===", NodeType::Heading, "", "==="},
        {"- item", NodeType::ListItem, "-", ""},
        {"12. item", NodeType::ListItem, "12.", ""},
        {"> quote", NodeType::BlockQuote, ">", ""},
        {"```js\ncode\n```", NodeType::CodeBlock, "```", "```"},
        {"~~~~\ncode\n~~~~~", NodeType::CodeBlock, "~~~~", "~~~~~"},
        {"$$\nx\n$$", NodeType::MathBlock, "$$", "$$"},
        {"$$x$$", NodeType::MathBlock, "$$", "$$"},
        {"Term\n: Definition", NodeType::DefinitionDescription, ":", ""},
        {"[^1]: note\n\nx[^1]", NodeType::FootnoteDefinition, "[^1]:", ""},
        {"[#k]: work", NodeType::CitationDefinition, "[#k]:", ""},
        {"[ref]: /url", NodeType::LinkDefinition, "[ref]:", ""},
        {"// comment", NodeType::CommentLine, "//", ""},
    };
    for (const Marker &m : markers) {
        const QString text = QString::fromUtf8(m.text);
        const md::Document doc = md::parse(text);
        const Node *node = first(doc, m.type);
        QVERIFY2(node, m.text);
        const QString open = doc.source(node->file, node->open).toString();
        const QString close = doc.source(node->file, node->close).toString();
        if (open != QString::fromUtf8(m.open) || close != QString::fromUtf8(m.close))
            QFAIL(qPrintable(QStringLiteral("%1: markers \"%2\" and \"%3\", expected \"%4\" and \"%5\"")
                                 .arg(text, open, close, QString::fromUtf8(m.open), QString::fromUtf8(m.close))));
        // The markers are at the two ends of the node.
        QCOMPARE(node->open.start, node->range.start);
        QCOMPARE(node->close.end, node->range.end);
    }

    // A quote marks every line it has a marker on; a table row its pipes.
    const QString quoted = QStringLiteral("> one\n> two\nlazy\n> > three\n");
    const md::Document quote = md::parse(quoted);
    const std::vector<const Node *> quotes = all(quote, NodeType::BlockQuote);
    QCOMPARE(quotes.size(), size_t(2));
    QCOMPARE(quotes[0]->markers, (std::vector<md::Range>{{0, 1}, {6, 7}, {17, 18}}));
    QCOMPARE(quotes[1]->markers, (std::vector<md::Range>{{19, 20}}));
    const QString row = QStringLiteral("| a | b |\n|---|---|\n c | d\n");
    const md::Document table = md::parse(row);
    const std::vector<const Node *> rows = all(table, NodeType::TableRow);
    QCOMPARE(rows[0]->markers, (std::vector<md::Range>{{0, 1}, {4, 5}, {8, 9}}));
    QCOMPARE(rows[1]->markers, (std::vector<md::Range>{{23, 24}}));

    // Breaks cover what makes them: the line ending, and the two spaces or
    // the backslash of a hard one.
    const QString breaks = QStringLiteral("soft\nhard  \nslash\\\nend");
    const md::Document broken = md::parse(breaks);
    QCOMPARE(first(broken, NodeType::SoftBreak)->range, (md::Range{4, 5}));
    const std::vector<const Node *> hard = all(broken, NodeType::HardBreak);
    QCOMPARE(hard.size(), size_t(2));
    QCOMPARE(broken.source(*hard[0]).toString(), QStringLiteral("  \n"));
    QCOMPARE(broken.source(*hard[1]).toString(), QStringLiteral("\\\n"));

    // The targets: a destination, a reference label, an info string, a path.
    const QString targets = QStringLiteral("[a](<my url> \"t\") [b][Ref One] ![c](pic.png)\n\n[ref one]: /u\n\n```python linenos\n```\n\nfile.md\n");
    const md::Document target = md::parse(targets);
    const std::vector<const Node *> links = all(target, NodeType::Link);
    QCOMPARE(target.source(0, links[0]->target).toString(), QStringLiteral("my url"));
    QCOMPARE(target.source(0, links[1]->target).toString(), QStringLiteral("Ref One"));
    QCOMPARE(target.source(0, first(target, NodeType::Image)->target).toString(), QStringLiteral("pic.png"));
    QCOMPARE(target.source(0, first(target, NodeType::LinkDefinition)->target).toString(), QStringLiteral("/u"));
    QCOMPARE(target.source(0, first(target, NodeType::CodeBlock)->target).toString(), QStringLiteral("python linenos"));
    QCOMPARE(target.source(0, first(target, NodeType::Include)->target).toString(), QStringLiteral("file.md"));
}

void randomDocuments()
{
    // Documents thrown together from every kind of line and fragment, in
    // any order and at any indentation. Whatever they turn out to mean, the
    // tree must be sound: ranges in order and inside their parents, text
    // equal to its source, nothing lost from the reading text's sources.
    static const char *const lines[] = {
        "# Heading [Label]", "## Closed ##", "Setext", "=====", "-----", "text", "more *text* here", "", "", "",
        "- item", "- [ ] task", "- [x] done", "* star", "+ plus", "1. one", "2) two", "10. ten", "> quote", "> > deeper", ">",
        "```", "```mermaid", "~~~~", "---", "***", "+++", "{{TOC}}", "// comment", "//", "$$", "$$ x $$", "$$x",
        "| a | b |", "| - | - |", "| :-: | -: |", "| c || d |", "|| e |", "a | b", "- | -", "| `x|y` | \\| |", "|",
        "Term", ": definition", ":   wide", "[^n]: note text", "[^n]:", "[#k]: A *work*.", "[ref]: /url 'title'",
        "[ref]:", "<https://example.org>", "notes.md", "\"a b.md\" 'Caption'", "/rooted/file.txt", "<div>", "</div>",
        "<ul><li>x", "<!-- comment -->", "<!--", "-->", "<script>", "</script>", "<b>bold</b> <i>it", "<br> <img src=x>",
        "    indented", "\tindented", "a\tb", "  two", "   three", "\\", "line  ", "line\\",
        "x^2 y^(a)^ z~1 w~a,b~ ~~s~~ ==h== ~t~", "$a+b$ and $5 or $10", "[%title] [%nope] [%",
        "a[^n] b[^An inline note.] c[^missing]", "[p. 1][#k] [][#k] [@k, p. 2] [@k; @j]", "[a][Label] [Label][] [b](#heading)",
        "**bold _it_** `code` ``a`b``", "[l](u \"t\") ![i](p.png) [r][ref] [ref]", "www.example.org me@example.org http://a.b/c.",
        "&amp; &#35; &nope; \\* \\[", "*a **b* c** _d __e_ f__", "[[[", "]]]", "![", "](", "`", "``", "$", "$$", "^", "~", "~~", "==",
        "\u00e9t\u00e9 \u65e5\u672c\u8a9e \U0001F600 e\u0301", "---", "title: T", "...",
    };
    static const char *const prefixes[] = {"", "", "", "", " ", "  ", "   ", "    ", "> ", ">", "- ", "1. ", "\t", "> - ", "  - ", "    > "};
    const int lineCount = int(std::size(lines));
    const int prefixCount = int(std::size(prefixes));
    quint32 seed = 20261116;
    auto next = [&seed](int bound) {
        seed = seed * 1664525u + 1013904223u;
        return int((seed >> 8) % quint32(bound));
    };
    md::MemoryIncludeResolver files;
    files.add(QStringLiteral("/r/notes.md"), QStringLiteral("# Included [Label]\n\nText[^n] *here*.\n\n[^n]: Included note.\n"));
    files.add(QStringLiteral("/r/a b.md"), QStringLiteral("notes.md\n\n| a |\n| - |\n"));
    md::Source source;
    source.path = QStringLiteral("/r/main.md");
    source.resolver = &files;

    // PARCH_MARKDOWN_RANDOM_DOCUMENTS raises the number for a longer hunt.
    const int documents = qMax(4000, qEnvironmentVariableIntValue("PARCH_MARKDOWN_RANDOM_DOCUMENTS"));
    for (int document = 0; document < documents; ++document) {
        QString text;
        const int count = 1 + next(12);
        for (int i = 0; i < count; ++i) {
            text += QString::fromUtf8(prefixes[next(prefixCount)]);
            text += QString::fromUtf8(lines[next(lineCount)]);
            if (next(8) == 0) text += QString::fromUtf8(lines[next(lineCount)]);
            text += next(10) == 0 ? QStringLiteral("\r\n") : QStringLiteral("\n");
        }
        if (next(4) == 0) text.chop(1);
        for (const md::Options &options : {writer(), standard()}) {
            const md::Document doc = md::parse(text, options, source);
            QStringList errors;
            checkRanges(doc, doc.root, nullptr, errors);
            if (!errors.isEmpty())
                QFAIL(qPrintable(QStringLiteral("document %1 (%2):\n%3\n%4")
                                     .arg(document).arg(options.profileName(), text, errors.mid(0, 4).join(u'\n'))));
            QCOMPARE(doc.root.range, (md::Range{0, int(text.size())}));
            // The safe rendering never carries markup that runs.
            const QString out = md::toHtml(doc);
            QVERIFY2(!out.contains(QLatin1StringView("<script")), qPrintable(text));
            // Every diagnostic points into its file.
            for (const md::Diagnostic &d : doc.diagnostics) {
                QVERIFY(d.file >= 0 && d.file < int(doc.files.size()));
                QVERIFY2(d.range.start >= 0 && d.range.start <= d.range.end
                             && d.range.end <= int(doc.files[d.file].text.size()),
                         qPrintable(text));
            }
            // Prose ranges are in order, and each is text of the file.
            int last = 0;
            for (const md::Range range : doc.proseRanges()) {
                QVERIFY2(range.start >= last && range.end > range.start && range.end <= int(text.size()), qPrintable(text));
                last = range.end;
            }
            // Every position has a path that starts at the document.
            for (int position = 0; position <= int(text.size()); position += 1 + next(7)) {
                const std::vector<const Node *> path = doc.pathAt(position);
                QVERIFY2(!path.empty() && path.front() == &doc.root, qPrintable(text));
                QVERIFY2(position >= path.back()->range.start && position <= path.back()->range.end, qPrintable(text));
            }
            // The outline's nodes are headings with the outline's slugs.
            for (const md::Heading &heading : doc.headings) QVERIFY(heading.node && heading.node->dest == heading.slug);
        }
    }
}

void outlineAndPlainText()
{
    md::MemoryIncludeResolver files;
    files.add(QStringLiteral("/d/chapter.md"), QStringLiteral("## Included *heading*\n\nIncluded text.\n"));
    md::Source source;
    source.path = QStringLiteral("/d/main.md");
    source.resolver = &files;
    const QString text = QStringLiteral(
        "---\ntitle: T\n---\n\n"
        "# First *heading* with `code` [Label]\n\nText.\n\n"
        "Second heading\n--------------\n\n"
        "chapter.md\n\n"
        "### Third [link](u) & more\n\n"
        "###### Sixth\n\n"
        "# First heading with code\n\n"
        "```\n# not a heading\n```\n\n"
        "> ## In a quote\n");
    const md::Document doc = md::parse(text, writer(), source);
    struct Expected {
        int level;
        const char *text;
        const char *slug;
        const char *label;
        int file;
        const char *source;
    };
    const Expected expected[] = {
        {1, "First heading with code", "first-heading-with-code", "Label", 0, "# First *heading* with `code` [Label]"},
        {2, "Second heading", "second-heading", "", 0, "Second heading\n--------------"},
        {2, "Included heading", "included-heading", "", 1, "## Included *heading*"},
        {3, "Third link & more", "third-link-more", "", 0, "### Third [link](u) & more"},
        {6, "Sixth", "sixth", "", 0, "###### Sixth"},
        {1, "First heading with code", "first-heading-with-code-2", "", 0, "# First heading with code"},
        {2, "In a quote", "in-a-quote", "", 0, "## In a quote"},
    };
    QCOMPARE(doc.headings.size(), std::size(expected));
    for (size_t i = 0; i < std::size(expected); ++i) {
        const md::Heading &h = doc.headings[i];
        QCOMPARE(h.level, expected[i].level);
        QCOMPARE(h.text, QString::fromUtf8(expected[i].text));
        QCOMPARE(h.slug, QString::fromUtf8(expected[i].slug));
        QCOMPARE(h.label, QString::fromUtf8(expected[i].label));
        QCOMPARE(h.file, expected[i].file);
        QCOMPARE(doc.source(h.file, h.range).toString(), QString::fromUtf8(expected[i].source));
        // The outline points at the heading's node, whose id is the slug.
        QVERIFY(h.node);
        QCOMPARE(h.node->type, NodeType::Heading);
        QCOMPARE(h.node->dest, h.slug);
        QCOMPARE(h.node->range, h.range);
    }

    // The text a reader sees, a block to a line: no punctuation, front
    // matter, comments, definitions or raw markup.
    const QString prose = QStringLiteral(
        "---\ntitle: T\n---\n\n"
        "# A *title* [Label]\n\n"
        "Some **bold** text with `code`, a [link](https://example.org),\nan ![image alt](p.png) and a note[^n].\n"
        "// a comment\n\n"
        "- one\n- [x] two\n\n"
        "| a | b |\n| - | - |\n| c | d |\n\n"
        "Term\n: Definition\n\n"
        "```\ncode line\n```\n\n"
        "<!-- hidden -->\n\n"
        "$$\nx\n$$\n\n"
        "An inline[^Inline note.] note and [%title].\n\n"
        "[^n]: The note.\n\n"
        "[ref]: /url\n");
    const md::Document plain = md::parse(prose);
    QCOMPARE(plain.plainText(),
             QStringLiteral("A title\n"
                            "Some bold text with code, a link, an image alt and a note.\n"
                            "one\n"
                            "two\n"
                            "a\tb\n"
                            "c\td\n"
                            "Term\n"
                            "Definition\n"
                            "code line\n"
                            "An inline note and T. Inline note.\n"
                            "The note."));
    md::PlainTextOptions proseOnly;
    proseOnly.code = false;
    proseOnly.notes = false;
    QCOMPARE(plain.plainText(proseOnly),
             QStringLiteral("A title\n"
                            "Some bold text with code, a link, an image alt and a note.\n"
                            "one\ntwo\na\tb\nc\td\nTerm\nDefinition\n"
                            "An inline note and T."));
    // Counting words is splitting that text.
    QCOMPARE(plain.plainText(proseOnly).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size(), 28);
    QCOMPARE(md::parse(QString()).plainText(), QString());
    // Within a range, as of a selection: what is written there, by the same
    // rules, a construct cut through giving the part of it inside.
    const auto within = [&](const QString &from, const QString &to) {
        md::PlainTextOptions options;
        const int start = int(prose.indexOf(from));
        const int end = int(prose.indexOf(to, start)) + int(to.size());
        options.range = md::Range{start, end};
        return plain.plainText(options);
    };
    QCOMPARE(within(QStringLiteral("bold**"), QStringLiteral("link](")), QStringLiteral("bold text with code, a link"));
    QCOMPARE(within(QStringLiteral("old**"), QStringLiteral("cod")), QStringLiteral("old text with cod"));
    QCOMPARE(within(QStringLiteral("example.org),"), QStringLiteral("image")), QStringLiteral(", an image"));
    QCOMPARE(within(QStringLiteral("- one"), QStringLiteral("| c |")), QStringLiteral("one\ntwo\na\tb\nc"));
    QCOMPARE(within(QStringLiteral("line\n```"), QStringLiteral("hidden")), QStringLiteral("line"));
    QCOMPARE(within(QStringLiteral("title: T"), QStringLiteral("---")), QString());
    QCOMPARE(within(QStringLiteral("title: T"), QStringLiteral("# A")), QStringLiteral("A"));
    QCOMPARE(within(QStringLiteral("An inline"), QStringLiteral("[ref]: /url")), plain.plainText().section(u'\n', 9));
    md::PlainTextOptions everything;
    everything.range = md::Range{0, int(prose.size())};
    QCOMPARE(plain.plainText(everything), plain.plainText());
    everything.range = md::Range{3, 3};
    QCOMPARE(plain.plainText(everything), QString());
    QCOMPARE(md::plainText(*first(plain, NodeType::Heading)), QStringLiteral("A title"));
    QCOMPARE(md::plainText(*first(plain, NodeType::Image)), QStringLiteral("image alt"));
    QCOMPARE(md::plainText(*first(plain, NodeType::List)), QStringLiteral("one\ntwo"));
}

void positionsAndProse()
{
    const QString text = QStringLiteral(
        "# Title\n"                              // 0
        "\n"
        "Some **bold _both_** text.\n"           // 9
        "\n"
        "- item `code`\n");                      // 37
    const md::Document doc = md::parse(text);
    auto types = [&](int position) {
        QStringList out;
        for (const Node *node : doc.pathAt(position)) out.append(QLatin1StringView(md::typeName(node->type)));
        return out.join(u'/');
    };
    QCOMPARE(types(0), QStringLiteral("document/heading"));
    QCOMPARE(types(2), QStringLiteral("document/heading/text"));
    QCOMPARE(types(8), QStringLiteral("document"));
    QCOMPARE(types(9), QStringLiteral("document/paragraph/text"));
    QCOMPARE(types(14), QStringLiteral("document/paragraph/strong"));
    QCOMPARE(types(16), QStringLiteral("document/paragraph/strong/text"));
    QCOMPARE(types(22), QStringLiteral("document/paragraph/strong/emphasis/text"));
    QCOMPARE(types(37), QStringLiteral("document/list/list-item"));
    QCOMPARE(types(39), QStringLiteral("document/list/list-item/paragraph/text"));
    QCOMPARE(types(46), QStringLiteral("document/list/list-item/paragraph/code"));
    // At the end of a node with nothing after it, the position is the node's.
    QCOMPARE(types(7), QStringLiteral("document/heading/text"));
    QCOMPARE(types(35), QStringLiteral("document/paragraph/text"));
    QCOMPARE(types(int(text.size())), QStringLiteral("document"));
    // Between two nodes the position belongs to the one that starts there.
    QCOMPARE(doc.nodeAt(15)->type, NodeType::Strong);
    QCOMPARE(doc.source(*doc.nodeAt(15)).toString(), QStringLiteral("**bold _both_**"));
    QCOMPARE(doc.nodeAt(22)->text, QStringLiteral("both"));
    QCOMPARE(doc.nodeAt(46)->type, NodeType::Code);
    QVERIFY(doc.pathAt(-1).empty());
    QVERIFY(doc.pathAt(int(text.size()) + 1).empty());
    QVERIFY(!doc.nodeAt(5, 3));

    // In an included file, positions are that file's.
    md::MemoryIncludeResolver files;
    files.add(QStringLiteral("/d/c.md"), QStringLiteral("Included *text*.\n"));
    md::Source source;
    source.path = QStringLiteral("/d/main.md");
    source.resolver = &files;
    const md::Document assembled = md::parse(QStringLiteral("Root.\n\nc.md\n\n{{TOC}}\n\n# H\n"), writer(), source);
    QCOMPARE(assembled.nodeAt(10, 1)->type, NodeType::Text);
    QCOMPARE(assembled.nodeAt(10, 1)->text, QStringLiteral("text"));
    QCOMPARE(assembled.pathAt(10, 1).front()->type, NodeType::Include);
    // From the root, an include is one node, and so is the table of contents.
    QCOMPARE(assembled.nodeAt(8)->type, NodeType::Include);
    QCOMPARE(assembled.nodeAt(15)->type, NodeType::TableOfContents);

    // Prose: what a spellchecker reads. Punctuation, code, math, URLs,
    // comments, front matter, raw markup, labels and keys are left out.
    const QString mixed = QStringLiteral(
        "---\ntitle: Tittle\n---\n\n"
        "# Heding one [Labell]\n\n"
        "Plain **bolld** `codde` $mathh$ [linkk](https://exammple.org \"titl\") ![altt](picc.png)\n"
        "<https://autolinkk.org> www.barelinkk.org me@maill.org \\*esc &amp; [%vaar] x^supp^\n"
        "// commment\n"
        "<b>rawtext</b> <!-- hiddden --> note[^reff] inline[^Notte text.] [p. 1][#keyy]\n\n"
        "```\ncodeblock\n```\n\n"
        "$$\nmathblock\n$$\n\n"
        "<div>\nhtmlblock\n</div>\n\n"
        "| celll | **two** |\n| --- | --- |\n\n"
        "Termm\n: Definitionn\n\n"
        "- [ ] taskk\n\n"
        "notes.md\n\n"
        "[^reff]: Footnotte.\n\n"
        "[#keyy]: Workk.\n\n"
        "[linkdef]: https://linkdeff.org 'Titl'\n");
    const md::Document spell = md::parse(mixed);
    QStringList words;
    int last = -1;
    for (const md::Range range : spell.proseRanges()) {
        QVERIFY(range.start >= last);
        QVERIFY(range.end > range.start);
        last = range.end;
        words.append(mixed.mid(range.start, range.length()).split(QRegularExpression(QStringLiteral("[^\\w]+")), Qt::SkipEmptyParts));
    }
    QCOMPARE(words,
             (QStringList{QStringLiteral("Heding"), QStringLiteral("one"), QStringLiteral("Plain"), QStringLiteral("bolld"),
                          QStringLiteral("linkk"), QStringLiteral("altt"), QStringLiteral("esc"), QStringLiteral("x"),
                          QStringLiteral("supp"), QStringLiteral("rawtext"), QStringLiteral("note"), QStringLiteral("inline"),
                          QStringLiteral("Notte"), QStringLiteral("text"), QStringLiteral("celll"), QStringLiteral("two"),
                          QStringLiteral("Termm"), QStringLiteral("Definitionn"), QStringLiteral("taskk"),
                          QStringLiteral("Footnotte"), QStringLiteral("Workk")}));
    // The same ranges, visited rather than collected.
    int visited = 0;
    spell.forEachProseRange(0, [&](md::Range) { ++visited; });
    QCOMPARE(visited, int(spell.proseRanges().size()));
    // Per file: the root's ranges do not include an included file's text.
    QCOMPARE(assembled.proseRanges(0).size(), size_t(2));
    QCOMPARE(assembled.proseRanges(1).size(), size_t(3));
    QCOMPARE(assembled.source(1, assembled.proseRanges(1).at(1)).toString(), QStringLiteral("text"));
}

void htmlRenderer()
{
    // Escaping, of text and of attributes.
    QCOMPARE(md::escapeHtml(u"<a href=\"x\">&</a>"), QStringLiteral("&lt;a href=&quot;x&quot;&gt;&amp;&lt;/a&gt;"));
    QCOMPARE(md::encodeUrl(u"/a b/\u00e9?q=1&r=[x]#f%20%zz"), QStringLiteral("/a%20b/%C3%A9?q=1&r=%5Bx%5D#f%20%25zz"));
    QCOMPARE(html(QStringLiteral("[a](</p ath> \"t<>&\\\"\")")),
             QStringLiteral("<p><a href=\"/p%20ath\" title=\"t&lt;&gt;&amp;&quot;\">a</a></p>\n"));
    QCOMPARE(html(QStringLiteral("```a\"b\nx\n```")), QStringLiteral("<pre><code class=\"language-a&quot;b\">x\n</code></pre>\n"));

    // Options: heading ids, and the notes and bibliography after the text.
    const QString text = QStringLiteral("# Title\n\nText[^1] and [][#k].\n\n[^1]: Note.\n\n[#k]: Work.\n");
    const md::Document doc = md::parse(text);
    const QString full = md::toHtml(doc);
    QVERIFY(full.startsWith(QLatin1StringView("<h1 id=\"title\">Title</h1>\n")));
    QVERIFY(full.contains(QLatin1StringView("<section class=\"footnotes\">")));
    QVERIFY(full.contains(QLatin1StringView("<section class=\"bibliography\">")));
    QVERIFY(full.indexOf(QLatin1StringView("bibliography")) < full.indexOf(QLatin1StringView("footnotes")));
    md::HtmlOptions bare;
    bare.headingIds = false;
    bare.endnotes = false;
    QCOMPARE(md::toHtml(doc, bare),
             QStringLiteral("<h1>Title</h1>\n<p>Text<sup class=\"footnote-ref\"><a href=\"#fn-1\" id=\"fnref-1\">1</a></sup> and "
                            "<span class=\"citation\">[<a href=\"#cite-1\">1</a>]</span>.</p>\n"));
    // Rendering twice gives the same result: the writer keeps no state in the document.
    QCOMPARE(md::toHtml(doc), full);

    // A note whose last block is not a paragraph gets its way back on a line of its own.
    QVERIFY(html(QStringLiteral("a[^1]\n\n[^1]: Text\n\n    - item\n"))
                .contains(QStringLiteral("</ul>\n<a href=\"#fnref-1\" class=\"footnote-backref\">\u21a9</a>\n</li>")));
    // A bibliography entry of several paragraphs keeps them.
    QVERIFY(html(QStringLiteral("a[][#k]\n\n[#k]: One.\n\n    Two.\n"))
                .contains(QLatin1StringView("<li id=\"cite-1\">\n<p>One.</p>\n<p>Two.</p>\n</li>")));
    // An empty document is an empty string.
    QCOMPARE(md::toHtml(md::parse(QString())), QString());
    QCOMPARE(md::toHtml(md::parse(QStringLiteral("\n\n  \n"))), QString());
}

void diagnostics()
{
    md::MemoryIncludeResolver files;
    files.add(QStringLiteral("/d/loop.md"), QStringLiteral("loop.md\n"));
    md::Source source;
    source.path = QStringLiteral("/d/main.md");
    source.resolver = &files;
    const QString text = QStringLiteral(
        "---\nkey: !tag value\n---\n\n"
        "# Twin\n\n# Twin\n\n"
        "[a][Missing] [b][Twin] [c](#nowhere) [^nonote] [%novar] [][#nokey] <blink>x</blink>\n\n"
        "| a | b |\n| - | - |\n| 1 || 2 | 3 |\n\n"
        "$$\nunclosed\n\n"
        "[^dup]: one\n[^dup]: two\n\n"
        "[#dup]: one\n[#dup]: two\n\n"
        "missing.md\n\n"
        "loop.md\n\n"
        "/rooted.md\n");
    const md::Document doc = md::parse(text, writer(), source);
    struct Expected {
        DiagnosticCode code;
        md::Severity severity;
        const char *source;
    };
    const Expected expected[] = {
        {DiagnosticCode::FrontMatter, md::Severity::Warning, ""},
        {DiagnosticCode::UnresolvedReference, md::Severity::Warning, "[a][Missing]"},
        {DiagnosticCode::AmbiguousHeadingReference, md::Severity::Warning, "[b][Twin]"},
        {DiagnosticCode::UnresolvedAnchor, md::Severity::Warning, "[c](#nowhere)"},
        {DiagnosticCode::MissingFootnote, md::Severity::Warning, "[^nonote]"},
        {DiagnosticCode::UnresolvedVariable, md::Severity::Warning, "[%novar]"},
        {DiagnosticCode::MissingCitation, md::Severity::Warning, "[#nokey]"},
        {DiagnosticCode::UnsupportedHtml, md::Severity::Info, "<blink>"},
        {DiagnosticCode::UnsupportedHtml, md::Severity::Info, "</blink>"},
        {DiagnosticCode::MalformedTableRow, md::Severity::Warning, "| 1 || 2 | 3 |"},
        {DiagnosticCode::UnclosedMath, md::Severity::Warning, "$$"},
        {DiagnosticCode::UnusedFootnote, md::Severity::Info, "[^dup]:"},
        {DiagnosticCode::DuplicateFootnote, md::Severity::Warning, "[^dup]:"},
        {DiagnosticCode::DuplicateCitation, md::Severity::Warning, "[#dup]:"},
        {DiagnosticCode::IncludeNotFound, md::Severity::Error, "missing.md"},
        {DiagnosticCode::IncludeNoLibrary, md::Severity::Error, "/rooted.md"},
        // In the included file.
        {DiagnosticCode::IncludeCycle, md::Severity::Error, "loop.md"},
    };
    QStringList got;
    for (const md::Diagnostic &d : doc.diagnostics)
        got.append(QStringLiteral("%1 %2 \"%3\" %4").arg(int(d.code)).arg(d.file).arg(doc.source(d.file, d.range)).arg(d.message));
    QVERIFY2(doc.diagnostics.size() == std::size(expected), qPrintable(got.join(u'\n')));
    for (size_t i = 0; i < std::size(expected); ++i) {
        const md::Diagnostic &d = doc.diagnostics[i];
        QVERIFY2(d.code == expected[i].code, qPrintable(got.join(u'\n')));
        QCOMPARE(d.severity, expected[i].severity);
        QCOMPARE(doc.source(d.file, d.range).toString(), QString::fromUtf8(expected[i].source));
        QVERIFY(!d.message.isEmpty());
        QCOMPARE(d.file, i + 1 == std::size(expected) ? 1 : 0);
        // A message describes the problem; it does not carry a path of this machine.
        QVERIFY(!d.message.contains(QLatin1StringView("/home/")));
    }
    QVERIFY(doc.incomplete);
    // Codes and severities have stable names for reports, all different.
    QSet<QString> names;
    for (int code = int(DiagnosticCode::UnresolvedReference); code <= int(DiagnosticCode::IncludeNoBaseFolder); ++code)
        names.insert(QLatin1StringView(md::codeName(DiagnosticCode(code))));
    QCOMPARE(names.size(), int(DiagnosticCode::IncludeNoBaseFolder) + 1);
    QVERIFY(!names.contains(QStringLiteral("unknown")));
    QCOMPARE(QLatin1StringView(md::codeName(DiagnosticCode::IncludeCycle)), QLatin1StringView("include-cycle"));
    QCOMPARE(QLatin1StringView(md::severityName(md::Severity::Error)), QLatin1StringView("error"));
    QCOMPARE(QLatin1StringView(md::severityName(md::Severity::Info)), QLatin1StringView("info"));
    // Every construct that could not be resolved is still in the output.
    const QString out = md::toHtml(doc);
    for (const char *visible : {"[a][Missing]", "[^nonote]", "[%novar]", "[][#nokey]", "&lt;blink&gt;x&lt;/blink&gt;", "$$\nunclosed",
                                "<p>missing.md</p>", "<p>loop.md</p>", "<p>/rooted.md</p>"})
        QVERIFY2(out.contains(QLatin1StringView(visible)), visible);

    // A clean document reports nothing.
    for (const md::Options &options : {writer(), standard()})
        QVERIFY(md::parse(QStringLiteral("# Title\n\nPlain *text* with a [link](u), `code` and [brackets].\n"), options)
                    .diagnostics.empty());
}

void parsesOnAnyThread()
{
    // The parser is a pure function: many threads at once, each on its own
    // text, give what one thread gives.
    QStringList texts{mixedDocument(), proseDocument(3000)};
    const QJsonArray examples = QJsonDocument::fromJson(readFixture("commonmark-0.31.2.json")).array();
    QString spec;
    for (const QJsonValue &value : examples) spec += value.toObject()[QLatin1StringView("markdown")].toString() + u'\n';
    texts.append(spec);
    QStringList expected;
    for (const QString &text : std::as_const(texts)) expected.append(md::toHtml(md::parse(text)));

    const int threadCount = 8;
    std::vector<QStringList> results(threadCount);
    std::vector<std::thread> threads;
    for (int t = 0; t < threadCount; ++t) {
        threads.emplace_back([&, t] {
            for (int round = 0; round < 3; ++round)
                for (const QString &text : std::as_const(texts)) results[t].append(md::toHtml(md::parse(text)));
        });
    }
    for (std::thread &thread : threads) thread.join();
    for (int t = 0; t < threadCount; ++t) {
        QCOMPARE(results[t].size(), expected.size() * 3);
        for (int i = 0; i < results[t].size(); ++i) QVERIFY(results[t][i] == expected[i % expected.size()]);
    }

    // A document built on one thread is read on another.
    md::Document moved = md::parse(mixedDocument());
    const QString here = md::toHtml(moved);
    QString there;
    int headings = 0;
    std::thread reader([&, doc = std::move(moved)] {
        there = md::toHtml(doc);
        for (const md::Heading &h : doc.headings) headings += h.node && h.node->type == NodeType::Heading;
    });
    reader.join();
    QCOMPARE(there, here);
    QCOMPARE(headings, 2);
}

void performance()
{
    // Ordinary prose: 10,000 words in under 10 ms and 100,000 in under 100 ms.
    struct Budget {
        int words;
        double milliseconds;
    };
    for (const Budget budget : {Budget{10000, 10.0}, Budget{100000, 100.0}}) {
        const QString text = proseDocument(budget.words);
        const int words = int(text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size());
        QVERIFY(words >= budget.words);
        double best = 1e9;
        double total = 0;
        const int runs = 7;
        size_t nodes = 0;
        for (int run = 0; run < runs; ++run) {
            QElapsedTimer timer;
            timer.start();
            const md::Document doc = md::parse(text);
            const double elapsed = timer.nsecsElapsed() / 1e6;
            best = qMin(best, elapsed);
            total += elapsed;
            nodes = doc.root.children.size();
        }
        QVERIFY(nodes > 0);
        qInfo("Markdown parse of %d words (%lld UTF-16 units): best %.2f ms, mean %.2f ms over %d runs; budget %.0f ms",
              words, qlonglong(text.size()), best, total / runs, runs, budget.milliseconds);
        QVERIFY2(best < budget.milliseconds,
                 qPrintable(QStringLiteral("%1 words took %2 ms").arg(words).arg(best, 0, 'f', 2)));
    }
}

void pathologicalInput()
{
    // Input built to make a careless parser quadratic or recursive. Each
    // must be read in time that grows about linearly, without overflowing
    // the stack, and the tree must stay shallow enough to walk.
    struct Input {
        QString name;
        QString text;
    };
    auto repeat = [](const QString &unit, int count) { return unit.repeated(count); };
    std::vector<Input> inputs;
    auto add = [&](const char *name, const QString &text) { inputs.push_back({QLatin1StringView(name), text}); };
    const int n = 10000;
    add("open brackets", repeat(QStringLiteral("["), n));
    add("close brackets", repeat(QStringLiteral("]"), n));
    add("bracket pairs", repeat(QStringLiteral("["), n) + repeat(QStringLiteral("]"), n));
    add("nested links", repeat(QStringLiteral("[a"), n) + repeat(QStringLiteral("](u)"), n));
    add("nested images", repeat(QStringLiteral("![a"), n) + repeat(QStringLiteral("](u)"), n));
    add("unclosed inline links", repeat(QStringLiteral("[a](b"), n));
    add("unclosed angle links", repeat(QStringLiteral("[a](<b"), n));
    add("unclosed titles", repeat(QStringLiteral("[a](b \"c "), n));
    add("reference openers", repeat(QStringLiteral("[a]["), n));
    add("many undefined references", repeat(QStringLiteral("[a][b] "), n));
    add("many definitions", repeat(QStringLiteral("[a]: /b\n"), n) + repeat(QStringLiteral("[a] "), n));
    add("note openers", repeat(QStringLiteral("[^"), n));
    add("nested notes", repeat(QStringLiteral("[^a "), n) + repeat(QStringLiteral("]"), n));
    add("many notes", repeat(QStringLiteral("x[^n] "), n) + QStringLiteral("\n\n[^n]: note\n"));
    add("asterisks", repeat(QStringLiteral("*"), n));
    add("underscores", repeat(QStringLiteral("_"), n));
    add("emphasis openers", repeat(QStringLiteral("*a "), n));
    add("emphasis closers", repeat(QStringLiteral("a* "), n));
    add("mixed emphasis openers", repeat(QStringLiteral("**a *a _a __a "), n / 2));
    add("nested emphasis", repeat(QStringLiteral("*a **b "), n / 2) + repeat(QStringLiteral("b** a* "), n / 2));
    add("intraword underscores", repeat(QStringLiteral("a_b_"), n));
    add("tildes", repeat(QStringLiteral("~"), n) + QStringLiteral(" ") + repeat(QStringLiteral("~~a "), n));
    add("subscripts", repeat(QStringLiteral("x~"), n) + repeat(QStringLiteral("x^"), n));
    add("equals", repeat(QStringLiteral("==a "), n) + repeat(QStringLiteral("="), n));
    add("dollars", repeat(QStringLiteral("$"), n) + QStringLiteral(" ") + repeat(QStringLiteral("$a "), n) + repeat(QStringLiteral("$$a "), n));
    add("backticks", repeat(QStringLiteral("`a"), n));
    {
        QString text;
        for (int i = 1; i <= 2000; ++i) text += QString(i % 300 + 1, u'`') + u' ';
        add("backtick runs of every length", text);
    }
    add("angle brackets", repeat(QStringLiteral("<"), n) + repeat(QStringLiteral(">"), n));
    add("unclosed comments", repeat(QStringLiteral("a <!-- b "), n));
    add("unclosed instructions", repeat(QStringLiteral("a <?b "), n) + repeat(QStringLiteral("a <!B "), n) + repeat(QStringLiteral("<![CDATA[ "), n));
    add("unclosed tags", repeat(QStringLiteral("<a href=\"b "), n));
    add("unpaired markup", repeat(QStringLiteral("<b>"), n) + repeat(QStringLiteral("</i>"), n));
    add("nested markup", repeat(QStringLiteral("<b>"), n) + QStringLiteral("x") + repeat(QStringLiteral("</b>"), n));
    add("nested markup block", QStringLiteral("<div>") + repeat(QStringLiteral("<ul><li>"), n));
    add("entities", repeat(QStringLiteral("&"), n) + repeat(QStringLiteral("&amp;"), n) + repeat(QStringLiteral("&#x;"), n));
    add("backslashes", repeat(QStringLiteral("\\"), n) + repeat(QStringLiteral("\\*"), n));
    add("bare links", repeat(QStringLiteral("www.a.b "), n) + repeat(QStringLiteral("http://a "), n) + repeat(QStringLiteral("a@b.c "), n));
    add("at signs", repeat(QStringLiteral("@"), n) + repeat(QStringLiteral("a@"), n));
    add("variables and citations", repeat(QStringLiteral("[%a"), n) + repeat(QStringLiteral("[@a"), n) + repeat(QStringLiteral("[%a] [@b] "), n));
    add("one long line", repeat(QStringLiteral("word "), 200000));
    add("one long word", repeat(QStringLiteral("a"), 1000000));
    add("many short lines", repeat(QStringLiteral("a\n"), 200000));
    add("many blank lines", repeat(QStringLiteral("\n"), 500000));
    add("nul characters", repeat(QString(QChar(0)), n) + QStringLiteral("\n\n*a") + QChar(0) + QStringLiteral("b*"));
    {
        // 1,000 quotes nested one inside the next, on one line and on many.
        add("nested quotes on one line", repeat(QStringLiteral("> "), 1000) + QStringLiteral("deep\n"));
        QString text;
        for (int i = 1; i <= 1000; ++i) text += repeat(QStringLiteral("> "), i) + QStringLiteral("line\n");
        add("nested quotes, one more each line", text);
        add("far too many quotes", repeat(QStringLiteral(">"), 200000));
    }
    {
        QString text;
        for (int i = 0; i < 2000; ++i) text += QString(i * 2, u' ') + QStringLiteral("- a\n");
        add("nested lists", text);
        add("list markers on one line", repeat(QStringLiteral("- "), 50000) + QStringLiteral("a"));
        add("ordered markers on one line", repeat(QStringLiteral("1. "), 50000) + QStringLiteral("a"));
    }
    add("many headings with a table of contents", QStringLiteral("{{TOC}}\n\n") + repeat(QStringLiteral("# Same heading\n\n"), n));
    add("many heading references", QStringLiteral("# H [L]\n\n") + repeat(QStringLiteral("[a][L] [b][H] [c][X] "), n));
    add("table with many cells", repeat(QStringLiteral("|a"), n) + QStringLiteral("\n") + repeat(QStringLiteral("|-"), n) + QStringLiteral("\n")
                                    + repeat(QStringLiteral("|`"), n) + QStringLiteral("\n") + repeat(QStringLiteral("||"), n));
    add("table with many rows", QStringLiteral("|a|b|\n|-|-|\n") + repeat(QStringLiteral("|c||d|e\n"), n));
    add("definition markers", QStringLiteral("Term\n") + repeat(QStringLiteral(": a\n"), n) + repeat(QStringLiteral(": "), n));
    add("math blocks never closed", repeat(QStringLiteral("$$\na\n"), n));
    add("fences never closed", repeat(QStringLiteral("```\n"), 3) + repeat(QStringLiteral("~~~~\n```\n"), n));
    add("comment lines", repeat(QStringLiteral("// a\n"), n) + repeat(QStringLiteral("text\n// a\n"), n));
    add("page breaks", repeat(QStringLiteral("+++\n\n"), n));
    {
        QString text = QStringLiteral("---\n");
        for (int i = 0; i < 3000; ++i) text += QString(i, u' ') + QStringLiteral("k:\n");
        text += QStringLiteral("flow: ") + repeat(QStringLiteral("["), 5000) + QStringLiteral("\n---\nbody\n");
        add("deep front matter", text);
    }

    for (const Input &input : inputs) {
        for (const md::Options &options : {writer(), standard()}) {
            QElapsedTimer timer;
            timer.start();
            const md::Document doc = md::parse(input.text, options);
            const QString out = md::toHtml(doc);
            const qint64 elapsed = timer.elapsed();
            QVERIFY2(elapsed < timeLimit(3000), qPrintable(QStringLiteral("%1 took %2 ms").arg(input.name).arg(elapsed)));
            // Shallow enough for any recursive reader.
            const int depth = treeDepth(doc.root);
            QVERIFY2(depth < 1200, qPrintable(QStringLiteral("%1 is %2 deep").arg(input.name).arg(depth)));
            QCOMPARE(doc.root.range, (md::Range{0, int(input.text.size())}));
            if (input.text.size() < 300000) {
                QStringList errors;
                checkRanges(doc, doc.root, nullptr, errors);
                QVERIFY2(errors.isEmpty(), qPrintable(input.name + u'\n' + errors.mid(0, 5).join(u'\n')));
                doc.proseRanges();
                doc.plainText();
                doc.nodeAt(int(input.text.size() / 2));
            }
            Q_UNUSED(out);
        }
    }

    // Linear, not merely bounded: ten times the input takes about ten times
    // as long, far from the hundred times of a quadratic parser.
    auto seconds = [](const QString &text) {
        double best = 1e9;
        for (int run = 0; run < 5; ++run) {
            QElapsedTimer timer;
            timer.start();
            md::parse(text);
            best = qMin(best, timer.nsecsElapsed() / 1e9);
        }
        return best;
    };
    for (const QString &unit : {QStringLiteral("["), QStringLiteral("*a "), QStringLiteral("[a](b"), QStringLiteral("`a"),
                                QStringLiteral("a <!-- b "), QStringLiteral("[^a "), QStringLiteral("$a ")}) {
        const double small = seconds(repeat(unit, 20000));
        const double large = seconds(repeat(unit, 200000));
        QVERIFY2(large < qMax(small, 0.0005) * 40,
                 qPrintable(QStringLiteral("\"%1\": %2 ms for 20,000 and %3 ms for 200,000").arg(unit).arg(small * 1e3).arg(large * 1e3)));
    }

    // 1,000 nested quotes are 1,000 quotes; beyond the limit the rest is text.
    const md::Document deep = md::parse(repeat(QStringLiteral("> "), 1000) + QStringLiteral("deep\n"));
    QCOMPARE(all(deep, NodeType::BlockQuote).size(), size_t(1000));
    QCOMPARE(all(deep, NodeType::Text).back()->text, QStringLiteral("deep"));
    const md::Document deeper = md::parse(repeat(QStringLiteral(">"), 5000) + QStringLiteral(" deep\n"));
    QVERIFY(all(deeper, NodeType::BlockQuote).size() <= size_t(1000));
    QVERIFY(md::toHtml(deeper).contains(QLatin1StringView("deep")));
    // Emphasis nested past its limit keeps its punctuation as text.
    const QString nestedEmphasis = repeat(QStringLiteral("*a "), 200) + repeat(QStringLiteral("a* "), 200);
    const md::Document emphasis = md::parse(nestedEmphasis);
    QVERIFY(treeDepth(emphasis.root) < 100);
    QCOMPARE(md::plainText(*first(emphasis, NodeType::Paragraph)).count(u'a'), 400);
}

} // namespace MarkdownTests
