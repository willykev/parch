#include "citationtests.h"

#include "bibtex.h"
#include "citations.h"
#include "cslengine.h"
#include "cslfixture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>
#include <QtConcurrent>
#include <QtTest>

namespace {

using Database = std::shared_ptr<const BibliographyDatabase>;

// Formatted text written the way the expectations below are: *italic*,
// **bold**, <sc>small caps</sc>, ^superscript^, and a space between a
// bibliography's display blocks ("[1] Author...").
QString marked(const CslText &text)
{
    QString out;
    CslFormat::Display last = CslFormat::Display::Inline;
    bool first = true;
    for (const CslSpan &span : text.spans()) {
        if (!first && span.format.display != last && !out.endsWith(u' ')) out += u' ';
        QString piece = span.text;
        if (span.format.italic) piece = u'*' + piece + u'*';
        if (span.format.bold) piece = QStringLiteral("**") + piece + QStringLiteral("**");
        if (span.format.smallCaps) piece = QStringLiteral("<sc>") + piece + QStringLiteral("</sc>");
        if (span.format.position == CslFormat::Position::Superscript) piece = u'^' + piece + u'^';
        out += piece;
        last = span.format.display;
        first = false;
    }
    return out;
}

Database database(const QByteArray &data, BibliographyDatabase::Format format = BibliographyDatabase::Format::CslJson,
                  const QString &identity = QStringLiteral("references.json"))
{
    return std::make_shared<const BibliographyDatabase>(BibliographyDatabase::fromData(data, format, identity));
}

CitationItemSpec cite(const QString &key, const QString &locator = {}, const QString &prefix = {},
                      const QString &suffix = {}, CslCite::Mode mode = CslCite::Mode::Normal)
{
    CitationItemSpec item;
    item.key = key;
    item.locator = locator;
    item.prefix = prefix;
    item.suffix = suffix;
    item.mode = mode;
    return item;
}

CitationClusterSpec cluster(const QList<CitationItemSpec> &items, int note = 0, const QString &file = {})
{
    CitationClusterSpec spec;
    spec.items = items;
    spec.noteIndex = note;
    spec.sourceFile = file;
    return spec;
}

struct Formatted {
    QStringList citations;
    QStringList bibliography;
    CitationOutput output;
};

Formatted format(const std::shared_ptr<const CslStyle> &style, const QList<Database> &databases,
                 const QList<CitationClusterSpec> &clusters, const QList<WriterDefinition> &definitions = {})
{
    CitationFormatter formatter;
    formatter.setStyle(style);
    formatter.setDatabases(databases);
    Formatted result;
    result.output = formatter.format(clusters, definitions);
    for (const CslText &text : std::as_const(result.output.clusters)) result.citations.append(marked(text));
    for (const CitationBibliographyEntry &entry : std::as_const(result.output.bibliography))
        result.bibliography.append(marked(entry.text));
    return result;
}

Formatted formatBundled(const QString &styleId, const QList<Database> &databases,
                        const QList<CitationClusterSpec> &clusters, const QList<WriterDefinition> &definitions = {})
{
    QString error;
    const std::shared_ptr<const CslStyle> style = CslCatalog::bundledStyle(styleId, &error);
    if (!style) qWarning("%s", qPrintable(error));
    return format(style, databases, clusters, definitions);
}

// The works every style test cites, in CSL JSON. Titles are in sentence
// case, as CSL expects them to be stored; styles that want title case
// convert them. What the guides word differently (a thesis's description,
// a report's kind, the form of a place) is filled in per style.
QByteArray works(const QHash<QString, QString> &fill = {})
{
    QHash<QString, QString> values{
        {QStringLiteral("BOOK_A"), QStringLiteral("Writing with examples")},
        {QStringLiteral("BOOK_B"), QStringLiteral("Writing without examples")},
        {QStringLiteral("BOOK_PLACE"), QStringLiteral("New York, NY, USA")},
        {QStringLiteral("CONTAINER"), QStringLiteral("Essays on writing")},
        {QStringLiteral("CHAPTER_PLACE"), QStringLiteral("Chicago, IL, USA")},
        {QStringLiteral("THESIS_GENRE"), QStringLiteral("Doctoral dissertation")},
        {QStringLiteral("THESIS_PLACE"), QStringLiteral("Austin, TX, USA")},
        {QStringLiteral("REPORT_GENRE"), QStringLiteral("Report")},
        {QStringLiteral("REPORT_PLACE"), QStringLiteral("Boston, MA, USA")},
        {QStringLiteral("SITE"), QStringLiteral("Example Writing Center")},
    };
    for (auto it = fill.constBegin(); it != fill.constEnd(); ++it) values.insert(it.key(), it.value());
    QString json = QStringLiteral(R"([
 {"id": "doe2006a", "type": "book", "author": [{"family": "Doe", "given": "John"}],
  "title": "BOOK_A", "publisher": "Example Press", "publisher-place": "BOOK_PLACE",
  "issued": {"date-parts": [[2006]]}},
 {"id": "doe2006b", "type": "book", "author": [{"family": "Doe", "given": "John"}],
  "title": "BOOK_B", "publisher": "Example Press", "publisher-place": "BOOK_PLACE",
  "issued": {"date-parts": [[2006]]}},
 {"id": "smith2019", "type": "article-journal",
  "author": [{"family": "Smith", "given": "Anna"}, {"family": "Jones", "given": "Brian"},
             {"family": "Brown", "given": "Carol"}],
  "title": "Measuring the quiet: A field study", "title-short": "Measuring the quiet",
  "container-title": "Journal of Examples",
  "container-title-short": "J. Examples", "volume": "12", "issue": "3", "page": "45-67",
  "issued": {"date-parts": [[2019]]}, "DOI": "10.1234/joe.2019.003"},
 {"id": "lee2015", "type": "chapter", "author": [{"family": "Lee", "given": "Mina"}],
  "title": "Notes on the margins", "container-title": "CONTAINER",
  "editor": [{"family": "Grant", "given": "Paul"}, {"family": "Hall", "given": "Rosa"}],
  "publisher": "Example University Press", "publisher-place": "CHAPTER_PLACE", "page": "101-120",
  "issued": {"date-parts": [[2015]]}},
 {"id": "park2021", "type": "webpage", "author": [{"family": "Park", "given": "Dana"}],
  "title": "How citations work", "container-title": "SITE",
  "issued": {"date-parts": [[2021, 3, 14]]}, "accessed": {"date-parts": [[2024, 1, 5]]},
  "URL": "https://example.org/citations"},
 {"id": "garcia2020", "type": "report", "author": [{"family": "Garcia", "given": "Luis"}],
  "title": "Annual report on examples", "genre": "REPORT_GENRE", "number": "17",
  "publisher": "Example Institute", "publisher-place": "REPORT_PLACE", "issued": {"date-parts": [[2020]]}},
 {"id": "kim2018", "type": "thesis", "author": [{"family": "Kim", "given": "Soo"}],
  "title": "Patterns of revision in student writing", "genre": "THESIS_GENRE",
  "publisher": "Example University", "publisher-place": "THESIS_PLACE", "issued": {"date-parts": [[2018]]}},
 {"id": "adams2022", "type": "article-journal",
  "author": [{"family": "Adams", "given": "Alice"}, {"family": "Baker", "given": "Ben"},
             {"family": "Clark", "given": "Cora"}, {"family": "Davis", "given": "Dan"},
             {"family": "Evans", "given": "Eve"}, {"family": "Fox", "given": "Finn"},
             {"family": "Green", "given": "Gail"}, {"family": "Hill", "given": "Hugo"}],
  "title": "Collective writing at scale", "container-title": "Collaboration Studies",
  "volume": "4", "issue": "1", "page": "1-19", "issued": {"date-parts": [[2022]]}}
])");
    // Longer names first, so that BOOK_A is not taken for BOOK_PLACE's start.
    QStringList keys = values.keys();
    std::sort(keys.begin(), keys.end(), [](const QString &a, const QString &b) { return a.size() > b.size(); });
    for (const QString &key : keys) json.replace(key, values.value(key));
    return json.toUtf8();
}

// The citations most style tests make, in this order.
QList<CitationClusterSpec> authorDateCitations()
{
    return {
        cluster({cite(QStringLiteral("doe2006a"), QStringLiteral("p. 23"))}),
        cluster({cite(QStringLiteral("smith2019"))}),
        cluster({cite(QStringLiteral("lee2015"), QStringLiteral("chap. 2"), QStringLiteral("see")),
                 cite(QStringLiteral("park2021"))}),
        cluster({cite(QStringLiteral("doe2006a")), cite(QStringLiteral("doe2006b"))}),
        cluster({cite(QStringLiteral("doe2006b"), {}, {}, {}, CslCite::Mode::AuthorInText)}),
        cluster({cite(QStringLiteral("kim2018"), {}, {}, {}, CslCite::Mode::SuppressAuthor)}),
        cluster({cite(QStringLiteral("adams2022"), QStringLiteral("pp. 3-5"))}),
        cluster({cite(QStringLiteral("garcia2020"))}),
        // "[@smith2019, p. 50, emphasis added]": a locator and a suffix.
        cluster({cite(QStringLiteral("smith2019"), QStringLiteral("p. 50, emphasis added"))}),
    };
}

// The fixtures are downloaded by bin/fetch-test-fixtures, checked against
// the list beside the known failures; they are not kept in the repository.
QString suiteFolder()
{
    return QStringLiteral(PARCH_SOURCE_DIR "/build/csl-suite");
}

QString suiteListFolder()
{
    return QStringLiteral(PARCH_SOURCE_DIR "/tests/csl-suite");
}

} // namespace

void CitationTests::richTextOutput()
{
    CslText text;
    text.append(QStringLiteral("Doe & <Roe>, "));
    CslFormat italic;
    italic.italic = true;
    text.append(QStringLiteral("A \"Title\""), italic);
    CslFormat both = italic;
    both.bold = true;
    text.append(QStringLiteral(" bold"), both);
    text.append(QStringLiteral(" again"), italic);
    CslFormat linked;
    linked.link = QStringLiteral("https://example.org/?a=1&b=2");
    text.append(QStringLiteral(" link"), linked);
    // Runs of one format merge; every character is escaped.
    QCOMPARE(text.spans().size(), 5);
    QCOMPARE(text.toPlainText(), QStringLiteral("Doe & <Roe>, A \"Title\" bold again link"));
    QCOMPARE(text.toHtml(), QStringLiteral("Doe &amp; &lt;Roe&gt;, <i>A &quot;Title&quot;<b> bold</b> again</i>"
                                           "<a href=\"https://example.org/?a=1&amp;b=2\"> link</a>"));
    // The HTML reads back into the same runs.
    QCOMPARE(CslText::fromHtml(text.toHtml()), text);
    // Small caps, superscripts and display blocks.
    CslText entry;
    CslFormat margin;
    margin.display = CslFormat::Display::LeftMargin;
    entry.append(QStringLiteral("[1]"), margin);
    CslFormat body;
    body.display = CslFormat::Display::RightInline;
    body.smallCaps = true;
    entry.append(QStringLiteral("Doe"), body);
    body.smallCaps = false;
    body.position = CslFormat::Position::Superscript;
    entry.append(QStringLiteral("2"), body);
    QCOMPARE(entry.toHtml(), QStringLiteral("<div class=\"csl-left-margin\">[1]</div><div class=\"csl-right-inline\">"
                                            "<span style=\"font-variant:small-caps;\">Doe</span><sup>2</sup></div>"));
    QCOMPARE(CslText::fromHtml(entry.toHtml()), entry);
    // CSL JSON's markup and entities, and text that only looks like markup.
    const CslText data = CslText::fromHtml(
        QStringLiteral("<i>Homo</i> <span class=\"nocase\">sapiens</span> &amp; <sc>caps</sc> 1 < 2 &#38; AT&T"));
    QCOMPARE(data.toPlainText(), QStringLiteral("Homo sapiens & caps 1 < 2 & AT&T"));
    QVERIFY(data.spans().first().format.italic);
}

void CitationTests::bibtexSyntax()
{
    const QString bib = QStringLiteral(R"(
@string{press = "Example Press"}
@STRING{ny = {New York}}
@comment{ This @book{ignored, title = {Not an entry}} is a comment }
@preamble{ "\newcommand{\noop}[1]{}" }
Text outside entries is ignored, as is an address like a@b.org.

@Book{doe2006,
  author    = {Doe, John},
  title     = "Writing {with} Examples",
  publisher = press,
  address   = ny # ", NY",
  year      = 2006,
  month     = mar,
  note      = {See "the {notes}" here},
}

@article(smith2019,
  author = {Anna Smith and Brian Jones},
  title = {Measuring the Quiet},
  journal = {Journal of Examples},
  volume = 12, number = {3}, pages = {45--67},
  year = {2019}
)
)");
    const BibTeX::Result result = BibTeX::parse(bib);
    QVERIFY2(result.diagnostics.isEmpty(), qPrintable(result.diagnostics.value(0).message));
    QCOMPARE(result.entries.size(), 2);
    const CslReference &doe = result.entries[0].reference;
    QCOMPARE(doe.id, QStringLiteral("doe2006"));
    QCOMPARE(doe.type, QStringLiteral("book"));
    QCOMPARE(result.entries[0].line, 8);
    QCOMPARE(doe.field(QStringLiteral("publisher")), QStringLiteral("Example Press"));
    QCOMPARE(doe.field(QStringLiteral("publisher-place")), QStringLiteral("New York, NY"));
    // A title case title becomes sentence case; braces keep their case.
    QCOMPARE(doe.field(QStringLiteral("title")), QStringLiteral("Writing <span class=\"nocase\">with</span> examples"));
    QCOMPARE(doe.field(QStringLiteral("note")), QStringLiteral("See \"the notes\" here"));
    QCOMPARE(doe.dates.value(QStringLiteral("issued")).start, (CslDatePoint{2006, 3, 0, 2}));
    QCOMPARE(doe.names.value(QStringLiteral("author")).size(), 1);
    QCOMPARE(doe.names.value(QStringLiteral("author")).first().family, QStringLiteral("Doe"));

    const CslReference &smith = result.entries[1].reference;
    QCOMPARE(smith.type, QStringLiteral("article-journal"));
    QCOMPARE(smith.field(QStringLiteral("title")), QStringLiteral("Measuring the quiet"));
    QCOMPARE(smith.field(QStringLiteral("container-title")), QStringLiteral("Journal of Examples"));
    QCOMPARE(smith.field(QStringLiteral("volume")), QStringLiteral("12"));
    QCOMPARE(smith.field(QStringLiteral("issue")), QStringLiteral("3"));
    QCOMPARE(smith.field(QStringLiteral("page")), QStringLiteral("45–67"));
    QCOMPARE(smith.names.value(QStringLiteral("author")).size(), 2);
    QCOMPARE(smith.names.value(QStringLiteral("author"))[1].given, QStringLiteral("Brian"));
}

void CitationTests::bibtexLatex()
{
    const QList<QPair<QString, QString>> cases{
        {QStringLiteral("{\\'E}mile Zola"), QStringLiteral("Émile Zola")},
        {QStringLiteral("Schr\\\"{o}dinger and G\\\"odel"), QStringLiteral("Schrödinger and Gödel")},
        {QStringLiteral("Stra\\ss{}e, Gar\\c{c}on, \\v{S}koda, {\\aa}ngstr\\\"om"),
         QStringLiteral("Straße, Garçon, Škoda, ångström")},
        {QStringLiteral("{\\o}re, \\l{}\\'od\\'z, na\\\"{\\i}ve, Ma\\~nana"), QStringLiteral("øre, łódź, naïve, Mañana")},
        {QStringLiteral("cr\\^epe, d\\`a, Erd\\H{o}s, \\k{a}, \\r{u}, \\=a, \\.z, \\u{g}"),
         QStringLiteral("crêpe, dà, Erdős, ą, ů, ā, ż, ğ")},
        {QStringLiteral("\\emph{Italic} and \\textbf{bold} and {\\em emphasis}"),
         QStringLiteral("<i>Italic</i> and <b>bold</b> and <i>emphasis</i>")},
        {QStringLiteral("\\textsc{Small} H\\textsubscript{2}O x\\textsuperscript{2}"),
         QStringLiteral("<span style=\"font-variant:small-caps;\">Small</span> H<sub>2</sub>O x<sup>2</sup>")},
        {QStringLiteral("10\\% \\& \\$5 \\#1 a\\_b"), QStringLiteral("10% &amp; $5 #1 a_b")},
        {QStringLiteral("pages 1--2 and 3---4"), QStringLiteral("pages 1–2 and 3—4")},
        {QStringLiteral("``quoted'' and a~tie"), QString::fromUtf8("“quoted” and a\u00A0tie")},
        {QStringLiteral("\\url{https://example.org/a_b%20c}"), QStringLiteral("https://example.org/a_b%20c")},
        {QStringLiteral("Energy $E=mc^2$ \\ldots{} done"), QStringLiteral("Energy E=mc^2 … done")},
        {QStringLiteral("An  extra\n   line"), QStringLiteral("An extra line")},
        // A declaration within a group formats the rest of it, closed or not.
        {QStringLiteral("{plain \\em emphasised} after"), QStringLiteral("plain <i>emphasised</i> after")},
        {QStringLiteral("{plain \\em emphasised"), QStringLiteral("plain <i>emphasised</i>")},
    };
    for (const auto &[latex, markup] : cases) QCOMPARE(BibTeX::toMarkup(latex), markup);
    // In a title, braces protect case; a special character in braces does not.
    QCOMPARE(BibTeX::toMarkup(QStringLiteral("The {NASA} Mission of {\\'E}tienne"), true),
             QStringLiteral("The <span class=\"nocase\">NASA</span> Mission of Étienne"));
    QCOMPARE(BibTeX::sentenceCase(QStringLiteral("The Art of <span class=\"nocase\">NASA</span>: A Study of iPhone "
                                                 "Usage in the USA")),
             QStringLiteral("The art of <span class=\"nocase\">NASA</span>: A study of iPhone usage in the USA"));
}

void CitationTests::bibtexNames()
{
    struct Expected {
        QString given;
        QString family;
        QString particle;
        QString suffix;
        QString literal;
    };
    const QList<QPair<QString, QList<Expected>>> cases{
        {QStringLiteral("Doe, John and Jane Roe"), {{"John", "Doe", {}, {}, {}}, {"Jane", "Roe", {}, {}, {}}}},
        {QStringLiteral("Ludwig van Beethoven"), {{"Ludwig", "Beethoven", "van", {}, {}}}},
        {QStringLiteral("van Beethoven, Ludwig"), {{"Ludwig", "Beethoven", "van", {}, {}}}},
        {QStringLiteral("Doe, Jr., John"), {{"John", "Doe", {}, "Jr.", {}}}},
        {QStringLiteral("{Barnes and Noble} and {World Health Organization}"),
         {{{}, {}, {}, {}, "Barnes and Noble"}, {{}, {}, {}, {}, "World Health Organization"}}},
        {QStringLiteral("{\\'E}mile Zola"), {{"Émile", "Zola", {}, {}, {}}}},
        {QStringLiteral("Jean-Paul Sartre AND Simone de Beauvoir"),
         {{"Jean-Paul", "Sartre", {}, {}, {}}, {"Simone", "Beauvoir", "de", {}, {}}}},
        {QStringLiteral("Charles Louis Xavier Joseph de la Vall{\\'e}e Poussin"),
         {{"Charles Louis Xavier Joseph", "Vallée Poussin", "de la", {}, {}}}},
        {QStringLiteral("Doe, John and others"), {{"John", "Doe", {}, {}, {}}}},
        {QStringLiteral("A. B. Smith"), {{"A. B.", "Smith", {}, {}, {}}}},
        {QStringLiteral("family=Gogh, given=Vincent, prefix=van, useprefix=true"), {{"Vincent", "Gogh", "van", {}, {}}}},
        {QStringLiteral("{von Neumann}, John"), {{"John", "von Neumann", {}, {}, {}}}},
    };
    for (const auto &[value, expected] : cases) {
        const QList<CslName> names = BibTeX::parseNames(value);
        QCOMPARE(names.size(), expected.size());
        for (qsizetype i = 0; i < names.size(); ++i) {
            QCOMPARE(names[i].given, expected[i].given);
            QCOMPARE(names[i].family, expected[i].family);
            QCOMPARE(names[i].nonDroppingParticle, expected[i].particle);
            QCOMPARE(names[i].suffix, expected[i].suffix);
            QCOMPARE(names[i].literal, expected[i].literal);
        }
    }
    // Without useprefix the particle drops from short forms.
    const QList<CslName> dropping = BibTeX::parseNames(QStringLiteral("Ludwig van Beethoven"), false);
    QCOMPARE(dropping.first().droppingParticle, QStringLiteral("van"));
    QVERIFY(dropping.first().nonDroppingParticle.isEmpty());
}

void CitationTests::bibtexDatesTypesAndFields()
{
    const QString bib = QStringLiteral(R"(
@inproceedings{conf, author = {Lee, Mina}, title = {A Talk}, booktitle = {Proceedings of the Meeting},
  date = {2006-05-01/2006-05-03}, eventtitle = {The Meeting}, venue = {Lisbon}, pages = {10--12}}
@phdthesis{thesis, author = {Kim, Soo}, title = {Patterns}, school = {Example University},
  date = {2018?}}
@techreport{report, author = {Garcia, Luis}, title = {Annual Report}, institution = {Example Institute},
  number = {17}, year = {2020}, month = {jun}}
@online{site, author = {Park, Dana}, title = {How Citations Work}, url = {https://example.org/c},
  urldate = {2024-01-05}, date = {2021/2022}}
@misc{forthcoming, author = {Roe, Jane}, title = {Coming Soon}, year = {forthcoming}}
@article{news, author = {Hall, Rosa}, title = {Headline}, journaltitle = {The Daily Example},
  entrysubtype = {newspaper}, date = {2020-02-03}}
@book{german, author = {Schmidt, Hans}, title = {Die Kunst des Schreibens}, subtitle = {Ein Leitfaden},
  langid = {german}, location = {Berlin}, edition = {2}, series = {Example Series}, number = {4}, year = {1999}}
@collection{parent, editor = {Grant, Paul}, title = {Essays on Writing}, publisher = {Example Press},
  location = {Chicago}, year = {2015}}
@incollection{child, author = {Lee, Mina}, title = {Notes on the Margins}, crossref = {parent}, pages = {101--120}}
)");
    const BibTeX::Result result = BibTeX::parse(bib);
    QVERIFY2(result.diagnostics.isEmpty(), qPrintable(result.diagnostics.value(0).message));
    QHash<QString, CslReference> byId;
    for (const BibTeX::Entry &entry : result.entries) byId.insert(entry.reference.id, entry.reference);

    const CslReference conf = byId.value(QStringLiteral("conf"));
    QCOMPARE(conf.type, QStringLiteral("paper-conference"));
    QCOMPARE(conf.field(QStringLiteral("container-title")), QStringLiteral("Proceedings of the meeting"));
    QCOMPARE(conf.field(QStringLiteral("event-title")), QStringLiteral("The meeting"));
    QCOMPARE(conf.field(QStringLiteral("event-place")), QStringLiteral("Lisbon"));
    const CslDate range = conf.dates.value(QStringLiteral("issued"));
    QVERIFY(range.isRange);
    QCOMPARE(range.start, (CslDatePoint{2006, 5, 1, 3}));
    QCOMPARE(range.end, (CslDatePoint{2006, 5, 3, 3}));

    const CslReference thesis = byId.value(QStringLiteral("thesis"));
    QCOMPARE(thesis.type, QStringLiteral("thesis"));
    QCOMPARE(thesis.field(QStringLiteral("genre")), QStringLiteral("PhD thesis"));
    QCOMPARE(thesis.field(QStringLiteral("publisher")), QStringLiteral("Example University"));
    QVERIFY(thesis.dates.value(QStringLiteral("issued")).circa);

    const CslReference report = byId.value(QStringLiteral("report"));
    QCOMPARE(report.type, QStringLiteral("report"));
    QCOMPARE(report.field(QStringLiteral("publisher")), QStringLiteral("Example Institute"));
    QCOMPARE(report.field(QStringLiteral("number")), QStringLiteral("17"));
    QCOMPARE(report.field(QStringLiteral("genre")), QStringLiteral("technical report"));
    QCOMPARE(report.dates.value(QStringLiteral("issued")).start, (CslDatePoint{2020, 6, 0, 2}));

    const CslReference site = byId.value(QStringLiteral("site"));
    QCOMPARE(site.type, QStringLiteral("webpage"));
    QCOMPARE(site.field(QStringLiteral("URL")), QStringLiteral("https://example.org/c"));
    QCOMPARE(site.dates.value(QStringLiteral("accessed")).start, (CslDatePoint{2024, 1, 5, 3}));
    QCOMPARE(site.dates.value(QStringLiteral("issued")).end, (CslDatePoint{2022, 0, 0, 1}));

    const CslReference forthcoming = byId.value(QStringLiteral("forthcoming"));
    QCOMPARE(forthcoming.type, QStringLiteral("document"));
    QCOMPARE(forthcoming.field(QStringLiteral("status")), QStringLiteral("forthcoming"));
    QVERIFY(!forthcoming.dates.contains(QStringLiteral("issued")));

    QCOMPARE(byId.value(QStringLiteral("news")).type, QStringLiteral("article-newspaper"));
    QCOMPARE(byId.value(QStringLiteral("news")).field(QStringLiteral("container-title")),
             QStringLiteral("The Daily Example"));

    // Titles in other languages keep their case; subtitles join with a colon.
    const CslReference german = byId.value(QStringLiteral("german"));
    QCOMPARE(german.field(QStringLiteral("language")), QStringLiteral("de"));
    QCOMPARE(german.field(QStringLiteral("title")), QStringLiteral("Die Kunst des Schreibens: Ein Leitfaden"));
    QCOMPARE(german.field(QStringLiteral("publisher-place")), QStringLiteral("Berlin"));
    QCOMPARE(german.field(QStringLiteral("edition")), QStringLiteral("2"));
    QCOMPARE(german.field(QStringLiteral("collection-title")), QStringLiteral("Example Series"));
    QCOMPARE(german.field(QStringLiteral("collection-number")), QStringLiteral("4"));

    // A crossref'd collection gives its title as the container and its editor.
    const CslReference child = byId.value(QStringLiteral("child"));
    QCOMPARE(child.type, QStringLiteral("chapter"));
    QCOMPARE(child.field(QStringLiteral("title")), QStringLiteral("Notes on the margins"));
    QCOMPARE(child.field(QStringLiteral("container-title")), QStringLiteral("Essays on writing"));
    QCOMPARE(child.names.value(QStringLiteral("editor")).value(0).family, QStringLiteral("Grant"));
    QCOMPARE(child.field(QStringLiteral("publisher")), QStringLiteral("Example Press"));
    QCOMPARE(child.dates.value(QStringLiteral("issued")).start.year, 2015);
}

void CitationTests::bibliographyDiagnostics()
{
    using Severity = BibliographyDiagnostic::Severity;
    // Duplicate keys: the first entry is used and the second is reported
    // with its line. An undefined @string is reported, and so is a crossref
    // to nothing.
    const QByteArray bib = R"(@book{doe, title = {First}, year = 2001}
@book{doe, title = {Second}, year = 2002}
@book{roe, title = undefinedmacro, crossref = {nowhere}}
@book{broken, title = {Unclosed
)";
    const BibliographyDatabase database =
        BibliographyDatabase::fromData(bib, BibliographyDatabase::Format::BibLaTeX, QStringLiteral("refs.bib"));
    QVERIFY(database.isValid());
    QCOMPARE(database.references().size(), 2);
    QCOMPARE(database.find(QStringLiteral("doe"))->field(QStringLiteral("title")), QStringLiteral("First"));
    QStringList messages;
    for (const BibliographyDiagnostic &d : database.diagnostics())
        messages.append(QStringLiteral("%1:%2:%3").arg(d.line).arg(d.severity == Severity::Error ? u'E' : u'W').arg(d.message));
    QVERIFY2(messages.contains(QStringLiteral("2:W:The key “doe” is defined more than once; the first entry is used.")),
             qPrintable(messages.join(u'\n')));
    QVERIFY2(messages.contains(QStringLiteral("3:W:The string “undefinedmacro” is not defined.")),
             qPrintable(messages.join(u'\n')));
    QVERIFY2(messages.contains(QStringLiteral("3:W:The entry “roe” refers to “nowhere”, which is not defined.")),
             qPrintable(messages.join(u'\n')));
    QVERIFY2(messages.contains(QStringLiteral("4:E:A value's braces are not closed.")), qPrintable(messages.join(u'\n')));

    // CSL JSON: a syntax error names its line; a duplicate id is reported.
    const BibliographyDatabase badJson = BibliographyDatabase::fromData("[\n{\"id\": \"a\",\n\"title\": }\n]",
                                                                        BibliographyDatabase::Format::CslJson,
                                                                        QStringLiteral("refs.json"));
    QVERIFY(!badJson.isValid());
    QCOMPARE(badJson.diagnostics().size(), 1);
    QCOMPARE(badJson.diagnostics().first().severity, Severity::Error);
    QCOMPARE(badJson.diagnostics().first().line, 3);
    QVERIFY(badJson.diagnostics().first().message.startsWith(QStringLiteral("The CSL JSON is not valid")));
    const BibliographyDatabase duplicates = BibliographyDatabase::fromData(
        R"([{"id": "a", "title": "One"}, {"id": "a", "title": "Two"}, {"title": "No id"}])",
        BibliographyDatabase::Format::CslJson, QStringLiteral("refs.json"));
    QCOMPARE(duplicates.references().size(), 1);
    QCOMPARE(duplicates.diagnostics().size(), 2);

    // CSL YAML: a syntax error names its line.
    const BibliographyDatabase badYaml = BibliographyDatabase::fromData(
        "references:\n- id: a\n  title: [unclosed\n", BibliographyDatabase::Format::CslYaml, QStringLiteral("refs.yaml"));
    QVERIFY(!badYaml.isValid());
    QCOMPARE(badYaml.diagnostics().size(), 1);
    QVERIFY(badYaml.diagnostics().first().message.startsWith(QStringLiteral("The CSL YAML is not valid")));
    QVERIFY(badYaml.diagnostics().first().line >= 3);

    // A file that cannot be read.
    QTemporaryDir folder;
    const BibliographyDatabase missing = BibliographyDatabase::fromFile(folder.filePath(QStringLiteral("none.bib")));
    QVERIFY(!missing.isValid());
    QVERIFY(missing.diagnostics().first().message.startsWith(QStringLiteral("Cannot read the bibliography “none.bib”")));
    // The extension picks the reader.
    QCOMPARE(BibliographyDatabase::formatForPath(QStringLiteral("/a/refs.yml")), BibliographyDatabase::Format::CslYaml);
    QCOMPARE(BibliographyDatabase::formatForPath(QStringLiteral("/a/refs.JSON")), BibliographyDatabase::Format::CslJson);
    QCOMPARE(BibliographyDatabase::formatForPath(QStringLiteral("/a/refs.bib")), BibliographyDatabase::Format::BibLaTeX);
}

void CitationTests::cslJsonAndYaml()
{
    const QByteArray json = R"([
 {"id": 7, "type": "book", "title": "Numbered", "volume": 12, "number-of-pages": "300",
  "author": [{"family": "van Gogh", "given": "Vincent"}, {"family": "d'Alembert", "given": "Jean"},
             {"family": "Doe", "given": "John, Jr."}, {"literal": "Example Society"}],
  "issued": {"date-parts": [[2006, 5, 1], [2006, 5, 3]]},
  "accessed": {"raw": "2020-06"},
  "original-date": "1850~",
  "event-date": {"date-parts": [[2001]], "season": 2}}
])";
    const BibliographyDatabase fromJson =
        BibliographyDatabase::fromData(json, BibliographyDatabase::Format::CslJson, QStringLiteral("refs.json"));
    QVERIFY(fromJson.diagnostics().isEmpty());
    const CslReference *item = fromJson.find(QStringLiteral("7"));
    QVERIFY(item);
    QCOMPARE(item->field(QStringLiteral("volume")), QStringLiteral("12"));
    const QList<CslName> authors = item->names.value(QStringLiteral("author"));
    QCOMPARE(authors.size(), 4);
    QCOMPARE(authors[0].nonDroppingParticle, QStringLiteral("van"));
    QCOMPARE(authors[0].family, QStringLiteral("Gogh"));
    QCOMPARE(authors[1].nonDroppingParticle, QStringLiteral("d'"));
    QCOMPARE(authors[1].family, QStringLiteral("Alembert"));
    QCOMPARE(authors[2].given, QStringLiteral("John"));
    QCOMPARE(authors[2].suffix, QStringLiteral("Jr."));
    QCOMPARE(authors[3].literal, QStringLiteral("Example Society"));
    const CslDate issued = item->dates.value(QStringLiteral("issued"));
    QVERIFY(issued.isRange);
    QCOMPARE(issued.end, (CslDatePoint{2006, 5, 3, 3}));
    QCOMPARE(item->dates.value(QStringLiteral("accessed")).start, (CslDatePoint{2020, 6, 0, 2}));
    QVERIFY(item->dates.value(QStringLiteral("original-date")).circa);
    QCOMPARE(item->dates.value(QStringLiteral("event-date")).start, (CslDatePoint{2001, 14, 0, 2}));
    QCOMPARE(CslDate::fromText(QStringLiteral("not a date")).literal, QStringLiteral("not a date"));

    // CSL YAML as Pandoc writes it, with an anchor and an alias.
    const QByteArray yaml = R"(---
references:
- id: doe2006
  type: book
  author:
  - family: Doe
    given: John
  title: "Writing with examples"
  publisher: &press Example Press
  issued:
    date-parts:
    - [2006, 3]
- id: roe2007
  type: book
  author:
  - {family: Roe, given: Jane}
  title: 'Second: a book'
  publisher: *press
  issued: 2007-05-02
...
)";
    const BibliographyDatabase fromYaml =
        BibliographyDatabase::fromData(yaml, BibliographyDatabase::Format::CslYaml, QStringLiteral("refs.yaml"));
    QVERIFY2(fromYaml.diagnostics().isEmpty(), qPrintable(fromYaml.diagnostics().value(0).message));
    QCOMPARE(fromYaml.references().size(), 2);
    QCOMPARE(fromYaml.find(QStringLiteral("doe2006"))->dates.value(QStringLiteral("issued")).start,
             (CslDatePoint{2006, 3, 0, 2}));
    const CslReference *roe = fromYaml.find(QStringLiteral("roe2007"));
    QCOMPARE(roe->field(QStringLiteral("publisher")), QStringLiteral("Example Press"));
    QCOMPARE(roe->field(QStringLiteral("title")), QStringLiteral("Second: a book"));
    QCOMPARE(roe->names.value(QStringLiteral("author")).first().family, QStringLiteral("Roe"));
    QCOMPARE(roe->dates.value(QStringLiteral("issued")).start, (CslDatePoint{2007, 5, 2, 3}));
    // A plain list of references, and the older {year: } date form.
    const BibliographyDatabase list = BibliographyDatabase::fromData(
        "- id: a\n  title: A\n  issued:\n  - year: 1999\n    month: 4\n", BibliographyDatabase::Format::CslYaml,
        QStringLiteral("list.yaml"));
    QCOMPARE(list.references().size(), 1);
    QCOMPARE(list.find(QStringLiteral("a"))->dates.value(QStringLiteral("issued")).start, (CslDatePoint{1999, 4, 0, 2}));
}

void CitationTests::styleErrors()
{
    const QByteArray head = R"(<style xmlns="http://purl.org/net/xbiblio/csl" class="in-text" version="1.0">
<info><title>T</title><id>t</id></info>
)";
    struct Case {
        QByteArray xml;
        QString message;
    };
    const QList<Case> cases{
        {QByteArray("<style class=\"in-text\"><citation><layout></style>"),
         QStringLiteral("The citation style cannot be used. The file is not well-formed XML: Opening and ending tag "
                        "mismatch. (line 1, column 49).")},
        {QByteArray("<locale xml:lang=\"en-US\"/>"),
         QStringLiteral("The citation style cannot be used. The file is not a CSL style: its root element is "
                        "<locale>, not <style> (line 1).")},
        {head + "</style>", QStringLiteral("The citation style cannot be used. The style has no <citation> element.")},
        {head + "<citation><layout><text macro=\"missing\"/></layout></citation></style>",
         QStringLiteral("The citation style cannot be used. The style calls the macro “missing”, which it does not "
                        "define (line 3).")},
        {head + "<macro name=\"a\"><text macro=\"b\"/></macro><macro name=\"b\"><text macro=\"a\"/></macro>"
                "<citation><layout><text macro=\"a\"/></layout></citation></style>",
         QStringLiteral("The citation style cannot be used. The macro “a” calls itself.")},
        {head + "<citation><layout><frobnicate/></layout></citation></style>",
         QStringLiteral("The citation style cannot be used. The style uses an element CSL does not define, "
                        "<frobnicate> (line 3).")},
        {head + "<citation><layout><choose><else/></choose></layout></citation></style>",
         QStringLiteral("The citation style cannot be used. A <choose> element must start with <if> (line 3).")},
        {head + "<citation><layout><text/></layout></citation></style>",
         QStringLiteral("The citation style cannot be used. A <text> element needs a variable, macro, term or value "
                        "(line 3).")},
        {head + "<citation></citation></style>",
         QStringLiteral("The citation style cannot be used. The <citation> element has no <layout> (line 3).")},
        {QByteArray(R"(<style xmlns="http://purl.org/net/xbiblio/csl" class="in-text" version="1.0"><info><title>Dep</title>
<link href="http://www.zotero.org/styles/apa" rel="independent-parent"/></info></style>)"),
         QStringLiteral("The citation style cannot be used. This is a dependent style, which only names its parent "
                        "style (http://www.zotero.org/styles/apa); choose the parent style instead.")},
    };
    for (const Case &c : cases) {
        QString error;
        QVERIFY(!CslStyle::fromXml(c.xml, &error));
        QCOMPARE(error, c.message);
    }
    // A chosen file that does not exist.
    QTemporaryDir folder;
    QString error;
    QVERIFY(!CslCatalog::styleFromFile(folder.filePath(QStringLiteral("mine.csl")), &error));
    QVERIFY(error.startsWith(QStringLiteral("Cannot read the citation style “mine.csl”")));
    // A writer's own style from a file works like a bundled one.
    QFile file(folder.filePath(QStringLiteral("mine.csl")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(head + R"(<citation><layout prefix="[" suffix="]"><text variable="title" font-style="italic"/>
</layout></citation></style>)");
    file.close();
    const std::shared_ptr<const CslStyle> mine = CslCatalog::styleFromFile(file.fileName(), &error);
    QVERIFY2(mine, qPrintable(error));
    const Formatted result =
        format(mine, {database(works())}, {cluster({cite(QStringLiteral("doe2006a"))})});
    QCOMPARE(result.citations, QStringList{QStringLiteral("[*Writing with examples*]")});
    // Without a style, citations show their keys and say why.
    const Formatted keys = format(nullptr, {database(works())}, {cluster({cite(QStringLiteral("doe2006a"))})});
    QCOMPARE(keys.citations, QStringList{QStringLiteral("[doe2006a]")});
    QCOMPARE(keys.output.diagnostics.size(), 1);
    QCOMPARE(keys.output.diagnostics.first().code, CitationDiagnostic::Code::NoStyle);
}

void CitationTests::bundledStylesLoad()
{
    const QList<CslCatalog::StyleInfo> styles = CslCatalog::bundledStyles();
    QStringList ids;
    for (const CslCatalog::StyleInfo &info : styles) {
        ids.append(info.id);
        QString error;
        const std::shared_ptr<const CslStyle> style = CslCatalog::bundledStyle(info.id, &error);
        QVERIFY2(style, qPrintable(error));
        QCOMPARE(style->title(), info.title);
        QCOMPARE(style->id(), QStringLiteral("http://www.zotero.org/styles/") + info.id);
        QVERIFY(style->bibliography());
        QCOMPARE(CslCatalog::localeFor(*style), info.locale);
    }
    QCOMPARE(ids, (QStringList{QStringLiteral("apa"), QStringLiteral("chicago-author-date"),
                               QStringLiteral("chicago-notes-bibliography"), QStringLiteral("ieee"),
                               QStringLiteral("modern-language-association"), QStringLiteral("harvard-cite-them-right"),
                               QStringLiteral("nlm-citation-sequence")}));
    QVERIFY(CslCatalog::bundledStyle(QStringLiteral("chicago-notes-bibliography"))->isNoteStyle());
    QVERIFY(!CslCatalog::bundledStyle(QStringLiteral("apa"))->isNoteStyle());
    // Vancouver is the NLM style; the same style is shared between callers.
    QCOMPARE(CslCatalog::bundledStyle(QStringLiteral("vancouver")),
             CslCatalog::bundledStyle(QStringLiteral("nlm-citation-sequence")));
    QString error;
    QVERIFY(!CslCatalog::bundledStyle(QStringLiteral("nonexistent"), &error));
    QCOMPARE(error, QStringLiteral("parch has no bundled citation style called “nonexistent”."));
    // Every bundled file is valid XML that names its CC BY-SA licence.
    for (const QString &name : QDir(QStringLiteral(":/csl")).entryList()) {
        QFile file(QStringLiteral(":/csl/") + name);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY2(file.readAll().contains("creativecommons.org/licenses/by-sa"), qPrintable(name));
    }
}

void CitationTests::localeTermsAndFallbacks()
{
    const std::shared_ptr<const CslLocale> us = CslCatalog::defaultLocale();
    QCOMPARE(us->language(), QStringLiteral("en-US"));
    QVERIFY(us->punctuationInQuote());
    QCOMPARE(us->termText(QStringLiteral("editor"), QStringLiteral("short"), true), QStringLiteral("eds."));
    QCOMPARE(us->termText(QStringLiteral("editor"), QStringLiteral("verb")), QStringLiteral("edited by"));
    // A missing form falls back: verb-short to verb to long, symbol to short.
    QCOMPARE(us->termText(QStringLiteral("and"), QStringLiteral("verb-short")), QStringLiteral("and"));
    QCOMPARE(us->termText(QStringLiteral("page"), QStringLiteral("symbol")), QStringLiteral("p."));
    QStringList ordinals;
    for (const int n : {1, 2, 3, 4, 11, 12, 13, 21, 22, 23, 101, 111, 112}) ordinals.append(us->ordinal(n));
    QCOMPARE(ordinals.join(u' '), QStringLiteral("1st 2nd 3rd 4th 11th 12th 13th 21st 22nd 23rd 101st 111th 112th"));
    QCOMPARE(us->longOrdinal(2), QStringLiteral("second"));
    QCOMPARE(us->longOrdinal(12), QStringLiteral("12th"));

    QString error;
    const std::shared_ptr<const CslLocale> gb = CslCatalog::locale(QStringLiteral("en-GB"), {}, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(gb->language(), QStringLiteral("en-GB"));
    QVERIFY(!gb->punctuationInQuote());
    QCOMPARE(gb->termText(QStringLiteral("open-quote")), QStringLiteral("‘"));

    // Other locales come from files.
    QTemporaryDir folder;
    QFile file(folder.filePath(QStringLiteral("locales-de-DE.xml")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(R"(<locale xmlns="http://purl.org/net/xbiblio/csl" version="1.0" xml:lang="de-DE">
<style-options punctuation-in-quote="false"/>
<date form="text"><date-part name="day" form="ordinal" suffix=" "/><date-part name="month" suffix=" "/><date-part name="year"/></date>
<terms><term name="and">und</term><term name="ordinal">.</term><term name="month-03">März</term>
<term name="open-quote">„</term><term name="close-quote">“</term></terms></locale>)");
    file.close();
    error.clear();
    const std::shared_ptr<const CslLocale> de = CslCatalog::locale(QStringLiteral("de-DE"), {folder.path()}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(de->termText(QStringLiteral("and")), QStringLiteral("und"));
    QCOMPARE(de->ordinal(3), QStringLiteral("3."));
    // Rendering with it: German terms, quotes and dates.
    const QByteArray style = R"(<style xmlns="http://purl.org/net/xbiblio/csl" class="in-text" version="1.0">
<info><title>T</title><id>t</id></info><citation><layout>
<group delimiter=", "><names variable="author"><name and="text"/></names><text variable="title" quotes="true"/>
<date variable="issued" form="text"/></group></layout></citation></style>)";
    CitationFormatter formatter;
    formatter.setStyle(CslStyle::fromXml(style, nullptr));
    formatter.setLocale(de);
    formatter.setDatabases({database(R"([{"id": "a", "title": "Titel", "issued": {"date-parts": [[2001, 3, 3]]},
        "author": [{"family": "Müller", "given": "Eva"}, {"family": "Roth", "given": "Uwe"}]}])")});
    const CitationOutput output = formatter.format({cluster({cite(QStringLiteral("a"))})});
    QCOMPARE(output.clusters.first().toPlainText(), QStringLiteral("Eva Müller und Uwe Roth, „Titel“, 3. März 2001"));
    // A language with no locale: English (US), and a message.
    error.clear();
    QCOMPARE(CslCatalog::locale(QStringLiteral("fr-FR"), {folder.path()}, &error), us);
    QCOMPARE(error, QStringLiteral("No citation locale for “fr-FR” was found; English (US) is used."));
}

void CitationTests::apaStyle()
{
    // APA Publication Manual, 7th edition: chapter 8 for citations, 9 and
    // 10 for references (10.1 articles, 10.2 books, 10.3 chapters, 10.4
    // reports, 10.6 dissertations, 10.16 web pages).
    const Database data = database(works({{QStringLiteral("THESIS_GENRE"), QStringLiteral("Unpublished doctoral dissertation")}}));
    QList<CitationClusterSpec> clusters = authorDateCitations();
    clusters.append(cluster({cite(QStringLiteral("park2021"), QStringLiteral("para. 4"))}));
    const Formatted result = formatBundled(QStringLiteral("apa"), {data}, clusters);
    QCOMPARE(result.citations, (QStringList{
                                   QStringLiteral("(Doe, 2006a, p. 23)"),
                                   QStringLiteral("(Smith et al., 2019)"),
                                   QStringLiteral("(see Lee, 2015, Chapter 2; Park, 2021)"),
                                   QStringLiteral("(Doe, 2006a, 2006b)"),
                                   QStringLiteral("Doe (2006b)"),
                                   QStringLiteral("(2018)"),
                                   QStringLiteral("(Adams et al., 2022, pp. 3–5)"),
                                   QStringLiteral("(Garcia, 2020)"),
                                   QStringLiteral("(Smith et al., 2019, p. 50, emphasis added)"),
                                   QStringLiteral("(Park, 2021, para. 4)"),
                               }));
    QCOMPARE(result.bibliography,
             (QStringList{
                 QStringLiteral("Adams, A., Baker, B., Clark, C., Davis, D., Evans, E., Fox, F., Green, G., & Hill, H. "
                                "(2022). Collective writing at scale. *Collaboration Studies*, *4*(1), 1–19."),
                 QStringLiteral("Doe, J. (2006a). *Writing with examples*. Example Press."),
                 QStringLiteral("Doe, J. (2006b). *Writing without examples*. Example Press."),
                 QStringLiteral("Garcia, L. (2020). *Annual report on examples* (Report No. 17). Example Institute."),
                 QStringLiteral("Kim, S. (2018). *Patterns of revision in student writing* [Unpublished doctoral "
                                "dissertation]. Example University."),
                 QStringLiteral("Lee, M. (2015). Notes on the margins. In P. Grant & R. Hall (Eds.), *Essays on writing* "
                                "(pp. 101–120). Example University Press."),
                 QStringLiteral("Park, D. (2021, March 14). *How citations work*. Example Writing Center. "
                                "https://example.org/citations"),
                 QStringLiteral("Smith, A., Jones, B., & Brown, C. (2019). Measuring the quiet: A field study. *Journal of "
                                "Examples*, *12*(3), 45–67. https://doi.org/10.1234/joe.2019.003"),
             }));
    QVERIFY(result.output.diagnostics.isEmpty());
    QVERIFY(result.output.layout.hangingIndent);
    QCOMPARE(result.output.layout.lineSpacing, 2.0);
    // The DOI is a link, resolver and all.
    const CslText smith = result.output.bibliography.last().text;
    QCOMPARE(smith.spans().last().text, QStringLiteral("https://doi.org/10.1234/joe.2019.003"));
    QCOMPARE(smith.spans().last().format.link, QStringLiteral("https://doi.org/10.1234/joe.2019.003"));
    QVERIFY(smith.toHtml().endsWith(QStringLiteral("<a href=\"https://doi.org/10.1234/joe.2019.003\">"
                                                   "https://doi.org/10.1234/joe.2019.003</a>")));

    // APA 8.20: initials tell apart first authors who share a surname.
    const Database namesakes = database(R"([
 {"id": "a", "type": "book", "title": "One", "author": [{"family": "Doe", "given": "John"}], "issued": {"date-parts": [[2000]]}},
 {"id": "b", "type": "book", "title": "Two", "author": [{"family": "Doe", "given": "Mary"}], "issued": {"date-parts": [[2010]]}}
])");
    const Formatted initials = formatBundled(QStringLiteral("apa"), {namesakes},
                                             {cluster({cite(QStringLiteral("a")), cite(QStringLiteral("b"))})});
    QCOMPARE(initials.citations, QStringList{QStringLiteral("(J. Doe, 2000; M. Doe, 2010)")});
}

void CitationTests::chicagoAuthorDateStyle()
{
    // The Chicago Manual of Style, 18th edition, chapter 13 (author-date):
    // no place of publication, et al. for three or more authors in the text
    // and for more than six in the reference list (first three named).
    const Database data = database(works({{QStringLiteral("THESIS_GENRE"), QStringLiteral("PhD diss.")}}));
    const Formatted result = formatBundled(QStringLiteral("chicago-author-date"), {data}, authorDateCitations());
    QCOMPARE(result.citations, (QStringList{
                                   QStringLiteral("(Doe 2006a, 23)"),
                                   QStringLiteral("(Smith et al. 2019)"),
                                   QStringLiteral("(see Lee 2015, chap. 2; Park 2021)"),
                                   QStringLiteral("(Doe 2006a, 2006b)"),
                                   QStringLiteral("Doe (2006b)"),
                                   QStringLiteral("(2018)"),
                                   QStringLiteral("(Adams et al. 2022, 3–5)"),
                                   QStringLiteral("(Garcia 2020)"),
                                   QStringLiteral("(Smith et al. 2019, 50, emphasis added)"),
                               }));
    QCOMPARE(result.bibliography,
             (QStringList{
                 QStringLiteral("Adams, Alice, Ben Baker, Cora Clark, et al. 2022. “Collective Writing at Scale.” "
                                "*Collaboration Studies* 4 (1): 1–19."),
                 QStringLiteral("Doe, John. 2006a. *Writing with Examples*. Example Press."),
                 QStringLiteral("Doe, John. 2006b. *Writing without Examples*. Example Press."),
                 QStringLiteral("Garcia, Luis. 2020. *Annual Report on Examples*. Report No. 17. Example Institute."),
                 QStringLiteral("Kim, Soo. 2018. “Patterns of Revision in Student Writing.” PhD diss., Example University."),
                 QStringLiteral("Lee, Mina. 2015. “Notes on the Margins.” In *Essays on Writing*, edited by Paul Grant and "
                                "Rosa Hall. Example University Press."),
                 QStringLiteral("Park, Dana. 2021. “How Citations Work.” Example Writing Center, March 14. "
                                "https://example.org/citations."),
                 QStringLiteral("Smith, Anna, Brian Jones, and Carol Brown. 2019. “Measuring the Quiet: A Field Study.” "
                                "*Journal of Examples* 12 (3): 45–67. https://doi.org/10.1234/joe.2019.003."),
             }));
}

void CitationTests::chicagoNotesStyle()
{
    // The Chicago Manual of Style, 18th edition, chapter 14 (notes and
    // bibliography). 14.34: a shortened note, not "ibid.", follows a note
    // for the same source, so the fourth note repeats the third.
    const Database data = database(works({{QStringLiteral("THESIS_GENRE"), QStringLiteral("PhD diss.")}}));
    const QList<CitationClusterSpec> notes{
        cluster({cite(QStringLiteral("doe2006a"), QStringLiteral("p. 23"))}, 1),
        cluster({cite(QStringLiteral("smith2019"), QStringLiteral("p. 50"))}, 2),
        cluster({cite(QStringLiteral("doe2006a"), QStringLiteral("p. 24"))}, 3),
        cluster({cite(QStringLiteral("doe2006a"), QStringLiteral("p. 24"))}, 4),
        cluster({cite(QStringLiteral("lee2015"), QStringLiteral("p. 105"))}, 5),
        cluster({cite(QStringLiteral("doe2006b"))}, 6),
        cluster({cite(QStringLiteral("park2021"))}, 7),
        cluster({cite(QStringLiteral("kim2018"))}, 8),
        cluster({cite(QStringLiteral("smith2019"), QStringLiteral("p. 52"))}, 9),
        cluster({cite(QStringLiteral("garcia2020"), QStringLiteral("p. 7"))}, 10),
    };
    const Formatted result = formatBundled(QStringLiteral("chicago-notes-bibliography"), {data}, notes);
    QVERIFY(result.output.noteStyle);
    QCOMPARE(result.citations,
             (QStringList{
                 QStringLiteral("John Doe, *Writing with Examples* (Example Press, 2006), 23."),
                 QStringLiteral("Anna Smith et al., “Measuring the Quiet: A Field Study,” *Journal of Examples* 12, no. 3 "
                                "(2019): 50, https://doi.org/10.1234/joe.2019.003."),
                 QStringLiteral("Doe, *Writing with Examples*, 24."),
                 QStringLiteral("Doe, *Writing with Examples*, 24."),
                 QStringLiteral("Mina Lee, “Notes on the Margins,” in *Essays on Writing*, ed. Paul Grant and Rosa Hall "
                                "(Example University Press, 2015), 105."),
                 QStringLiteral("John Doe, *Writing without Examples* (Example Press, 2006)."),
                 QStringLiteral("Dana Park, “How Citations Work,” Example Writing Center, March 14, 2021, "
                                "https://example.org/citations."),
                 QStringLiteral("Soo Kim, “Patterns of Revision in Student Writing” (PhD diss., Example University, 2018)."),
                 QStringLiteral("Smith et al., “Measuring the Quiet,” 52."),
                 QStringLiteral("Luis Garcia, *Annual Report on Examples*, Report no. 17 (Example Institute, 2020), 7."),
             }));
    QCOMPARE(result.bibliography,
             (QStringList{
                 QStringLiteral("Doe, John. *Writing with Examples*. Example Press, 2006."),
                 QStringLiteral("Doe, John. *Writing without Examples*. Example Press, 2006."),
                 QStringLiteral("Garcia, Luis. *Annual Report on Examples*. Report No. 17. Example Institute, 2020."),
                 QStringLiteral("Kim, Soo. “Patterns of Revision in Student Writing.” PhD diss., Example University, 2018."),
                 QStringLiteral("Lee, Mina. “Notes on the Margins.” In *Essays on Writing*, edited by Paul Grant and Rosa "
                                "Hall. Example University Press, 2015."),
                 QStringLiteral("Park, Dana. “How Citations Work.” Example Writing Center, March 14, 2021. "
                                "https://example.org/citations."),
                 QStringLiteral("Smith, Anna, Brian Jones, and Carol Brown. “Measuring the Quiet: A Field Study.” *Journal "
                                "of Examples* 12, no. 3 (2019): 45–67. https://doi.org/10.1234/joe.2019.003."),
             }));
}

void CitationTests::ieeeStyle()
{
    // IEEE Reference Guide (11.29.2023): references numbered in order of
    // first citation, book titles in title case, abbreviated journal
    // titles, "Ch." for chapters. The bundled style writes "Eds., City"
    // where the guide's chapter example has "Ed. City", and does not join
    // consecutive numbers into a range.
    const Database data = database(works({{QStringLiteral("BOOK_A"), QStringLiteral("The Craft of Examples")},
                                          {QStringLiteral("CONTAINER"), QStringLiteral("Essays on Writing")},
                                          {QStringLiteral("THESIS_GENRE"), QStringLiteral("Ph.D. dissertation")},
                                          {QStringLiteral("REPORT_GENRE"), QStringLiteral("Tech. Rep.")}}));
    const Formatted result = formatBundled(
        QStringLiteral("ieee"), {data},
        {cluster({cite(QStringLiteral("doe2006a"), QStringLiteral("p. 23"))}),
         cluster({cite(QStringLiteral("smith2019"))}),
         cluster({cite(QStringLiteral("lee2015"), QStringLiteral("chap. 4"))}),
         cluster({cite(QStringLiteral("park2021"))}), cluster({cite(QStringLiteral("kim2018"))}),
         cluster({cite(QStringLiteral("garcia2020"))}), cluster({cite(QStringLiteral("adams2022"))}),
         cluster({cite(QStringLiteral("park2021")), cite(QStringLiteral("smith2019"))})});
    QCOMPARE(result.citations,
             (QStringList{QStringLiteral("[1, p. 23]"), QStringLiteral("[2]"), QStringLiteral("[3, Ch. 4]"),
                          QStringLiteral("[4]"), QStringLiteral("[5]"), QStringLiteral("[6]"), QStringLiteral("[7]"),
                          QStringLiteral("[2], [4]")}));
    QCOMPARE(result.bibliography,
             (QStringList{
                 QStringLiteral("[1] J. Doe, *The Craft of Examples*. New York, NY, USA: Example Press, 2006."),
                 QStringLiteral("[2] A. Smith, B. Jones, and C. Brown, “Measuring the quiet: A field study,” *J. Examples*, "
                                "vol. 12, no. 3, pp. 45–67, 2019, doi: 10.1234/joe.2019.003."),
                 QStringLiteral("[3] M. Lee, “Notes on the margins,” in *Essays on Writing*, P. Grant and R. Hall, Eds., "
                                "Chicago, IL, USA: Example University Press, 2015, pp. 101–120."),
                 QStringLiteral("[4] D. Park, “How citations work,” Example Writing Center. Accessed: Jan. 05, 2024. "
                                "[Online]. Available: https://example.org/citations"),
                 QStringLiteral("[5] S. Kim, “Patterns of revision in student writing,” Ph.D. dissertation, Example "
                                "University, Austin, TX, USA, 2018."),
                 QStringLiteral("[6] L. Garcia, “Annual report on examples,” Example Institute, Boston, MA, USA, Tech. Rep. "
                                "17, 2020."),
                 QStringLiteral("[7] A. Adams *et al.*, “Collective writing at scale,” *Collaboration Studies*, vol. 4, no. "
                                "1, pp. 1–19, 2022."),
             }));
    QCOMPARE(result.output.layout.secondFieldAlign, QStringLiteral("flush"));
    // The number stands apart from the entry, as the guide's list sets it.
    const CslText first = result.output.bibliography.first().text;
    QCOMPARE(first.spans().first().text, QStringLiteral("[1]"));
    QCOMPARE(first.spans().first().format.display, CslFormat::Display::LeftMargin);
    QCOMPARE(first.spans().last().format.display, CslFormat::Display::RightInline);
}

void CitationTests::mlaStyle()
{
    // MLA Handbook, 9th edition: (Author page) citations, a short title when
    // the works cited list has several works by the author (6.10), et al.
    // for three or more authors, and a dash for a repeated author in the
    // list (5.124), which the bundled style writes as three em dashes.
    const Database data = database(works({{QStringLiteral("THESIS_GENRE"), QStringLiteral("PhD dissertation")}}));
    const Formatted result = formatBundled(
        QStringLiteral("modern-language-association"), {data},
        {cluster({cite(QStringLiteral("doe2006a"), QStringLiteral("p. 23"))}),
         cluster({cite(QStringLiteral("smith2019"), QStringLiteral("p. 50"))}),
         cluster({cite(QStringLiteral("lee2015"), QStringLiteral("105"))}), cluster({cite(QStringLiteral("park2021"))}),
         cluster({cite(QStringLiteral("doe2006b")), cite(QStringLiteral("smith2019"))}),
         cluster({cite(QStringLiteral("kim2018"))}), cluster({cite(QStringLiteral("garcia2020"))}),
         cluster({cite(QStringLiteral("adams2022"), QStringLiteral("pp. 3-5"))})});
    QCOMPARE(result.citations, (QStringList{
                                   QStringLiteral("(Doe, *Writing with Examples* 23)"),
                                   QStringLiteral("(Smith et al. 50)"),
                                   QStringLiteral("(Lee 105)"),
                                   QStringLiteral("(Park)"),
                                   QStringLiteral("(Doe, *Writing without Examples*; Smith et al.)"),
                                   QStringLiteral("(Kim)"),
                                   QStringLiteral("(Garcia)"),
                                   QStringLiteral("(Adams et al. 3–5)"),
                               }));
    QCOMPARE(result.bibliography,
             (QStringList{
                 QStringLiteral("Adams, Alice, et al. “Collective Writing at Scale.” *Collaboration Studies*, vol. 4, no. 1, "
                                "2022, pp. 1–19."),
                 QStringLiteral("Doe, John. *Writing with Examples*. Example Press, 2006."),
                 QStringLiteral("———. *Writing without Examples*. Example Press, 2006."),
                 QStringLiteral("Garcia, Luis. *Annual Report on Examples*. Report no. 17, Example Institute, 2020."),
                 QStringLiteral("Kim, Soo. *Patterns of Revision in Student Writing*. 2018. Example University, PhD "
                                "dissertation."),
                 QStringLiteral("Lee, Mina. “Notes on the Margins.” *Essays on Writing*, edited by Paul Grant and Rosa Hall, "
                                "Example University Press, 2015, pp. 101–20."),
                 QStringLiteral("Park, Dana. “How Citations Work.” *Example Writing Center*, 14 Mar. 2021, "
                                "https://example.org/citations."),
                 QStringLiteral("Smith, Anna, et al. “Measuring the Quiet: A Field Study.” *Journal of Examples*, vol. 12, "
                                "no. 3, 2019, pp. 45–67, https://doi.org/10.1234/joe.2019.003."),
             }));
}

void CitationTests::harvardStyle()
{
    // Cite Them Right, 12th edition (Harvard), in British English: single
    // quotation marks, day-month dates, "eds", up to three authors named in
    // the text.
    const Database data = database(works({{QStringLiteral("BOOK_PLACE"), QStringLiteral("New York")},
                                          {QStringLiteral("CHAPTER_PLACE"), QStringLiteral("Chicago")},
                                          {QStringLiteral("REPORT_PLACE"), QStringLiteral("Boston")},
                                          {QStringLiteral("THESIS_GENRE"), QStringLiteral("PhD thesis")},
                                          {QStringLiteral("\"container-title\": \"SITE\","), QString()}}));
    const Formatted result = formatBundled(QStringLiteral("harvard"), {data}, authorDateCitations());
    QCOMPARE(result.citations, (QStringList{
                                   QStringLiteral("(Doe, 2006a, p. 23)"),
                                   QStringLiteral("(Smith, Jones and Brown, 2019)"),
                                   QStringLiteral("(see Lee, 2015, ch. 2; Park, 2021)"),
                                   QStringLiteral("(Doe, 2006a, 2006b)"),
                                   QStringLiteral("Doe (2006b)"),
                                   QStringLiteral("(2018)"),
                                   QStringLiteral("(Adams *et al.*, 2022, pp. 3–5)"),
                                   QStringLiteral("(Garcia, 2020)"),
                                   QStringLiteral("(Smith, Jones and Brown, 2019, p. 50, emphasis added)"),
                               }));
    QCOMPARE(result.bibliography,
             (QStringList{
                 QStringLiteral("Adams, A. *et al.* (2022) ‘Collective writing at scale’, *Collaboration Studies*, 4(1), "
                                "pp. 1–19."),
                 QStringLiteral("Doe, J. (2006a) *Writing with examples*. New York: Example Press."),
                 QStringLiteral("Doe, J. (2006b) *Writing without examples*. New York: Example Press."),
                 QStringLiteral("Garcia, L. (2020) *Annual report on examples*. Report 17. Boston: Example Institute."),
                 QStringLiteral("Kim, S. (2018) *Patterns of revision in student writing*. PhD thesis. Example University."),
                 QStringLiteral("Lee, M. (2015) ‘Notes on the margins’, in P. Grant and R. Hall (eds) *Essays on writing*. "
                                "Chicago: Example University Press, pp. 101–120."),
                 QStringLiteral("Park, D. (2021) *How citations work*. Available at: https://example.org/citations "
                                "(Accessed: 5 January 2024)."),
                 QStringLiteral("Smith, A., Jones, B. and Brown, C. (2019) ‘Measuring the quiet: A field study’, *Journal "
                                "of Examples*, 12(3), pp. 45–67. Available at: https://doi.org/10.1234/joe.2019.003."),
             }));
}

void CitationTests::vancouverStyle()
{
    // Citing Medicine, 2nd edition (NLM, the Vancouver system): numbers by
    // first citation, consecutive numbers joined into a range, abbreviated
    // journal titles, places with their state, up to six authors and et al.
    const Database data = database(works({{QStringLiteral("BOOK_PLACE"), QStringLiteral("New York (NY)")},
                                          {QStringLiteral("CHAPTER_PLACE"), QStringLiteral("Chicago (IL)")},
                                          {QStringLiteral("REPORT_PLACE"), QStringLiteral("Boston (MA)")},
                                          {QStringLiteral("THESIS_PLACE"), QStringLiteral("Austin (TX)")},
                                          {QStringLiteral("THESIS_GENRE"), QStringLiteral("dissertation")},
                                          {QStringLiteral("REPORT_GENRE"), QString()},
                                          {QStringLiteral("J. Examples"), QStringLiteral("J Ex")}}));
    const Formatted result = formatBundled(
        QStringLiteral("vancouver"), {data},
        {cluster({cite(QStringLiteral("doe2006a"))}),
         cluster({cite(QStringLiteral("smith2019")), cite(QStringLiteral("lee2015")), cite(QStringLiteral("park2021")),
                  cite(QStringLiteral("adams2022"))}),
         cluster({cite(QStringLiteral("kim2018"))}), cluster({cite(QStringLiteral("garcia2020"))}),
         cluster({cite(QStringLiteral("doe2006b")), cite(QStringLiteral("doe2006a")), cite(QStringLiteral("smith2019")),
                  cite(QStringLiteral("lee2015")), cite(QStringLiteral("kim2018"))})});
    QCOMPARE(result.citations, (QStringList{QStringLiteral("(1)"), QStringLiteral("(2–5)"), QStringLiteral("(6)"),
                                            QStringLiteral("(7)"), QStringLiteral("(1–3,6,8)")}));
    QCOMPARE(result.bibliography,
             (QStringList{
                 QStringLiteral("1. Doe J. Writing with examples. New York (NY): Example Press; 2006."),
                 QStringLiteral("2. Smith A, Jones B, Brown C. Measuring the quiet: A field study. J Ex. 2019;12(3):45–67. "
                                "doi:10.1234/joe.2019.003"),
                 QStringLiteral("3. Lee M. Notes on the margins. In: Grant P, Hall R, editors. Essays on writing. Chicago "
                                "(IL): Example University Press; 2015. p. 101–20."),
                 QStringLiteral("4. Park D. Example Writing Center [Internet]. 2021 [cited 2024 Jan 5]. How citations work. "
                                "Available from: https://example.org/citations"),
                 QStringLiteral("5. Adams A, Baker B, Clark C, Davis D, Evans E, Fox F, et al. Collective writing at scale. "
                                "Collaboration Studies. 2022;4(1):1–19."),
                 QStringLiteral("6. Kim S. Patterns of revision in student writing [dissertation]. [Austin (TX)]: Example "
                                "University; 2018."),
                 QStringLiteral("7. Garcia L. Annual report on examples. Boston (MA): Example Institute; 2020. Report no.: "
                                "17."),
                 QStringLiteral("8. Doe J. Writing without examples. New York (NY): Example Press; 2006."),
             }));
}

void CitationTests::locatorParsing()
{
    const CslLocale &locale = *CslCatalog::defaultLocale();
    struct Case {
        QString text;
        QString label;
        QString locator;
        QString suffix;
    };
    const QList<Case> cases{
        {QStringLiteral("p. 23"), QStringLiteral("page"), QStringLiteral("23"), {}},
        {QStringLiteral("pp. 23-25"), QStringLiteral("page"), QStringLiteral("23-25"), {}},
        {QStringLiteral("23"), QStringLiteral("page"), QStringLiteral("23"), {}},
        {QStringLiteral("chap. 2"), QStringLiteral("chapter"), QStringLiteral("2"), {}},
        {QStringLiteral("chapter 2"), QStringLiteral("chapter"), QStringLiteral("2"), {}},
        {QStringLiteral("§ 5"), QStringLiteral("section"), QStringLiteral("5"), {}},
        {QStringLiteral("vol. 3"), QStringLiteral("volume"), QStringLiteral("3"), {}},
        {QStringLiteral("para. 4"), QStringLiteral("paragraph"), QStringLiteral("4"), {}},
        {QStringLiteral("fig. 7"), QStringLiteral("figure"), QStringLiteral("7"), {}},
        {QStringLiteral("n. 12"), QStringLiteral("note"), QStringLiteral("12"), {}},
        {QStringLiteral("p. xii"), QStringLiteral("page"), QStringLiteral("xii"), {}},
        {QStringLiteral("pp. 4, 7-9"), QStringLiteral("page"), QStringLiteral("4, 7-9"), {}},
        {QStringLiteral("p. 23, emphasis added"), QStringLiteral("page"), QStringLiteral("23"),
         QStringLiteral(", emphasis added")},
        {QStringLiteral("p. {iv-vi and passim}"), QStringLiteral("page"), QStringLiteral("iv-vi and passim"), {}},
        {QStringLiteral("emphasis added"), {}, {}, QStringLiteral("emphasis added")},
    };
    for (const Case &c : cases) {
        const CslEngine::Locator locator = CslEngine::parseLocator(c.text, locale);
        QCOMPARE(locator.label, c.label);
        QCOMPARE(locator.locator, c.locator);
        QCOMPARE(locator.suffix, c.suffix);
    }
}

void CitationTests::notePositions()
{
    // A note style that shows each cite's position, as CSL 1.0.2 defines
    // them: first, subsequent, ibid, ibid-with-locator and near-note.
    const QByteArray xml = R"(<style xmlns="http://purl.org/net/xbiblio/csl" class="note" version="1.0">
<info><title>Positions</title><id>positions</id></info>
<citation near-note-distance="3"><layout delimiter="; " suffix=".">
<choose>
  <if position="ibid-with-locator"><group delimiter=", "><text term="ibid" text-case="capitalize-first"/><text variable="locator"/></group></if>
  <else-if position="ibid"><text term="ibid" text-case="capitalize-first"/></else-if>
  <else-if position="near-note"><group delimiter=", "><text variable="title" form="short"/><text value="near"/><text variable="locator"/></group></else-if>
  <else-if position="subsequent"><group delimiter=", "><text variable="title" form="short"/><group><text value="n. "/><text variable="first-reference-note-number"/></group><text variable="locator"/></group></else-if>
  <else><group delimiter=", "><text variable="title"/><text variable="locator"/></group></else>
</choose></layout></citation></style>)";
    const std::shared_ptr<const CslStyle> style = CslStyle::fromXml(xml, nullptr);
    QVERIFY(style);
    const Database data = database(R"([
 {"id": "a", "title": "Alpha: A history", "title-short": "Alpha"},
 {"id": "b", "title": "Beta: A history", "title-short": "Beta"}])");
    const Formatted result = format(
        style, {data},
        {cluster({cite(QStringLiteral("a"))}, 1),
         cluster({cite(QStringLiteral("a"), QStringLiteral("p. 5"))}, 2),
         cluster({cite(QStringLiteral("a"), QStringLiteral("p. 5"))}, 3),
         cluster({cite(QStringLiteral("a"))}, 4),
         cluster({cite(QStringLiteral("b")), cite(QStringLiteral("b"), QStringLiteral("p. 2"))}, 5),
         cluster({cite(QStringLiteral("a"))}, 10),
         cluster({cite(QStringLiteral("b"), QStringLiteral("p. 9"))}, 20),
         // Two citations in one note: the second's cite follows the first's;
         // the next note follows a note of two cites, so it is no ibid.
         cluster({cite(QStringLiteral("a"))}, 21), cluster({cite(QStringLiteral("a"))}, 21),
         cluster({cite(QStringLiteral("a"))}, 30)});
    QCOMPARE(result.citations, (QStringList{
                                   QStringLiteral("Alpha: A history."),
                                   QStringLiteral("Ibid., 5."),
                                   QStringLiteral("Ibid."),
                                   QStringLiteral("Alpha, near."),
                                   QStringLiteral("Beta: A history; Ibid., 2."),
                                   QStringLiteral("Alpha, n. 1."),
                                   QStringLiteral("Beta, n. 5, 9."),
                                   QStringLiteral("Alpha, n. 1."),
                                   QStringLiteral("Ibid."),
                                   QStringLiteral("Alpha, n. 1."),
                               }));
}

void CitationTests::keysAndScopes()
{
    using Code = CitationDiagnostic::Code;
    const QString chapter = QStringLiteral("/library/chapters/one.md");
    const Database root = database(works(), BibliographyDatabase::Format::CslJson, QStringLiteral("/library/root.json"));
    // A second database with a key the first also has, for another work.
    const Database other = database(R"([{"id": "doe2006a", "type": "book", "title": "A different book",
        "author": [{"family": "Other", "given": "Olive"}], "issued": {"date-parts": [[1999]]}}])",
                                    BibliographyDatabase::Format::CslJson, QStringLiteral("/library/other.json"));
    CitationFormatter formatter;
    formatter.setStyle(CslCatalog::bundledStyle(QStringLiteral("apa")));
    formatter.setDatabases({root, other});
    // The chapter has its own bibliography, with the other work under the same key.
    formatter.setDatabasesFor(chapter, {other});
    const CitationOutput output = formatter.format(
        {cluster({cite(QStringLiteral("@doe2006a")), cite(QStringLiteral("@nobody"))}),
         cluster({cite(QStringLiteral("doe2006a"))}, 0, chapter)});
    QCOMPARE(marked(output.clusters[0]), QStringLiteral("(Doe, 2006; **nobody?**)"));
    QCOMPARE(marked(output.clusters[1]), QStringLiteral("(Other, 1999)"));
    // Both works are listed; neither overwrote the other.
    QCOMPARE(output.bibliography.size(), 2);
    QCOMPARE(output.bibliography[0].source, QStringLiteral("/library/root.json"));
    QCOMPARE(output.bibliography[1].source, QStringLiteral("/library/other.json"));
    QList<Code> codes;
    for (const CitationDiagnostic &d : output.diagnostics) codes.append(d.code);
    QCOMPARE(codes, (QList<Code>{Code::AmbiguousKey, Code::MissingKey}));
    QCOMPARE(output.diagnostics[0].message,
             QStringLiteral("The key “doe2006a” is in more than one bibliography: root.json is used, not other.json."));
    QCOMPARE(output.diagnostics[1].key, QStringLiteral("nobody"));
    QCOMPARE(output.diagnostics[1].cluster, 0);
    QCOMPARE(output.diagnostics[1].item, 1);
    QCOMPARE(output.diagnostics[1].message,
             QStringLiteral("The citation “nobody” is not defined in the document or its bibliography."));

    // A database's own problems come through, once.
    const Database broken = database("@book{x, title = {A}}\n@book{x, title = {B}}\n",
                                     BibliographyDatabase::Format::BibLaTeX, QStringLiteral("/library/broken.bib"));
    CitationFormatter withBroken;
    withBroken.setStyle(CslCatalog::bundledStyle(QStringLiteral("apa")));
    withBroken.setDatabases({broken});
    withBroken.setDatabasesFor(chapter, {broken});
    const CitationOutput brokenOutput = withBroken.format({cluster({cite(QStringLiteral("x"))})});
    QCOMPARE(brokenOutput.diagnostics.size(), 1);
    QCOMPARE(brokenOutput.diagnostics.first().code, Code::Database);
    QCOMPARE(brokenOutput.diagnostics.first().source, QStringLiteral("/library/broken.bib"));
    QCOMPARE(brokenOutput.diagnostics.first().line, 2);

    // Writer definitions: the citing file's own first, then the root's.
    WriterDefinition rootDefinition{QStringLiteral("#Doe:2006"), CslText(QStringLiteral("Root definition.")), {}};
    WriterDefinition localDefinition{QStringLiteral("Doe:2006"), CslText(QStringLiteral("Chapter definition.")), chapter};
    WriterDefinition duplicate{QStringLiteral("#Doe:2006"), CslText(QStringLiteral("Again.")), {}};
    CitationFormatter writer;
    const CitationOutput scoped =
        writer.format({cluster({cite(QStringLiteral("#Doe:2006"))}), cluster({cite(QStringLiteral("#Doe:2006"))}, 0, chapter)},
                      {rootDefinition, localDefinition, duplicate});
    QCOMPARE(scoped.bibliography.size(), 2);
    QCOMPARE(scoped.bibliography[0].text.toPlainText(), QStringLiteral("[1]Root definition."));
    QCOMPARE(scoped.bibliography[1].text.toPlainText(), QStringLiteral("[2]Chapter definition."));
    QCOMPARE(scoped.bibliography[1].source, chapter);
    QCOMPARE(scoped.diagnostics.size(), 1);
    QCOMPARE(scoped.diagnostics.first().code, Code::DuplicateDefinition);

    // A Writer citation without a definition falls back to the bibliography.
    CitationFormatter mixed;
    mixed.setStyle(CslCatalog::bundledStyle(QStringLiteral("apa")));
    mixed.setDatabases({root});
    const CitationOutput fallback = mixed.format({cluster({cite(QStringLiteral("#doe2006a"), QStringLiteral("p. 23"))})});
    QCOMPARE(marked(fallback.clusters.first()), QStringLiteral("(Doe, 2006, p. 23)"));
}

void CitationTests::writerDefinitions()
{
    // "[p. 23][#Doe:2006]" with "[#Doe:2006]: John Doe. *A Sample Book*.
    // Example Press, 2006.": numbered by first use, the locator after the
    // number, listed as written.
    const QList<WriterDefinition> definitions{
        {QStringLiteral("#Doe:2006"),
         CslText::fromHtml(QStringLiteral("John Doe. <i>A Sample Book</i>. Example Press, 2006.")), {}},
        {QStringLiteral("#Roe:2010"), CslText(QStringLiteral("Jane Roe. Another Book. 2010.")), {}},
        {QStringLiteral("#Unused"), CslText(QStringLiteral("Never cited.")), {}},
    };
    CitationFormatter formatter;
    const CitationOutput output = formatter.format(
        {cluster({cite(QStringLiteral("#Doe:2006"), QStringLiteral("p. 23"))}),
         cluster({cite(QStringLiteral("#Roe:2010"), QString())}), cluster({cite(QStringLiteral("#Doe:2006"))}),
         cluster({cite(QStringLiteral("#Roe:2010"), QStringLiteral("pp. 4-6"))}),
         cluster({cite(QStringLiteral("#Doe:2006"), QStringLiteral("chap. 3"))})},
        definitions);
    QStringList citations;
    for (const CslText &text : output.clusters) citations.append(marked(text));
    QCOMPARE(citations, (QStringList{QStringLiteral("[1, p. 23]"), QStringLiteral("[2]"), QStringLiteral("[1]"),
                                     QStringLiteral("[2, pp. 4–6]"), QStringLiteral("[1, chap. 3]")}));
    QCOMPARE(output.bibliography.size(), 2);
    QCOMPARE(marked(output.bibliography[0].text), QStringLiteral("[1] John Doe. *A Sample Book*. Example Press, 2006."));
    QCOMPARE(output.bibliography[0].number, 1);
    QCOMPARE(output.bibliography[0].key, QStringLiteral("Doe:2006"));
    QCOMPARE(marked(output.bibliography[1].text), QStringLiteral("[2] Jane Roe. Another Book. 2010."));
    QVERIFY(output.diagnostics.isEmpty());
    QCOMPARE(output.bibliography[0].text.toHtml(),
             QStringLiteral("<div class=\"csl-left-margin\">[1]</div><div class=\"csl-right-inline\">John Doe. <i>A Sample "
                            "Book</i>. Example Press, 2006.</div>"));

    // With a numeric style for database citations, the two numberings are separate.
    CitationFormatter numeric;
    numeric.setStyle(CslCatalog::bundledStyle(QStringLiteral("ieee")));
    numeric.setDatabases({database(works())});
    const CitationOutput both = numeric.format(
        {cluster({cite(QStringLiteral("@smith2019")), cite(QStringLiteral("#Doe:2006"), QStringLiteral("p. 2"))})},
        definitions);
    QCOMPARE(marked(both.clusters.first()), QStringLiteral("[1] [1, p. 2]"));
    QCOMPARE(both.diagnostics.size(), 1);
    QCOMPARE(both.diagnostics.first().code, CitationDiagnostic::Code::SeparateNumbering);
}

void CitationTests::formattingOffTheUiThread()
{
    // Formatting reads shared styles and databases and changes nothing, so
    // workers can run it together.
    const Database data = database(works());
    const QList<CitationClusterSpec> clusters = authorDateCitations();
    const Formatted expected = formatBundled(QStringLiteral("chicago-author-date"), {data}, clusters);
    QList<QFuture<QStringList>> futures;
    for (int i = 0; i < 8; ++i) {
        futures.append(QtConcurrent::run([data, clusters, i] {
            const QString id = i % 2 ? QStringLiteral("chicago-author-date") : QStringLiteral("apa");
            const Formatted result = formatBundled(id, {data}, clusters);
            return result.citations + result.bibliography;
        }));
    }
    const Formatted apa = formatBundled(QStringLiteral("apa"), {data}, clusters);
    for (int i = 0; i < futures.size(); ++i) {
        const QStringList result = futures[i].result();
        QCOMPARE(result, i % 2 ? expected.citations + expected.bibliography : apa.citations + apa.bibliography);
    }
}

void CitationTests::cslTestSuite()
{
    // Fixtures from the CSL test suite (citation-style-language/test-suite,
    // processor-tests/humans), with the ones parch does not yet pass listed.
    const QDir folder(suiteFolder());
    const QStringList fixtures = folder.entryList({QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
    if (fixtures.isEmpty()) QSKIP("Run bin/fetch-test-fixtures to download the CSL test suite's fixtures");
    QVERIFY(fixtures.size() > 500);
    QFile list(QDir(suiteListFolder()).filePath(QStringLiteral("known-failures")));
    QVERIFY(list.open(QIODevice::ReadOnly));
    QSet<QString> known;
    for (const QByteArray &line : list.readAll().split('\n')) {
        const QString name = QString::fromUtf8(line).trimmed();
        if (!name.isEmpty() && !name.startsWith(u'#')) known.insert(name);
    }
    int passed = 0;
    QStringList unexpected;
    QStringList fixed;
    for (const QString &name : fixtures) {
        const QString base = QFileInfo(name).completeBaseName();
        const CslFixture::Outcome outcome = CslFixture::run(folder.filePath(name));
        if (outcome.passed) {
            ++passed;
            if (known.contains(base)) fixed.append(base);
        } else if (!known.contains(base)) {
            unexpected.append(QStringLiteral("%1\n  expected: %2\n  actual:   %3%4")
                                  .arg(base, outcome.expected, outcome.actual,
                                       outcome.error.isEmpty() ? QString() : QStringLiteral("\n  error: ") + outcome.error));
        }
    }
    qInfo("CSL test suite: %d of %d fixtures pass (%.1f%%)", passed, int(fixtures.size()),
          100.0 * passed / fixtures.size());
    if (!fixed.isEmpty()) qInfo("Now passing, no longer known failures: %s", qPrintable(fixed.join(u' ')));
    QVERIFY2(unexpected.isEmpty(), qPrintable(unexpected.join(u'\n')));
}
