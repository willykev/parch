#include "settingstests.h"

#include "apppaths.h"
#include "attachments.h"
#include "backend.h"
#include "cli.h"
#include "clicommands.h"
#include "dialectchoice.h"
#include "documentcheck.h"
#include "documentcitations.h"
#include "documentreading.h"
#include "documentsettings.h"
#include "documentsettingsstore.h"
#include "editordecorations.h"
#include "fileorganizer.h"
#include "markdownstyling.h"
#include "mdhtml.h"
#include "mdinclude.h"
#include "outline.h"
#include "spellchecker.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"
#include "wordcounter.h"

#include <QBuffer>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

namespace SettingsTests {
namespace {

QString canonicalPath(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

// Two works, in each of the bibliography formats parch reads.
const char *bibtex = R"(@book{doe2006,
  author    = {Doe, John},
  title     = {Writing with examples},
  publisher = {Example Press},
  year      = {2006}
}

@book{roe2010,
  author    = {Roe, Jane},
  title     = {Reading without examples},
  publisher = {Sample House},
  year      = {2010}
}
)";

const char *cslJson = R"([
  {"id": "doe2006", "type": "book", "author": [{"family": "Doe", "given": "John"}],
   "title": "Writing with examples", "publisher": "Example Press", "issued": {"date-parts": [[2006]]}},
  {"id": "roe2010", "type": "book", "author": [{"family": "Roe", "given": "Jane"}],
   "title": "Reading without examples", "publisher": "Sample House", "issued": {"date-parts": [[2010]]}}
]
)";

const char *cslYaml = R"(references:
- id: doe2006
  type: book
  author:
  - family: Doe
    given: John
  title: Writing with examples
  publisher: Example Press
  issued:
    date-parts:
    - [2006]
- id: roe2010
  type: book
  author:
  - family: Roe
    given: Jane
  title: Reading without examples
  publisher: Sample House
  issued:
    date-parts:
    - [2010]
)";

// A style of the writer's own: "<<Doe 2006>>", and entries "Doe, John: Title".
const char *ownStyle = R"(<?xml version="1.0" encoding="utf-8"?>
<style xmlns="http://purl.org/net/xbiblio/csl" class="in-text" version="1.0" default-locale="en-US">
  <info>
    <title>A writer's own style</title>
    <id>http://example.org/styles/own</id>
    <updated>2024-01-01T00:00:00+00:00</updated>
  </info>
  <citation>
    <layout prefix="&lt;&lt;" suffix="&gt;&gt;" delimiter="; ">
      <names variable="author"><name form="short"/></names>
      <date variable="issued" prefix=" "><date-part name="year"/></date>
    </layout>
  </citation>
  <bibliography>
    <layout>
      <names variable="author"><name name-as-sort-order="all"/></names>
      <text variable="title" prefix=": "/>
    </layout>
  </bibliography>
</style>
)";

// What a command printed and how it ended, run in the test's process.
struct Ran {
    int code = -1;
    QString out;
    QString err;
    QJsonObject json() const { return QJsonDocument::fromJson(out.toUtf8()).object(); }
    QString said() const { return QStringLiteral("exit %1\n--- out\n%2\n--- err\n%3").arg(code).arg(out, err); }
};

Ran runCli(const QString &folder, const AppPaths &paths, QStringList arguments)
{
    QBuffer out, err;
    out.open(QIODevice::WriteOnly);
    err.open(QIODevice::WriteOnly);
    const Cli::Context context{folder, folder + QStringLiteral("/home"), paths, &out, &err};
    arguments.prepend(QStringLiteral("parch"));
    Ran ran;
    ran.code = Cli::run(arguments, context);
    ran.out = QString::fromUtf8(out.data());
    ran.err = QString::fromUtf8(err.data());
    return ran;
}

// A document read as every output reads it, from a file.
DocumentReading::Reading readFileIn(const QString &path, const Dialect::Layers &layers = {})
{
    md::FileIncludeResolver resolver;
    const md::IncludeResolver::File file = resolver.read(path, 1 << 20);
    md::Source source;
    source.path = file.canonicalPath;
    source.resolver = &resolver;
    return DocumentReading::read(file.text, layers, source);
}

QString htmlOf(const DocumentReading::Reading &reading)
{
    return md::toHtml(reading.document, reading.htmlOptions());
}

QStringList citationTexts(const DocumentCitations::Formatted &formatted)
{
    QStringList texts;
    for (const DocumentCitations::Formatted::Citation &citation : formatted.citations)
        texts.append(citation.text.toPlainText());
    return texts;
}

QStringList bibliographyTexts(const DocumentCitations::Formatted &formatted)
{
    QStringList texts;
    for (const CitationBibliographyEntry &entry : formatted.bibliography) texts.append(entry.text.toPlainText());
    return texts;
}

QStringList problemCodes(const QList<DocumentCitations::Problem> &problems)
{
    QStringList codes;
    for (const DocumentCitations::Problem &problem : problems) codes.append(problem.code);
    return codes;
}

bool run(QQuickWindow *window, const char *command)
{
    return QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
}

QStringList headingTexts(const QList<Heading> &headings)
{
    QStringList texts;
    for (const Heading &heading : headings) texts.append(heading.text);
    return texts;
}

std::vector<DecorationSpan> joinedRuns(EditorDecorations *decorations)
{
    std::vector<DecorationSpan> joined;
    for (const DecorationSpan &span : decorations->spans(MarkdownStyling::layerName())) {
        if (!joined.empty() && joined.back().end == span.start && joined.back().style == span.style)
            joined.back().end = span.end;
        else
            joined.push_back(span);
    }
    return joined;
}

QString shownTextOf(QObject *item)
{
    return item ? item->property("text").toString() : QStringLiteral("(missing)");
}

} // namespace

void layersFollowTheirPrecedence()
{
    // Each value is that of the highest layer that names it: the command
    // line, the document, its front matter, its library folder, every
    // document, and then the defaults.
    Dialect::Layers layers;
    QCOMPARE(Dialect::effective(layers).options, md::Options::writer());
    QCOMPARE(Dialect::effective(layers).profileFrom, QStringLiteral("default"));
    QCOMPARE(Dialect::effective(layers).bibliographyFrom, QStringLiteral("default"));
    QVERIFY(!Dialect::effective(layers).savedSettings);

    layers.application.profile = md::Profile::Standard;
    layers.application.citationStyle = QStringLiteral("apa");
    layers.application.bibliography = QStringList{QStringLiteral("/app.bib")};
    layers.application.extensions = {{QStringLiteral("math"), true}};
    Dialect::Choice choice = Dialect::effective(layers);
    QCOMPARE(choice.options.profile, md::Profile::Standard);
    QCOMPARE(choice.profileFrom, QStringLiteral("application"));
    QVERIFY(choice.options.math);
    QCOMPARE(choice.extensionsFrom.value(QStringLiteral("math")), QStringLiteral("application"));
    QCOMPARE(choice.citationStyle, QStringLiteral("apa"));
    QCOMPARE(choice.citationStyleFrom, QStringLiteral("application"));
    QVERIFY(choice.savedSettings);

    layers.library.profile = md::Profile::Writer;
    layers.library.libraryRoot = QStringLiteral("/library");
    layers.library.citationStyle = QStringLiteral("ieee");
    layers.library.extensions = {{QStringLiteral("math"), false}};
    choice = Dialect::effective(layers);
    QCOMPARE(choice.options.profile, md::Profile::Writer);
    QCOMPARE(choice.profileFrom, QStringLiteral("library"));
    QVERIFY(!choice.options.math);
    QCOMPARE(choice.extensionsFrom.value(QStringLiteral("math")), QStringLiteral("library"));
    QCOMPARE(choice.citationStyle, QStringLiteral("ieee"));
    QCOMPARE(choice.libraryRootFrom, QStringLiteral("library"));
    QCOMPARE(choice.bibliography, QStringList{QStringLiteral("/app.bib")});

    // The front matter names a bibliography and a style, over the library's.
    layers.frontMatter.bibliography = QStringList{QStringLiteral("/front.bib")};
    layers.frontMatter.citationStyle = QStringLiteral("chicago-author-date");
    choice = Dialect::effective(layers);
    QCOMPARE(choice.bibliography, QStringList{QStringLiteral("/front.bib")});
    QCOMPARE(choice.bibliographyFrom, QStringLiteral("front-matter"));
    QCOMPARE(choice.citationStyleFrom, QStringLiteral("front-matter"));

    // The document's own settings win over its front matter; a list is
    // chosen whole, so an empty one chooses no bibliography at all.
    layers.document.bibliography = QStringList();
    layers.document.citationStyle = QStringLiteral("modern-language-association");
    layers.document.extensions = {{QStringLiteral("math"), true}, {QStringLiteral("footnotes"), false}};
    choice = Dialect::effective(layers);
    QVERIFY(choice.bibliography.isEmpty());
    QCOMPARE(choice.bibliographyFrom, QStringLiteral("document"));
    QCOMPARE(choice.citationStyle, QStringLiteral("modern-language-association"));
    // Math is back to the Writer profile's own; footnotes are off by the document.
    QVERIFY(choice.options.math);
    QVERIFY(!choice.extensionsFrom.contains(QStringLiteral("math")));
    QVERIFY(!choice.options.footnotes);
    QCOMPARE(choice.extensionsFrom.value(QStringLiteral("footnotes")), QStringLiteral("document"));

    // The command line wins over everything; an extension switched back to
    // the profile's own comes from nowhere in particular.
    layers.flags.profile = md::Profile::Standard;
    layers.flags.extensions = {{QStringLiteral("math"), false}};
    layers.flags.citationStyle = QStringLiteral("apa");
    choice = Dialect::effective(layers);
    QCOMPARE(choice.profileFrom, QStringLiteral("flag"));
    QVERIFY(!choice.options.math);
    QVERIFY(!choice.extensionsFrom.contains(QStringLiteral("math")));
    QCOMPARE(choice.citationStyleFrom, QStringLiteral("flag"));

    // JSON says all of it.
    const QJsonObject json = choice.toJson();
    QCOMPARE(json.value(QStringLiteral("citationStyle")).toString(), QStringLiteral("apa"));
    QCOMPARE(json.value(QStringLiteral("bibliography")).toArray().size(), 0);
    QCOMPARE(json.value(QStringLiteral("from")).toObject().value(QStringLiteral("bibliography")).toString(),
             QStringLiteral("document"));
    QCOMPARE(json.value(QStringLiteral("from")).toObject().value(QStringLiteral("citationStyle")).toString(),
             QStringLiteral("flag"));
    QVERIFY(choice.describe().contains(QStringLiteral("style apa")));
    // The three-layer form of the command line is the same choice.
    QCOMPARE(Dialect::choose(layers.flags, layers.document, layers.library).options,
             Dialect::effective({layers.flags, layers.document, {}, layers.library, {}}).options);
}

void frontMatterChoosesBibliographyAndStyle()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = canonicalPath(folder.path());
    writeFile(base + QStringLiteral("/mine.csl"), ownStyle);
    const auto layerOf = [&](const QString &text) {
        return Dialect::frontMatterLayer(md::parseFrontMatter(text), base);
    };
    // One file, or a list, found from the document's folder.
    Dialect::Layer layer = layerOf(QStringLiteral("---\nbibliography: refs.bib\n---\nText\n"));
    QCOMPARE(layer.bibliography.value_or(QStringList()), QStringList{base + QStringLiteral("/refs.bib")});
    layer = layerOf(QStringLiteral("---\nbibliography:\n  - a.bib\n  - /abs/b.json\ncsl: apa\ntitle: Kept\n---\n"));
    QCOMPARE(layer.bibliography.value_or(QStringList()),
             (QStringList{base + QStringLiteral("/a.bib"), QStringLiteral("/abs/b.json")}));
    QCOMPARE(layer.citationStyle, QStringLiteral("apa"));
    // Nothing else is read from it: no profile, no extensions.
    QVERIFY(!layer.profile);
    QVERIFY(layer.extensions.isEmpty());
    // A style file beside the document, a bundled style by another name, and
    // a bundled style asked for as a file that is not there.
    QCOMPARE(layerOf(QStringLiteral("---\ncsl: mine.csl\n---\n")).citationStyle, base + QStringLiteral("/mine.csl"));
    QCOMPARE(layerOf(QStringLiteral("---\ncsl: Vancouver\n---\n")).citationStyle, QStringLiteral("nlm-citation-sequence"));
    QCOMPARE(layerOf(QStringLiteral("---\ncsl: ieee.csl\n---\n")).citationStyle, QStringLiteral("ieee"));
    QCOMPARE(layerOf(QStringLiteral("---\ncsl: styles/missing.csl\n---\n")).citationStyle,
             base + QStringLiteral("/styles/missing.csl"));
    // Without front matter, nothing.
    QVERIFY(layerOf(QStringLiteral("bibliography: refs.bib\n")).isEmpty());
    QVERIFY(Dialect::linkCitations(md::parseFrontMatter(u"---\nlink-citations: true\n---\n")));
    QVERIFY(!Dialect::linkCitations(md::parseFrontMatter(u"---\nlink-citations: false\n---\n")));

    // A profile that reads no front matter takes none of it.
    const QString text = QStringLiteral("---\nbibliography: refs.bib\ncsl: apa\n---\nText\n");
    Dialect::Layers layers;
    QCOMPARE(Dialect::effective(DocumentReading::withFrontMatter(layers, text, base)).citationStyle, QStringLiteral("apa"));
    layers.document.profile = md::Profile::Standard;
    QVERIFY(Dialect::effective(DocumentReading::withFrontMatter(layers, text, base)).citationStyle.isEmpty());
    layers.document.extensions = {{QStringLiteral("front-matter"), true}};
    QCOMPARE(Dialect::effective(DocumentReading::withFrontMatter(layers, text, base)).citationStyle, QStringLiteral("apa"));

    // Reading a document honours its front matter and changes nothing in it.
    writeFile(base + QStringLiteral("/refs.bib"), bibtex);
    const QString source = QStringLiteral("---\ntitle: Kept as it is\nbibliography: refs.bib\ncsl: apa\n"
                                          "unknown: [1, 2]\n---\n\nAs shown [@doe2006, p. 23].\n");
    writeFile(base + QStringLiteral("/doc.md"), source.toUtf8());
    const DocumentReading::Reading reading = readFileIn(base + QStringLiteral("/doc.md"));
    QCOMPARE(reading.dialect.bibliographyFrom, QStringLiteral("front-matter"));
    QCOMPARE(citationTexts(reading.formatted), QStringList{QStringLiteral("(Doe, 2006, p. 23)")});
    QCOMPARE(readFile(base + QStringLiteral("/doc.md")), source.toUtf8());
    QCOMPARE(reading.document.frontMatter().scalar(u"title").value_or(QString()), QStringLiteral("Kept as it is"));
}

void settingsAreKeptBesideTheDocuments()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const AppPaths paths = AppPaths::under(folder.path());
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        DocumentSettings::Store store(paths.documentSettingsFile());
        QVERIFY(store.load());
        Dialect::Layer layer;
        layer.profile = md::Profile::Standard;
        layer.extensions = {{QStringLiteral("math"), true}, {QStringLiteral("footnotes"), false}};
        layer.bibliography = QStringList{QStringLiteral("/refs/a.bib"), QStringLiteral("/refs/b.yaml")};
        layer.citationStyle = QStringLiteral("/styles/own.csl");
        layer.libraryRoot = QStringLiteral("/books");
        QVERIFY(store.setDocument(id, layer));
        Dialect::Layer library;
        library.citationStyle = QStringLiteral("ieee");
        QVERIFY(store.setLibrary(QStringLiteral("/library"), library));
        Dialect::Layer application;
        application.profile = md::Profile::Writer;
        application.libraryRoot = QStringLiteral("/never"); // not a setting for every document
        QVERIFY(store.setApplication(application));
        // A document id is a UUID, a library a full path.
        QVERIFY(!store.setDocument(QStringLiteral("../escape"), layer));
        QVERIFY(!store.setLibrary(QStringLiteral("relative"), library));
    }
    // The file is the format the command line reads.
    const QJsonObject json = QJsonDocument::fromJson(readFile(paths.documentSettingsFile())).object();
    const QJsonObject entry = json.value(QStringLiteral("documents")).toObject().value(id).toObject();
    QCOMPARE(entry.value(QStringLiteral("profile")).toString(), QStringLiteral("standard"));
    QCOMPARE(entry.value(QStringLiteral("extensions")).toObject().value(QStringLiteral("math")).toBool(), true);
    QCOMPARE(entry.value(QStringLiteral("extensions")).toObject().value(QStringLiteral("footnotes")).toBool(true), false);
    QCOMPARE(entry.value(QStringLiteral("bibliography")).toArray().size(), 2);
    QCOMPARE(entry.value(QStringLiteral("citationStyle")).toString(), QStringLiteral("/styles/own.csl"));
    QCOMPARE(entry.value(QStringLiteral("libraryRoot")).toString(), QStringLiteral("/books"));
    QCOMPARE(json.value(QStringLiteral("libraries")).toObject().value(QStringLiteral("/library")).toObject().value(
                 QStringLiteral("citationStyle")).toString(),
             QStringLiteral("ieee"));
    QVERIFY(!json.value(QStringLiteral("application")).toObject().contains(QStringLiteral("libraryRoot")));

    // Read again, it is the same; what the file holds that parch does not
    // know stays when parch writes it back.
    QJsonObject withUnknown = json;
    QJsonObject documents = withUnknown.value(QStringLiteral("documents")).toObject();
    QJsonObject kept = documents.value(id).toObject();
    kept.insert(QStringLiteral("future"), QStringLiteral("kept"));
    documents.insert(id, kept);
    withUnknown.insert(QStringLiteral("documents"), documents);
    withUnknown.insert(QStringLiteral("later"), 42);
    writeFile(paths.documentSettingsFile(), QJsonDocument(withUnknown).toJson());
    DocumentSettings::Store store(paths.documentSettingsFile());
    QVERIFY(store.load());
    QCOMPARE(store.document(id).profile, md::Profile::Standard);
    QCOMPARE(store.document(id).bibliography.value_or(QStringList()).size(), 2);
    QCOMPARE(store.library(QStringLiteral("/library")).citationStyle, QStringLiteral("ieee"));
    QCOMPARE(store.libraryFolders(), QStringList{QStringLiteral("/library")});
    Dialect::Layer changed = store.document(id);
    changed.profile.reset();
    QVERIFY(store.setDocument(id, changed));
    const QJsonObject again = QJsonDocument::fromJson(readFile(paths.documentSettingsFile())).object();
    QCOMPARE(again.value(QStringLiteral("later")).toInt(), 42);
    const QJsonObject keptEntry = again.value(QStringLiteral("documents")).toObject().value(id).toObject();
    QCOMPARE(keptEntry.value(QStringLiteral("future")).toString(), QStringLiteral("kept"));
    QVERIFY(!keptEntry.contains(QStringLiteral("profile")));
    // An empty layer takes its entry away.
    QVERIFY(store.setLibrary(QStringLiteral("/library"), {}));
    QVERIFY(!QJsonDocument::fromJson(readFile(paths.documentSettingsFile())).object().contains(QStringLiteral("libraries")));

    // What a document beneath a library folder is given: its own, the
    // nearest library's (whose folder is its library root), every document's.
    QVERIFY(store.setLibrary(QStringLiteral("/library/notes"), {{}, {}, {}, QStringList{}, QStringLiteral("apa")}));
    DocumentSettings::Saved saved =
        store.savedFor(id, QStringLiteral("/library/notes/day.md"), {QStringLiteral("/library")});
    QCOMPARE(saved.libraryFolder, QStringLiteral("/library/notes"));
    QCOMPARE(saved.library.citationStyle, QStringLiteral("apa"));
    QCOMPARE(saved.library.libraryRoot, QStringLiteral("/library/notes"));
    QCOMPARE(saved.document.libraryRoot, QStringLiteral("/books"));
    QCOMPARE(saved.application.profile, md::Profile::Writer);
    saved = store.savedFor(QString(), QStringLiteral("/elsewhere/day.md"), {QStringLiteral("/library")});
    QVERIFY(saved.libraryFolder.isEmpty() && saved.library.isEmpty() && saved.document.isEmpty());

    // The command line's reader takes the same, every document's layer too.
    writeFile(paths.registryFile(), QJsonDocument(QJsonObject{{QStringLiteral("/library/notes/day.md"), id}}).toJson());
    DocumentSettings::Saved read;
    QString error;
    QVERIFY(DocumentSettings::read(paths, QStringLiteral("/library/notes/day.md"), &read, &error));
    QCOMPARE(read.document.bibliography.value_or(QStringList()).size(), 2);
    QCOMPARE(read.library.citationStyle, QStringLiteral("apa"));
    QCOMPARE(read.application.profile, md::Profile::Writer);

    // A file that cannot be read is never written over: the first change
    // moves it aside.
    writeFile(paths.documentSettingsFile(), "{not json");
    DocumentSettings::Store damaged(paths.documentSettingsFile());
    QVERIFY(!damaged.load(&error));
    QVERIFY(error.contains(QStringLiteral("document-settings.json")));
    QVERIFY(damaged.document(id).isEmpty());
    QVERIFY(damaged.setApplication({md::Profile::Standard, {}, {}, {}, {}}));
    QCOMPARE(readFile(paths.documentSettingsFile() + QStringLiteral(".bad")), QByteArray("{not json"));
    QCOMPARE(QJsonDocument::fromJson(readFile(paths.documentSettingsFile()))
                 .object()
                 .value(QStringLiteral("application"))
                 .toObject()
                 .value(QStringLiteral("profile"))
                 .toString(),
             QStringLiteral("standard"));
}

void openDocumentsFollowTheirSettings()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonicalPath(files.path());
    const QString library = root + QStringLiteral("/library");
    const QString inside = library + QStringLiteral("/inside.md");
    const QString outside = root + QStringLiteral("/outside.md");
    writeFile(inside, "Inside.\n");
    writeFile(outside, "Outside.\n");
    const QByteArray settings = QJsonDocument(QJsonObject{{QStringLiteral("libraryFolders"), QJsonArray{library}}}).toJson();
    ShellFixture shell({.settings = settings, .files = {outside, inside}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    DocumentSettingsStore *store = shell.backend->documentSettings();
    QVERIFY(store);
    Document *in = shell.document();
    QCOMPARE(in->path(), inside);
    TabModel *tabs = shell.context()->tabs();
    Document *out = tabs->documentAt(tabs->indexOf(in) == 0 ? 1 : 0);
    QVERIFY(out && out->path() == outside);
    // At first the defaults; the library folder is the root of `/rooted` includes.
    QCOMPARE(in->markdownOptions(), md::Options::writer());
    QCOMPARE(in->libraryRoot(), library);
    QVERIFY(out->libraryRoot().isEmpty());
    QCOMPARE(store->libraryFolderOf(in), library);

    // Every document's setting reaches both, at once.
    QSignalSpy inChanged(in, &Document::dialectChanged), outChanged(out, &Document::dialectChanged);
    QVERIFY(store->setExtension(nullptr, QStringLiteral("application"), QStringLiteral("math"), QStringLiteral("off")));
    QCOMPARE(inChanged.count(), 1);
    QCOMPARE(outChanged.count(), 1);
    QVERIFY(!in->markdownOptions().math && !out->markdownOptions().math);
    // The library's reaches the document in it alone.
    QVERIFY(store->setProfile(in, QStringLiteral("library"), QStringLiteral("standard")));
    QCOMPARE(in->markdownOptions().profile, md::Profile::Standard);
    QCOMPARE(out->markdownOptions().profile, md::Profile::Writer);
    QCOMPARE(outChanged.count(), 1);
    // A document outside the library has none to change.
    QSignalSpy problems(store, &DocumentSettingsStore::problem);
    QVERIFY(!store->setProfile(out, QStringLiteral("library"), QStringLiteral("standard")));
    QCOMPARE(problems.count(), 1);
    // The document's own wins over its library's.
    QVERIFY(store->setProfile(in, QStringLiteral("document"), QStringLiteral("writer")));
    QCOMPARE(in->markdownOptions().profile, md::Profile::Writer);
    QCOMPARE(in->savedDialect().document.profile, md::Profile::Writer);
    QCOMPARE(in->savedDialect().library.profile, md::Profile::Standard);
    QVERIFY(store->setLibraryRoot(in, QStringLiteral("document"), root));
    QCOMPARE(in->libraryRoot(), root);
    // Unknown names change nothing.
    QVERIFY(!store->setProfile(in, QStringLiteral("document"), QStringLiteral("gfm")));
    QVERIFY(!store->setExtension(in, QStringLiteral("document"), QStringLiteral("nothing"), QStringLiteral("on")));
    QVERIFY(!store->setCitationStyle(in, QStringLiteral("document"), QStringLiteral("no-such-style")));
    QVERIFY(!store->setLibraryRoot(nullptr, QStringLiteral("application"), root));

    // A draft has settings of its own before it has a file.
    shell.backend->newDraft();
    Document *draft = shell.document();
    QVERIFY(draft->isDraft());
    QVERIFY(!draft->markdownOptions().math);
    QVERIFY(store->setExtension(draft, QStringLiteral("document"), QStringLiteral("math"), QStringLiteral("on")));
    QVERIFY(draft->markdownOptions().math);
    QVERIFY(store->libraryFolderOf(draft).isEmpty());

    // Kept: a new run reads them back for the same documents.
    const AppPaths paths = shell.paths();
    DocumentSettings::Store kept(paths.documentSettingsFile());
    QVERIFY(kept.load());
    QCOMPARE(kept.document(in->id()).profile, md::Profile::Writer);
    QCOMPARE(kept.library(library).profile, md::Profile::Standard);
    QCOMPARE(kept.document(draft->id()).extensions.size(), 1);

    // A folder with settings of its own keeps them for the documents in it
    // when it leaves the library's list, as the command line reads them.
    shell.settings()->setLibraryFolders({});
    QCOMPARE(in->savedDialect().library.profile, md::Profile::Standard);
    QCOMPARE(in->libraryRoot(), root);
    // Settings that change while the document is open reach it as they change.
    QVERIFY(store->setProfile(in, QStringLiteral("document"), QString()));
    QCOMPARE(in->markdownOptions().profile, md::Profile::Standard);
    QVERIFY(store->setProfile(in, QStringLiteral("library"), QString()));
    QCOMPARE(in->markdownOptions().profile, md::Profile::Writer);
}

void profileChangesEveryReading()
{
    // A document with Writer constructs. Read in the Standard profile, the
    // outline, the word count, the prose spelling reads, live formatting,
    // Check links and the command line all read it otherwise, at once.
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonicalPath(files.path());
    const QString path = root + QStringLiteral("/doc.md");
    const QString text = QStringLiteral("---\ntitle: Notes\n---\n\n"
                                        "# Start [Opening]\n\n"
                                        "// a private zzqx comment line\n\n"
                                        "Some ==marked== words.\n\n"
                                        "chapters/missing.md\n");
    writeFile(path, text.toUtf8());
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    Document *document = shell.document();
    QVERIFY(document && document->path() == path);
    DocumentSettingsStore *store = shell.backend->documentSettings();

    // The outline, as the library and Go to heading show it.
    OutlineModel outline;
    outline.setDocument(document);
    outline.setActive(true);
    QTRY_VERIFY(outline.ready());
    QCOMPARE(headingTexts(outline.headings()), QStringList{QStringLiteral("Start")});
    // The word count in the status line.
    auto *counter = qobject_cast<WordCounter *>(findObject(window, QStringLiteral("wordCounter")));
    QVERIFY(counter);
    QTRY_COMPARE(counter->words(), 4);
    // Live formatting.
    auto *decorations = qobject_cast<EditorDecorations *>(findObject(window, QStringLiteral("decorations")));
    auto *styling = qobject_cast<MarkdownStyling *>(findObject(window, QStringLiteral("styling")));
    QVERIFY(decorations && styling);
    const auto styled = [&](const md::Options &options) {
        return QTest::qWaitFor(
            [&] {
                decorations->flush();
                return !styling->busy() && joinedRuns(decorations) == markdownStyleRuns(md::parse(text, options));
            },
            5000);
    };
    QVERIFY(styled(md::Options::writer()));
    // Check links reads the include.
    Attachments *attachments = shell.backend->attachments();
    const auto missingLinks = [&] {
        QSignalSpy checked(attachments, &Attachments::linksChecked);
        attachments->checkLinks(document);
        if (!checked.wait(5000)) return -1;
        return int(checked.last().at(1).toList().size());
    };
    QCOMPARE(missingLinks(), 1);
    // Spelling's prose leaves the comment out.
    const auto prose = [&] {
        QString words;
        for (const md::Range range : spellingProse(text, document->markdownOptions()))
            words += QStringView(text).sliced(range.start, range.length()).toString();
        return words;
    };
    QVERIFY(!prose().contains(QStringLiteral("zzqx")));

    QSignalSpy changed(document, &Document::dialectChanged);
    QVERIFY(store->setProfile(document, QStringLiteral("document"), QStringLiteral("standard")));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(document->markdownOptions(), md::Options::standard());
    // Front matter is a thematic break and a Setext heading now, and the
    // label is part of the heading's text.
    QTRY_COMPARE(headingTexts(outline.headings()),
                 (QStringList{QStringLiteral("title: Notes"), QStringLiteral("Start [Opening]")}));
    // The comment line is words now.
    QTRY_VERIFY(counter->words() > 10);
    QVERIFY(styled(md::Options::standard()));
    QVERIFY(markdownStyleRuns(md::parse(text, md::Options::standard())) != markdownStyleRuns(md::parse(text)));
    QCOMPARE(missingLinks(), 0);
    QVERIFY(prose().contains(QStringLiteral("zzqx")));
    // The command line reads it so too, when asked to use saved settings.
    const Ran saved = runCli(root, shell.paths(), {"render", path, "--to", "outline", "--use-saved-settings"});
    QCOMPARE(saved.code, 0);
    QCOMPARE(saved.out, QStringLiteral("  title: Notes\nStart [Opening]\n"));
    const Ran unasked = runCli(root, shell.paths(), {"render", path, "--to", "outline"});
    QCOMPARE(unasked.out, QStringLiteral("Start\n"));

    // And back.
    QVERIFY(store->setProfile(document, QStringLiteral("document"), QString()));
    QTRY_COMPARE(headingTexts(outline.headings()), QStringList{QStringLiteral("Start")});
    QTRY_COMPARE(counter->words(), 4);
    QVERIFY(styled(md::Options::writer()));
}

void linkUpdatesReadEachFileInItsDialect()
{
    // Moving a document updates the links to it, as each file that has one
    // is read in its own dialect: a path alone in a paragraph is an include
    // in the Writer profile and plain text in Standard.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString base = canonicalPath(root.path());
    const QString library = base + QStringLiteral("/Library");
    const AppPaths paths = AppPaths::under(base + QStringLiteral("/app"));
    const QByteArray book = "# Book\n\nplan.md \"The plan\"\n";
    writeFile(library + QStringLiteral("/book.md"), book);
    writeFile(library + QStringLiteral("/standard/book.md"), "# Book\n\n../plan.md \"The plan\"\n");
    writeFile(library + QStringLiteral("/plan.md"), "Plan.\n");
    QVERIFY(QDir().mkpath(library + QStringLiteral("/archive")));
    writeFile(paths.documentSettingsFile(),
              QJsonDocument(QJsonObject{{QStringLiteral("libraries"),
                                         QJsonObject{{library + QStringLiteral("/standard"),
                                                      QJsonObject{{QStringLiteral("profile"), QStringLiteral("standard")}}}}}})
                  .toJson());
    Backend backend(paths);
    backend.start({});
    backend.settings()->setLibraryFolders({library});
    FileOrganizer *organizer = backend.organizer();
    organizer->move(library + QStringLiteral("/plan.md"), library + QStringLiteral("/archive"));
    QTRY_VERIFY(!organizer->review().isEmpty());
    QCOMPARE(organizer->review().value(QStringLiteral("files")).toInt(), 1);
    organizer->confirmMove(true);
    QTRY_VERIFY(!organizer->busy());
    QCOMPARE(readFile(library + QStringLiteral("/book.md")), QByteArray("# Book\n\narchive/plan.md \"The plan\"\n"));
    QCOMPARE(readFile(library + QStringLiteral("/standard/book.md")), QByteArray("# Book\n\n../plan.md \"The plan\"\n"));
}

void documentSettingsDialogShowsSources()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonicalPath(files.path());
    const QString library = root + QStringLiteral("/library");
    const QString path = library + QStringLiteral("/essay.md");
    writeFile(path, "---\nbibliography: refs.bib\n---\n\n# Essay\n\nAs shown [@doe2006].\n");
    writeFile(library + QStringLiteral("/refs.bib"), bibtex);
    writeFile(root + QStringLiteral("/other.bib"), bibtex);
    const QByteArray settings = QJsonDocument(QJsonObject{{QStringLiteral("libraryFolders"), QJsonArray{library}}}).toJson();
    ShellFixture shell({.settings = settings, .files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    Document *document = shell.document();
    DocumentSettingsStore *store = shell.backend->documentSettings();
    QVERIFY(store->setCitationStyle(nullptr, QStringLiteral("application"), QStringLiteral("apa")));

    // A command of the registry, which needs a document, in the menu.
    CommandRegistry *commands = shell.backend->commands();
    QCOMPARE(commands->title(QStringLiteral("documentSettings")), QStringLiteral("Document settings…"));
    QCOMPARE(commands->needs(QStringLiteral("documentSettings")), QStringLiteral("document"));
    QObject *entry = shell.object(QStringLiteral("documentSettingsEntry"));
    QVERIFY(entry && entry->property("enabled").toBool());
    QObject *dialog = shell.object(QStringLiteral("documentSettingsDialog"));
    QVERIFY(dialog);
    QVERIFY(run(window, "documentSettings"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QCOMPARE(dialog->property("title").toString(), QStringLiteral("Document settings for “essay.md”"));

    // Each value, and where it comes from. Preferences has a form of its
    // own, so the dialog's controls are found in the dialog's form.
    QQuickItem *form = shell.item(QStringLiteral("documentSettingsForm"));
    QVERIFY(form);
    const auto inForm = [&](const char *name) { return findItem(form, QString::fromLatin1(name)); };
    const auto shown = [&](const char *name) { return shownTextOf(inForm(name)); };
    QCOMPARE(inForm("profileChoice")->property("currentIndex").toInt(), 0);
    QCOMPARE(shown("profileSource"), QStringLiteral("The default"));
    QCOMPARE(inForm("styleChoice")->property("displayText").toString(),
             QStringLiteral("APA Style 7th edition"));
    QCOMPARE(shown("styleSource"), QStringLiteral("Set in Preferences for every document"));
    QCOMPARE(shown("bibliographySource"), QStringLiteral("From the front matter"));
    QCOMPARE(shown("bibliographyFile"), shell.backend->shownText(library + QStringLiteral("/refs.bib")));
    QCOMPARE(shown("libraryRootSource"), QStringLiteral("Set for the library folder “library”"));
    QCOMPARE(shown("libraryRootPath"), shell.backend->shownText(library));
    QVERIFY(!inForm("profileReset")->property("visible").toBool());

    // Named for assistive technology, laid out within the window, and every
    // control reached by Tab.
    QTest::qWait(50);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    QCOMPARE(window->activeFocusItem()->objectName(), QStringLiteral("closeDocumentSettings"));
    QStringList reached;
    for (int i = 0; i < 60; ++i) {
        QTest::keyClick(window, Qt::Key_Tab);
        QQuickItem *now = window->activeFocusItem();
        if (!now || now->objectName() == u"closeDocumentSettings") break;
        reached << now->objectName();
        QVERIFY2(now->property("visualFocus").toBool(), qPrintable(now->objectName()));
    }
    for (const char *name : {"scopeDocument", "scopeLibrary", "profileChoice", "extension-math", "styleChoice",
                             "bibliographyRemove", "bibliographyAdd", "libraryRootChoose"})
        QVERIFY2(reached.contains(QString::fromLatin1(name)), qPrintable(reached.join(", ")));

    // The profile, for the document alone: the source says so, and the
    // setting is kept for the document.
    auto *profile = inForm("profileChoice");
    profile->forceActiveFocus(Qt::TabFocusReason);
    QMetaObject::invokeMethod(profile, "activated", Q_ARG(int, 1));
    QTRY_COMPARE(shown("profileSource"), QStringLiteral("Set for this document"));
    QCOMPARE(document->markdownOptions().profile, md::Profile::Standard);
    QCOMPARE(store->store().document(document->id()).profile, md::Profile::Standard);
    QVERIFY(inForm("profileReset")->property("visible").toBool());
    // Standard reads no front matter, so the bibliography is no longer its.
    QCOMPARE(shown("bibliographySource"), QStringLiteral("The default"));
    QVERIFY(inForm("bibliographyNone")->property("visible").toBool());
    // An extension, from the keyboard.
    auto *math = inForm("extension-math");
    QVERIFY(!math->property("checked").toBool());
    math->forceActiveFocus(Qt::TabFocusReason);
    QTest::keyClick(window, Qt::Key_Space);
    QTRY_VERIFY(document->markdownOptions().math);
    QVERIFY(math->property("checked").toBool());
    QCOMPARE(window->activeFocusItem(), math);
    QVERIFY(math->property("text").toString().contains(QStringLiteral("set for this document")));
    // Use the default gives it back.
    QMetaObject::invokeMethod(inForm("profileReset"), "clicked");
    QTRY_COMPARE(shown("profileSource"), QStringLiteral("The default"));
    QCOMPARE(shown("bibliographySource"), QStringLiteral("From the front matter"));

    // For every document of the library folder.
    QMetaObject::invokeMethod(shell.object(QStringLiteral("scopeLibrary")), "click");
    QTRY_COMPARE(dialog->property("scope").toString(), QStringLiteral("library"));
    QMetaObject::invokeMethod(inForm("styleChoice"), "activated", Q_ARG(int, 3));
    QTRY_COMPARE(shown("styleSource"), QStringLiteral("Set for the library folder “library”"));
    QCOMPARE(store->store().library(library).citationStyle, QStringLiteral("ieee"));
    QVERIFY(store->store().document(document->id()).citationStyle.isEmpty());
    // Files from the desktop's file dialog, as it answers.
    emit shell.backend->settingsFilesChosen(QStringLiteral("bibliography"), {root + QStringLiteral("/other.bib")});
    QTRY_COMPARE(store->store().library(library).bibliography.value_or(QStringList()),
                 (QStringList{library + QStringLiteral("/refs.bib"), root + QStringLiteral("/other.bib")}));
    // The front matter still names the document's own bibliography, over the library's.
    QCOMPARE(shown("bibliographySource"), QStringLiteral("From the front matter"));
    QMetaObject::invokeMethod(shell.object(QStringLiteral("scopeDocument")), "click");
    emit shell.backend->settingsFilesChosen(QStringLiteral("bibliography"), {root + QStringLiteral("/other.bib")});
    QTRY_COMPARE(shown("bibliographySource"), QStringLiteral("Set for this document"));
    QCOMPARE(store->store().document(document->id()).bibliography.value_or(QStringList()),
             (QStringList{library + QStringLiteral("/refs.bib"), root + QStringLiteral("/other.bib")}));
    emit shell.backend->settingsFilesChosen(QStringLiteral("style"), {root + QStringLiteral("/mine.csl")});
    QTRY_COMPARE(store->store().document(document->id()).citationStyle, root + QStringLiteral("/mine.csl"));
    QCOMPARE(inForm("styleChoice")->property("displayText").toString(), QStringLiteral("mine.csl"));
    emit shell.backend->settingsFilesChosen(QStringLiteral("libraryRoot"), {root});
    QTRY_COMPARE(document->libraryRoot(), root);
    QCOMPARE(shown("libraryRootSource"), QStringLiteral("Set for this document"));
    // Narrow, it still fits.
    resizeWindow(window, 360);
    QTest::qWait(50);
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QTRY_COMPARE(window->activeFocusItem(), shell.item(QStringLiteral("editor")));
}

void preferencesChooseForEveryDocument()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell;
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    Document *document = shell.document();
    QVERIFY(document);
    QVERIFY(run(window, "preferences"));
    QObject *dialog = shell.object(QStringLiteral("preferencesDialog"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    auto *form = qobject_cast<QQuickItem *>(shell.object(QStringLiteral("applicationDialectForm")));
    QVERIFY(form);
    QCOMPARE(form->property("scope").toString(), QStringLiteral("application"));
    // No library root for every document.
    auto *rootChoice = findItem(form, QStringLiteral("libraryRootChoose"));
    QVERIFY(rootChoice && !rootChoice->isVisible());
    QTest::qWait(50);
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    // The profile for every document reaches the document in front.
    auto *profile = findItem(form, QStringLiteral("profileChoice"));
    QVERIFY(profile);
    QMetaObject::invokeMethod(profile, "activated", Q_ARG(int, 1));
    QTRY_COMPARE(document->markdownOptions().profile, md::Profile::Standard);
    QCOMPARE(shownTextOf(findItem(form, QStringLiteral("profileSource"))),
             QStringLiteral("Set in Preferences for every document"));
    // A new document reads it too, and it is kept.
    shell.backend->newDraft();
    QCOMPARE(shell.document()->markdownOptions().profile, md::Profile::Standard);
    DocumentSettings::Store kept(shell.paths().documentSettingsFile());
    QVERIFY(kept.load());
    QCOMPARE(kept.application().profile, md::Profile::Standard);
}

void citationSuffixesAndSuppressedAuthors()
{
    // A locator with a suffix after it, and `-@` for an author the text names
    // already, alone or in a group; a key's `@` that follows a word makes no
    // citation, nor does a link.
    md::Source source;
    source.externalCitationKeys = {QStringLiteral("doe2006"), QStringLiteral("roe2010")};
    const md::Document document = md::parse(
        QStringLiteral("A [@doe2006, p. 23, emphasis added] and [-@roe2010] and [@doe2006; -@roe2010, ch. 2].\n"
                       "Not: [see@doe2006], [x-@doe2006], [-@doe2006](url).\n"),
        md::Options(), source);
    std::vector<const md::Node *> cites;
    std::function<void(const md::Node &)> collect = [&](const md::Node &node) {
        if (node.type == md::NodeType::Citation) cites.push_back(&node);
        for (const md::Node &child : node.children) collect(child);
    };
    collect(document.root);
    QCOMPARE(cites.size(), size_t(5)); // three citations, the last a group of two
    QCOMPARE(cites[0]->label, QStringLiteral("doe2006"));
    QCOMPARE(cites[0]->text, QStringLiteral("p. 23, emphasis added"));
    QCOMPARE(document.source(0, cites[0]->target).toString(), QStringLiteral("doe2006"));
    QCOMPARE(cites[1]->label, QStringLiteral("roe2010"));
    QCOMPARE(cites[1]->marker, QChar(u'-'));
    QCOMPARE(document.source(*cites[1]).toString(), QStringLiteral("[-@roe2010]"));
    QCOMPARE(document.source(0, cites[1]->target).toString(), QStringLiteral("roe2010"));
    QCOMPARE(cites[2]->children.size(), size_t(2));
    QCOMPARE(cites[3]->marker, QChar());
    QCOMPARE(cites[4]->marker, QChar(u'-'));
    QCOMPARE(cites[4]->text, QStringLiteral("ch. 2"));
    QCOMPARE(document.source(*cites[4]).toString(), QStringLiteral("-@roe2010, ch. 2"));
    QCOMPARE(document.source(cites[4]->file, cites[4]->open).toString(), QStringLiteral("-@"));
    QVERIFY(document.diagnostics.empty());

    // Formatted: the suffix follows the locator, and the author is left out.
    md::Document formatted = md::parse(
        QStringLiteral("A [@doe2006, p. 23, emphasis added] and Roe [-@roe2010].\n"), md::Options(), source);
    QTemporaryDir folder;
    writeFile(folder.filePath(QStringLiteral("refs.bib")), bibtex);
    Dialect::Choice choice = Dialect::effective({});
    choice.citationStyle = QStringLiteral("apa");
    choice.bibliography = {folder.filePath(QStringLiteral("refs.bib"))};
    const DocumentCitations::Formatted result =
        DocumentCitations::format(formatted, DocumentCitations::prepare(choice));
    QCOMPARE(citationTexts(result),
             (QStringList{QStringLiteral("(Doe, 2006, p. 23, emphasis added)"), QStringLiteral("(2010)")}));

    // A prefix before each key, and a key in the text that names its author
    // there, reach the style as Pandoc gives them to a CSL processor.
    md::Document prefixed = md::parse(
        QStringLiteral("A [see @doe2006, p. 23; also -@roe2010]. As @doe2006 [p. 4] shows, and @roe2010 too.\n"),
        md::Options(), source);
    QVERIFY(prefixed.diagnostics.empty());
    const DocumentCitations::Formatted words =
        DocumentCitations::format(prefixed, DocumentCitations::prepare(choice));
    QCOMPARE(citationTexts(words), (QStringList{QStringLiteral("(see Doe, 2006, p. 23; also 2010)"),
                                                QStringLiteral("Doe (2006, p. 4)"), QStringLiteral("Roe (2010)")}));
}

void citationsAuthorDateInHtml()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = canonicalPath(folder.path());
    writeFile(base + QStringLiteral("/refs.bib"), bibtex);
    writeFile(base + QStringLiteral("/doc.md"),
              "---\nbibliography: refs.bib\ncsl: apa\nlink-citations: true\n---\n\n"
              "As shown [@doe2006, p. 23], and later [@roe2010; @doe2006].\n");
    const DocumentReading::Reading reading = readFileIn(base + QStringLiteral("/doc.md"));
    QVERIFY(reading.formatted.active);
    QVERIFY(!reading.formatted.noteStyle);
    QCOMPARE(citationTexts(reading.formatted),
             (QStringList{QStringLiteral("(Doe, 2006, p. 23)"), QStringLiteral("(Doe, 2006; Roe, 2010)")}));
    QCOMPARE(bibliographyTexts(reading.formatted),
             (QStringList{QStringLiteral("Doe, J. (2006). Writing with examples. Example Press."),
                          QStringLiteral("Roe, J. (2010). Reading without examples. Sample House.")}));
    QVERIFY(reading.formatted.problems.isEmpty());
    const QString html = htmlOf(reading);
    // In the text, each linked to its entry when there is one work; the
    // bibliography after it, in the style's order and with its formatting.
    QVERIFY2(html.contains(QStringLiteral("<p>As shown <span class=\"citation\" data-cites=\"doe2006\">"
                                          "<a href=\"#ref-doe2006\">(Doe, 2006, p. 23)</a></span>")),
             qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("<span class=\"citation\" data-cites=\"roe2010 doe2006\">(Doe, 2006; Roe, 2010)</span>")),
             qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("<section class=\"bibliography csl-bib-body hanging-indent\" role=\"doc-bibliography\">\n"
                                          "<div class=\"csl-entry\" id=\"ref-doe2006\">Doe, J. (2006). <i>Writing with "
                                          "examples</i>. Example Press.</div>")),
             qPrintable(html));
    QVERIFY(!html.contains(QStringLiteral("cite-1")));
    // The engine resolved the keys from the bibliography: nothing is missing.
    QVERIFY(reading.document.diagnostics.empty());
}

void citationsNumericThroughRender()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = canonicalPath(folder.path());
    writeFile(base + QStringLiteral("/refs.json"), cslJson);
    writeFile(base + QStringLiteral("/doc.md"), "First [@roe2010], then [@doe2006, p. 23] and [@roe2010].\n");
    const AppPaths paths = AppPaths::under(base + QStringLiteral("/app"));
    const Ran ran = runCli(base, paths, {"render", "doc.md", "--csl", "ieee", "--bibliography", "refs.json"});
    QVERIFY2(ran.code == 0, qPrintable(ran.said()));
    // Numbers by first citation, the locator with its label.
    QVERIFY2(ran.out.contains(QStringLiteral("First <span class=\"citation\" data-cites=\"roe2010\">[1]</span>, then "
                                             "<span class=\"citation\" data-cites=\"doe2006\">[2, p. 23]</span> and "
                                             "<span class=\"citation\" data-cites=\"roe2010\">[1]</span>.")),
             qPrintable(ran.said()));
    // The entries in that order, each number standing apart.
    const qsizetype roe = ran.out.indexOf(QStringLiteral("id=\"ref-roe2010\""));
    const qsizetype doe = ran.out.indexOf(QStringLiteral("id=\"ref-doe2006\""));
    QVERIFY2(roe > 0 && doe > roe, qPrintable(ran.out));
    QVERIFY(ran.out.contains(QStringLiteral("<div class=\"csl-left-margin\">[1]</div>")));
    QVERIFY2(ran.out.contains(QStringLiteral("J. Roe, <i>Reading")), qPrintable(ran.out));
    // The JSON report says which style and bibliography were used, from where.
    const Ran json = runCli(base, paths, {"render", "doc.md", "--csl", "ieee", "--bibliography", "refs.json", "--json"});
    const QJsonObject dialect = json.json().value(QStringLiteral("dialect")).toObject();
    QCOMPARE(dialect.value(QStringLiteral("citationStyle")).toString(), QStringLiteral("ieee"));
    QCOMPARE(dialect.value(QStringLiteral("bibliography")).toArray().first().toString(), base + QStringLiteral("/refs.json"));
    QCOMPARE(dialect.value(QStringLiteral("from")).toObject().value(QStringLiteral("citationStyle")).toString(),
             QStringLiteral("flag"));
    // The export writes the same citations into its page.
    const Ran exported = runCli(base, paths, {"export", "doc.md", "-o", "doc.html", "--csl", "ieee", "--bibliography",
                                              "refs.json", "--quiet"});
    QVERIFY2(exported.code == 0, qPrintable(exported.said()));
    const QString page = QString::fromUtf8(readFile(base + QStringLiteral("/doc.html")));
    QVERIFY(page.contains(QStringLiteral("[2, p. 23]")));
    QVERIFY(page.contains(QStringLiteral(".csl-left-margin")));
    // A flag naming a style or a file that is not there is a wrong command line.
    QCOMPARE(runCli(base, paths, {"render", "doc.md", "--csl", "no-such-style"}).code, 1);
    QCOMPARE(runCli(base, paths, {"render", "doc.md", "--bibliography", "missing.bib"}).code, 1);
    // Without a style or bibliography, the engine's own numbering stands.
    const Ran plain = runCli(base, paths, {"render", "doc.md"});
    QVERIFY(plain.out.contains(QStringLiteral("[@roe2010]")));
}

void citationsInANoteStyleAreNotes()
{
    // In a note style each citation of the running text is a footnote,
    // numbered with the other notes as a reader meets them; a citation in a
    // note stays in it.
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = canonicalPath(folder.path());
    writeFile(base + QStringLiteral("/refs.yaml"), cslYaml);
    writeFile(base + QStringLiteral("/doc.md"),
              "---\nbibliography: refs.yaml\ncsl: chicago-notes-bibliography\n---\n\n"
              "First a note.[^a] Then a work [@doe2006, p. 23]. A note citing [^b]. Again [@doe2006, p. 24].\n\n"
              "[^a]: Plain.\n[^b]: See [@roe2010].\n");
    const DocumentReading::Reading reading = readFileIn(base + QStringLiteral("/doc.md"));
    QVERIFY(reading.formatted.noteStyle);
    const md::Document &document = reading.document;
    QCOMPARE(document.footnotes.size(), size_t(4));
    QCOMPARE(document.footnotes[0].label, QStringLiteral("a"));
    QCOMPARE(document.footnotes[1].content->type, md::NodeType::Citation);
    QCOMPARE(document.footnotes[2].label, QStringLiteral("b"));
    QCOMPARE(document.footnotes[3].content->type, md::NodeType::Citation);
    // Each citation knows its note, the one in note 3 too.
    QCOMPARE(reading.formatted.citations.size(), 3);
    QCOMPARE(reading.formatted.citations[0].note, 2);
    QCOMPARE(reading.formatted.citations[1].note, 3);
    QCOMPARE(reading.formatted.citations[2].note, 4);
    QCOMPARE(citationTexts(reading.formatted),
             (QStringList{QStringLiteral("John Doe, Writing with Examples (Example Press, 2006), 23."),
                          QStringLiteral("Jane Roe, Reading without Examples (Sample House, 2010)."),
                          QStringLiteral("Doe, Writing with Examples, 24.")}));
    const QString html = htmlOf(reading);
    QVERIFY2(html.contains(QStringLiteral("Then a work <sup class=\"footnote-ref\"><a href=\"#fn-2\" id=\"fnref-2\">2</a></sup>.")),
             qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("<li id=\"fn-2\">\n<p><span class=\"citation\" data-cites=\"doe2006\">John Doe, "
                                          "<i>Writing with Examples</i> (Example Press, 2006), 23.</span> "
                                          "<a href=\"#fnref-2\" class=\"footnote-backref\">")),
             qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("<li id=\"fn-3\">\n<p>See <span class=\"citation\" data-cites=\"roe2010\">")),
             qPrintable(html));
    QVERIFY(html.contains(QStringLiteral("<section class=\"bibliography csl-bib-body")));
    // A group of works is one note; an inline note's citation stays in it.
    md::Source notes;
    notes.citationNotes = true;
    notes.externalCitationKeys = {QStringLiteral("doe2006"), QStringLiteral("roe2010")};
    const md::Document grouped = md::parse(
        QStringLiteral("Both [@doe2006; @roe2010]. Aside[^see [@roe2010] too].\n"), md::Options(), notes);
    QCOMPARE(grouped.footnotes.size(), size_t(2));
    QCOMPARE(grouped.footnotes[0].content->type, md::NodeType::Citation);
    QCOMPARE(grouped.footnotes[1].content->type, md::NodeType::InlineNote);
    QCOMPARE(grouped.bibliography.size(), size_t(2));
    // An author-date style leaves the same text's notes as they are.
    Dialect::Layers layers;
    layers.flags.citationStyle = QStringLiteral("apa");
    const DocumentReading::Reading authorDate = readFileIn(base + QStringLiteral("/doc.md"), layers);
    QCOMPARE(authorDate.document.footnotes.size(), size_t(2));
    QCOMPARE(authorDate.formatted.citations[1].note, 2);
}

void citationsFromEveryBibliographyFormat()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = canonicalPath(folder.path());
    writeFile(base + QStringLiteral("/refs.bib"), bibtex);
    writeFile(base + QStringLiteral("/refs.json"), cslJson);
    writeFile(base + QStringLiteral("/refs.yaml"), cslYaml);
    writeFile(base + QStringLiteral("/doc.md"), "As shown [@doe2006, p. 23] and [@roe2010].\n");
    QStringList first;
    for (const char *file : {"refs.bib", "refs.json", "refs.yaml"}) {
        Dialect::Layers layers;
        layers.flags.citationStyle = QStringLiteral("chicago-author-date");
        layers.flags.bibliography = QStringList{base + u'/' + QString::fromLatin1(file)};
        const DocumentReading::Reading reading = readFileIn(base + QStringLiteral("/doc.md"), layers);
        QVERIFY2(reading.formatted.problems.isEmpty(), file);
        const QStringList texts = citationTexts(reading.formatted) + bibliographyTexts(reading.formatted);
        QCOMPARE(texts.first(), QStringLiteral("(Doe 2006, 23)"));
        if (first.isEmpty()) first = texts;
        else QCOMPARE(texts, first);
    }
    // A bibliography changed on disk is read again.
    writeFile(base + QStringLiteral("/refs.bib"), QByteArray(bibtex).replace("Doe, John", "Doe, Jonathan"));
    Dialect::Layers layers;
    layers.flags.citationStyle = QStringLiteral("apa");
    layers.flags.bibliography = QStringList{base + QStringLiteral("/refs.bib")};
    QTRY_VERIFY(bibliographyTexts(readFileIn(base + QStringLiteral("/doc.md"), layers).formatted).first().startsWith(
        QStringLiteral("Doe, J. (2006)")));
    QVERIFY(DocumentCitations::prepare(Dialect::effective(layers)).keys().contains(QStringLiteral("doe2006")));
}

void citationsWithAStyleFile()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = canonicalPath(folder.path());
    writeFile(base + QStringLiteral("/refs.bib"), bibtex);
    writeFile(base + QStringLiteral("/styles/own.csl"), ownStyle);
    writeFile(base + QStringLiteral("/doc.md"),
              "---\nbibliography: refs.bib\ncsl: styles/own.csl\n---\n\nShown [@doe2006; @roe2010].\n");
    const DocumentReading::Reading reading = readFileIn(base + QStringLiteral("/doc.md"));
    QCOMPARE(reading.dialect.citationStyle, base + QStringLiteral("/styles/own.csl"));
    QCOMPARE(citationTexts(reading.formatted), QStringList{QStringLiteral("<<Doe 2006; Roe 2010>>")});
    QCOMPARE(bibliographyTexts(reading.formatted),
             (QStringList{QStringLiteral("Doe, John: Writing with examples"), QStringLiteral("Roe, Jane: Reading without examples")}));
    QVERIFY(htmlOf(reading).contains(QStringLiteral("&lt;&lt;Doe 2006; Roe 2010&gt;&gt;")));
    // `parch render` takes the same file from the front matter.
    const Ran ran = runCli(base, AppPaths::under(base + QStringLiteral("/app")), {"render", "doc.md"});
    QVERIFY2(ran.out.contains(QStringLiteral("&lt;&lt;Doe 2006; Roe 2010&gt;&gt;")), qPrintable(ran.said()));
    // A style file that cannot be used says why, and citations show their keys.
    writeFile(base + QStringLiteral("/styles/broken.csl"), "<style><nonsense");
    Dialect::Layers layers;
    layers.flags.citationStyle = base + QStringLiteral("/styles/broken.csl");
    const DocumentReading::Reading broken = readFileIn(base + QStringLiteral("/doc.md"), layers);
    QCOMPARE(problemCodes(broken.formatted.problems), QStringList{QStringLiteral("citation-style")});
    QCOMPARE(broken.formatted.problems.first().severity, md::Severity::Error);
    QCOMPARE(citationTexts(broken.formatted), QStringList{QStringLiteral("[doe2006; roe2010]")});
}

void writerAndPandocCitationsTogether()
{
    // Writer citations of definitions in the text and Pandoc citations of a
    // bibliography file, in one document and one style.
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = canonicalPath(folder.path());
    writeFile(base + QStringLiteral("/refs.bib"), bibtex);
    writeFile(base + QStringLiteral("/doc.md"),
              "---\nbibliography: refs.bib\ncsl: apa\n---\n\n"
              "A statement[p. 23][#Notes:2020]. Another [@doe2006]. Again[][#Notes:2020].\n\n"
              "[#Notes:2020]: A. Writer. *Field notes*. Self-published, 2020.\n");
    const DocumentReading::Reading reading = readFileIn(base + QStringLiteral("/doc.md"));
    QVERIFY(reading.document.diagnostics.empty());
    QCOMPARE(citationTexts(reading.formatted),
             (QStringList{QStringLiteral("[1, p. 23]"), QStringLiteral("(Doe, 2006)"), QStringLiteral("[1]")}));
    // The style's entries first, then the document's own as written, numbered.
    QCOMPARE(bibliographyTexts(reading.formatted),
             (QStringList{QStringLiteral("Doe, J. (2006). Writing with examples. Example Press."),
                          QStringLiteral("[1]A. Writer. Field notes. Self-published, 2020.")}));
    const QString html = htmlOf(reading);
    QVERIFY2(html.contains(QStringLiteral("<div class=\"csl-entry\" id=\"ref-Notes:2020\"><div class=\"csl-left-margin\">[1]</div>"
                                          "<div class=\"csl-right-inline\">A. Writer. <i>Field notes</i>. Self-published, "
                                          "2020.</div></div>")),
             qPrintable(html));
}

void checkReportsCitationProblems()
{
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    const QString base = canonicalPath(folder.path());
    // A key twice in one file, a key in two files, a file that cannot be read.
    writeFile(base + QStringLiteral("/refs.bib"),
              QByteArray(bibtex) + "\n@book{doe2006,\n  author = {Doe, Again},\n  title = {Twice},\n  year = {2007}\n}\n");
    writeFile(base + QStringLiteral("/more.json"), cslJson);
    writeFile(base + QStringLiteral("/doc.md"),
              "---\nbibliography: [refs.bib, more.json, gone.bib]\ncsl: apa\n---\n\n"
              "Cited [@doe2006] and [@roe2010] and [@nobody].\n");
    const AppPaths paths = AppPaths::under(base + QStringLiteral("/app"));
    const Ran ran = runCli(base, paths, {"check", "doc.md", "--json"});
    QCOMPARE(ran.code, 2);
    const QJsonObject document = ran.json().value(QStringLiteral("documents")).toArray().first().toObject();
    QHash<QString, QJsonObject> byCode;
    QStringList codes;
    for (const QJsonValue &value : document.value(QStringLiteral("problems")).toArray()) {
        const QJsonObject problem = value.toObject();
        codes.append(problem.value(QStringLiteral("code")).toString());
        byCode.insert(problem.value(QStringLiteral("code")).toString() + u'@'
                          + QFileInfo(problem.value(QStringLiteral("file")).toString()).fileName(),
                      problem);
    }
    // The missing key, at its place in the text.
    const QJsonObject missing = byCode.value(QStringLiteral("missing-citation@doc.md"));
    QCOMPARE(missing.value(QStringLiteral("line")).toInt(), 6);
    QCOMPARE(missing.value(QStringLiteral("kind")).toString(), QStringLiteral("citation"));
    // The key twice in one file, at its line there.
    const QJsonObject twice = byCode.value(QStringLiteral("duplicate-citation-key@refs.bib"));
    QCOMPARE(twice.value(QStringLiteral("line")).toInt(), 15);
    QCOMPARE(twice.value(QStringLiteral("kind")).toString(), QStringLiteral("bibliography"));
    QVERIFY(twice.value(QStringLiteral("range")).isNull());
    // The keys in two files, where they are cited.
    const QJsonObject ambiguous = byCode.value(QStringLiteral("duplicate-citation-key@doc.md"));
    QCOMPARE(ambiguous.value(QStringLiteral("line")).toInt(), 6);
    QVERIFY(ambiguous.value(QStringLiteral("message")).toString().contains(QStringLiteral("more.json")));
    // The file that is not there, an error.
    const QJsonObject unreadable = byCode.value(QStringLiteral("unreadable-bibliography@gone.bib"));
    QCOMPARE(unreadable.value(QStringLiteral("severity")).toString(), QStringLiteral("error"));
    QCOMPARE(codes.count(QStringLiteral("missing-citation")), 1);
    // In words, with the bibliography's own line.
    const Ran text = runCli(base, paths, {"check", "doc.md"});
    QVERIFY2(text.out.contains(QStringLiteral("refs.bib:15:1: warning: The key “doe2006” is defined more than once; the "
                                              "first entry is used. [duplicate-citation-key]")),
             qPrintable(text.said()));
    QVERIFY2(text.out.contains(QStringLiteral("gone.bib: error: ")), qPrintable(text.said()));

    // A bibliography without a style: keys shown, and why.
    writeFile(base + QStringLiteral("/nostyle.md"), "---\nbibliography: more.json\n---\n\nCited [@doe2006].\n");
    const Ran nostyle = runCli(base, paths, {"check", "nostyle.md", "--json"});
    QCOMPARE(nostyle.code, 2);
    QVERIFY(nostyle.out.contains(QStringLiteral("no-citation-style")));
    // An unknown bundled style, from the front matter.
    writeFile(base + QStringLiteral("/unknown.md"), "---\ncsl: no-such-style\n---\n\nText.\n");
    const Ran unknown = runCli(base, paths, {"check", "unknown.md"});
    QCOMPARE(unknown.code, 2);
    QVERIFY2(unknown.out.contains(QStringLiteral("[citation-style]")), qPrintable(unknown.said()));
    // A clean document with a style and a bibliography is clean.
    writeFile(base + QStringLiteral("/refs2.bib"), bibtex);
    writeFile(base + QStringLiteral("/clean.md"), "---\nbibliography: refs2.bib\ncsl: apa\n---\n\nCited [@doe2006].\n");
    const Ran clean = runCli(base, paths, {"check", "clean.md"});
    QVERIFY2(clean.code == 0, qPrintable(clean.said()));
    // The export says what its reader will miss.
    const Ran exported = runCli(base, paths, {"export", "doc.md", "-o", "doc.html"});
    QCOMPARE(exported.code, 0);
    QVERIFY2(exported.err.contains(QStringLiteral("[missing-citation]")), qPrintable(exported.said()));
}

} // namespace SettingsTests
