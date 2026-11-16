#include "diagnosticstests.h"

#include "apppaths.h"
#include "backend.h"
#include "cli.h"
#include "clicommands.h"
#include "dialectchoice.h"
#include "document.h"
#include "documentcheck.h"
#include "documentchecker.h"
#include "documentreading.h"
#include "documentsettings.h"
#include "documentsettingsstore.h"
#include "mdhtml.h"
#include "mdinclude.h"
#include "preview.h"
#include "previewbuilder.h"
#include "previewstyle.h"
#include "renderhelpers.h"
#include "renderservice.h"
#include "rendertesting.h"
#include "sourcebuffer.h"
#include "tabmodel.h"
#include "testfiles.h"
#include "windowcontext.h"
#include "windowtesting.h"

#include <QAccessible>
#include <QBuffer>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

namespace DiagnosticsTests {
namespace {

QString canonicalPath(const QString &path)
{
    return QFileInfo(path).canonicalFilePath();
}

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
    Cli::Context context{folder, folder + QStringLiteral("/home"), paths, &out, &err};
    context.renderCache = paths.data + QStringLiteral("/../cache/render");
    arguments.prepend(QStringLiteral("parch"));
    Ran ran;
    ran.code = Cli::run(arguments, context);
    ran.out = QString::fromUtf8(out.data());
    ran.err = QString::fromUtf8(err.data());
    return ran;
}

// The codes of the problems `parch check --json` found in its one document.
QStringList problemCodes(const QJsonObject &result)
{
    QStringList codes;
    const QJsonArray documents = result.value(QStringLiteral("documents")).toArray();
    for (const QJsonValue &problem : documents.first().toObject().value(QStringLiteral("problems")).toArray())
        codes.append(problem.toObject().value(QStringLiteral("code")).toString());
    return codes;
}

QJsonObject dialectOf(const QJsonObject &result)
{
    return result.value(QStringLiteral("documents")).toArray().first().toObject().value(QStringLiteral("dialect")).toObject();
}

QStringList diagnosticCodes(const md::Document &document)
{
    QStringList codes;
    for (const md::Diagnostic &diagnostic : document.diagnostics)
        codes.append(QString::fromLatin1(md::codeName(diagnostic.code)));
    return codes;
}

Ran runCliWith(const QString &folder, const AppPaths &paths, const HelperSet &helpers, QStringList arguments)
{
    QBuffer out, err;
    out.open(QIODevice::WriteOnly);
    err.open(QIODevice::WriteOnly);
    Cli::Context context{folder, folder + QStringLiteral("/home"), paths, &out, &err};
    context.renderCache = paths.data + QStringLiteral("/../cache/render");
    context.helpers = helpers;
    arguments.prepend(QStringLiteral("parch"));
    Ran ran;
    ran.code = Cli::run(arguments, context);
    ran.out = QString::fromUtf8(out.data());
    ran.err = QString::fromUtf8(err.data());
    return ran;
}

// The problems of `parch check --json`'s one document, as "line:column code kind".
QStringList problemPlaces(const QJsonObject &result)
{
    QStringList places;
    const QJsonArray documents = result.value(QStringLiteral("documents")).toArray();
    for (const QJsonValue &value : documents.first().toObject().value(QStringLiteral("problems")).toArray()) {
        const QJsonObject problem = value.toObject();
        places.append(QStringLiteral("%1:%2 %3 %4")
                          .arg(problem.value(QStringLiteral("line")).toInt())
                          .arg(problem.value(QStringLiteral("column")).toInt())
                          .arg(problem.value(QStringLiteral("code")).toString(),
                               problem.value(QStringLiteral("kind")).toString()));
    }
    return places;
}

int helperRuns(const QString &log)
{
    return int(QString::fromUtf8(readFile(log)).count(QStringLiteral("start ")));
}

// A document read as every output reads it, from a file, in these layers.
DocumentReading::Reading readFileIn(const QString &path, const Dialect::Layers &layers)
{
    md::FileIncludeResolver resolver;
    const md::IncludeResolver::File file = resolver.read(path, 1 << 20);
    md::Source source;
    source.path = file.canonicalPath;
    source.resolver = &resolver;
    return DocumentReading::read(file.text, layers, source);
}

} // namespace

void includeLimitsAreConfigurable()
{
    // The three guards are settings like the others: the highest layer that
    // names one wins, and every reading of the document follows it.
    Dialect::Layers layers;
    Dialect::Choice choice = Dialect::effective(layers);
    const md::Source engine;
    QCOMPARE(choice.maxIncludeDepth, engine.maxIncludeDepth);
    QCOMPARE(choice.maxIncludeSize, engine.maxExpandedBytes);
    QCOMPARE(choice.maxIncludes, engine.maxIncludeCount);
    QCOMPARE(choice.maxIncludeDepthFrom, QStringLiteral("default"));
    layers.application.maxIncludeDepth = 8;
    layers.application.maxIncludes = 50;
    layers.library.maxIncludeSize = 4096;
    layers.document.maxIncludes = 5;
    layers.flags.maxIncludeDepth = 2;
    choice = Dialect::effective(layers);
    QCOMPARE(choice.maxIncludeDepth, 2);
    QCOMPARE(choice.maxIncludeDepthFrom, QStringLiteral("flag"));
    QCOMPARE(choice.maxIncludeSize, qint64(4096));
    QCOMPARE(choice.maxIncludeSizeFrom, QStringLiteral("library"));
    QCOMPARE(choice.maxIncludes, 5);
    QCOMPARE(choice.maxIncludesFrom, QStringLiteral("document"));
    QVERIFY(choice.savedSettings);
    md::Source limited;
    choice.limit(limited);
    QCOMPARE(limited.maxIncludeDepth, 2);
    QCOMPARE(limited.maxExpandedBytes, qint64(4096));
    QCOMPARE(limited.maxIncludeCount, 5);
    const QJsonObject json = choice.toJson();
    QCOMPARE(json.value(QStringLiteral("maxIncludeDepth")).toInt(), 2);
    QCOMPARE(json.value(QStringLiteral("maxIncludeSize")).toInteger(), 4096);
    QCOMPARE(json.value(QStringLiteral("maxIncludes")).toInt(), 5);
    QCOMPARE(json.value(QStringLiteral("from")).toObject().value(QStringLiteral("maxIncludeSize")).toString(),
             QStringLiteral("library"));
    QVERIFY2(choice.describe().contains(QStringLiteral("includes at most 2 deep")), qPrintable(choice.describe()));
    QVERIFY(!Dialect::effective({}).describe().contains(QStringLiteral("includes")));

    // Kept in the settings file like the rest, read back the same.
    Dialect::Layer kept;
    kept.maxIncludeDepth = 3;
    kept.maxIncludeSize = 2 * 1024 * 1024;
    kept.maxIncludes = 7;
    const QJsonObject entry = DocumentSettings::entryOf(kept);
    QCOMPARE(entry.value(QStringLiteral("maxIncludeDepth")).toInt(), 3);
    QCOMPARE(entry.value(QStringLiteral("maxIncludeSize")).toInteger(), 2 * 1024 * 1024);
    QCOMPARE(entry.value(QStringLiteral("maxIncludes")).toInt(), 7);
    QCOMPARE(DocumentSettings::layerOf(entry), kept);
    // A guard of nothing, or of less, guards nothing: it is not read.
    QVERIFY(DocumentSettings::layerOf(QJsonObject{{QStringLiteral("maxIncludeDepth"), 0},
                                                  {QStringLiteral("maxIncludeSize"), -5},
                                                  {QStringLiteral("maxIncludes"), QStringLiteral("many")}})
                .isEmpty());

    // A chain of three includes, a few bytes each.
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonicalPath(files.path());
    const QString path = root + QStringLiteral("/book.md");
    writeFile(path, "# Book\n\nchapters/one.md\n");
    writeFile(root + QStringLiteral("/chapters/one.md"), "One.\n\ntwo.md\n");
    writeFile(root + QStringLiteral("/chapters/two.md"), "Two.\n\nthree.md\n");
    writeFile(root + QStringLiteral("/chapters/three.md"), "Three.\n");
    const AppPaths paths = AppPaths::under(root + QStringLiteral("/app"));

    // Within the defaults, nothing is wrong.
    Ran ran = runCli(root, paths, {QStringLiteral("check"), path, QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    QCOMPARE(dialectOf(ran.json()).value(QStringLiteral("maxIncludeDepth")).toInt(), 32);
    // Each option is a guard of its own, and says so with the line it stops at.
    ran = runCli(root, paths, {QStringLiteral("check"), path, QStringLiteral("--max-include-depth"), QStringLiteral("2"),
                               QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Problems, qPrintable(ran.said()));
    QCOMPARE(problemCodes(ran.json()), QStringList{QStringLiteral("include-depth-limit")});
    QCOMPARE(dialectOf(ran.json()).value(QStringLiteral("maxIncludeDepth")).toInt(), 2);
    QCOMPARE(dialectOf(ran.json()).value(QStringLiteral("from")).toObject().value(QStringLiteral("maxIncludeDepth")).toString(),
             QStringLiteral("flag"));
    ran = runCli(root, paths, {QStringLiteral("check"), path, QStringLiteral("--max-include-depth=3")});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    ran = runCli(root, paths, {QStringLiteral("check"), path, QStringLiteral("--max-includes"), QStringLiteral("2"),
                               QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Problems, qPrintable(ran.said()));
    QCOMPARE(problemCodes(ran.json()), QStringList{QStringLiteral("include-count-limit")});
    ran = runCli(root, paths, {QStringLiteral("check"), path, QStringLiteral("--max-include-size"), QStringLiteral("40")});
    QVERIFY2(ran.code == Cli::Problems, qPrintable(ran.said()));
    QVERIFY2(ran.out.contains(QStringLiteral("[include-size-limit]")), qPrintable(ran.said()));
    QVERIFY2(ran.out.contains(QStringLiteral("larger than 40 bytes")), qPrintable(ran.said()));
    // Sizes may carry a unit.
    ran = runCli(root, paths, {QStringLiteral("check"), path, QStringLiteral("--max-include-size"), QStringLiteral("1K"),
                               QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    QCOMPARE(dialectOf(ran.json()).value(QStringLiteral("maxIncludeSize")).toInteger(), 1024);
    // render and stats take them too.
    ran = runCli(root, paths, {QStringLiteral("render"), path, QStringLiteral("--to"), QStringLiteral("text"),
                               QStringLiteral("--max-include-depth"), QStringLiteral("1")});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    QVERIFY2(ran.out.contains(QStringLiteral("One.")) && !ran.out.contains(QStringLiteral("Two.")), qPrintable(ran.said()));
    // What is not a limit is a wrong command line.
    for (const QStringList &wrong : {QStringList{QStringLiteral("--max-include-depth"), QStringLiteral("0")},
                                     QStringList{QStringLiteral("--max-include-depth"), QStringLiteral("deep")},
                                     QStringList{QStringLiteral("--max-includes"), QStringLiteral("-1")},
                                     QStringList{QStringLiteral("--max-include-size"), QStringLiteral("big")},
                                     QStringList{QStringLiteral("--max-include-size"), QStringLiteral("0K")}}) {
        ran = runCli(root, paths, QStringList{QStringLiteral("check"), path} + wrong);
        QVERIFY2(ran.code == Cli::Usage, qPrintable(wrong.join(u' ') + u'\n' + ran.said()));
    }
    ran = runCli(root, paths, {QStringLiteral("help"), QStringLiteral("check")});
    for (const char *option : {"--max-include-depth N", "--max-include-size SIZE", "--max-includes N"})
        QVERIFY2(ran.out.contains(QLatin1String(option)), qPrintable(ran.said()));

    // From the settings parch keeps, when asked to read them.
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    writeFile(paths.registryFile(), QJsonDocument(QJsonObject{{path, id}}).toJson());
    {
        DocumentSettings::Store store(paths.documentSettingsFile());
        QVERIFY(store.load());
        Dialect::Layer own;
        own.maxIncludeDepth = 1;
        QVERIFY(store.setDocument(id, own));
    }
    ran = runCli(root, paths, {QStringLiteral("check"), path});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    ran = runCli(root, paths, {QStringLiteral("check"), path, QStringLiteral("--use-saved-settings"), QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Problems, qPrintable(ran.said()));
    QCOMPARE(problemCodes(ran.json()), QStringList{QStringLiteral("include-depth-limit")});
    QCOMPARE(dialectOf(ran.json()).value(QStringLiteral("from")).toObject().value(QStringLiteral("maxIncludeDepth")).toString(),
             QStringLiteral("document"));

    // In the window: the document settings set them, every reading of the
    // document (the preview reads it so) follows, and the form shows them.
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);
    Document *document = shell.document();
    DocumentSettingsStore *store = shell.backend->documentSettings();
    QVERIFY(diagnosticCodes(readFileIn(path, document->savedDialect()).document).isEmpty());
    QSignalSpy changed(document, &Document::dialectChanged);
    QVERIFY(store->setIncludeLimit(document, QStringLiteral("document"), QStringLiteral("maxIncludeDepth"), 2));
    QCOMPARE(changed.count(), 1);
    QCOMPARE(document->savedDialect().document.maxIncludeDepth, 2);
    QCOMPARE(diagnosticCodes(readFileIn(path, document->savedDialect()).document),
             QStringList{QStringLiteral("include-depth-limit")});
    QVERIFY(store->setIncludeLimit(nullptr, QStringLiteral("application"), QStringLiteral("maxIncludes"), 1));
    QCOMPARE(Dialect::effective(document->savedDialect()).maxIncludes, 1);
    // Unknown names and values that guard nothing change nothing.
    QVERIFY(!store->setIncludeLimit(document, QStringLiteral("document"), QStringLiteral("maxWords"), 2));
    QVERIFY(!store->setIncludeLimit(document, QStringLiteral("document"), QStringLiteral("maxIncludes"), -2));
    QVariantMap info = store->describe(document);
    QCOMPARE(info.value(QStringLiteral("maxIncludeDepth")).toInt(), 2);
    QCOMPARE(info.value(QStringLiteral("maxIncludeDepthFrom")).toString(), QStringLiteral("document"));
    QCOMPARE(info.value(QStringLiteral("maxIncludesFrom")).toString(), QStringLiteral("application"));
    QCOMPARE(info.value(QStringLiteral("maxIncludeSize")).toLongLong(), engine.maxExpandedBytes);

    QObject *dialog = shell.object(QStringLiteral("documentSettingsDialog"));
    QVERIFY(QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QStringLiteral("documentSettings"))));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QQuickItem *form = shell.item(QStringLiteral("documentSettingsForm"));
    QVERIFY(form);
    QQuickItem *depth = findItem(form, QStringLiteral("maxIncludeDepthChoice"));
    QQuickItem *size = findItem(form, QStringLiteral("maxIncludeSizeChoice"));
    QQuickItem *count = findItem(form, QStringLiteral("maxIncludesChoice"));
    QVERIFY(depth && size && count);
    QCOMPARE(depth->property("value").toInt(), 2);
    QCOMPARE(size->property("value").toInt(), 100); // MiB
    QCOMPARE(count->property("value").toInt(), 1);
    QCOMPARE(findItem(form, QStringLiteral("includeLimitsSource"))->property("text").toString(),
             QStringLiteral("Depth: set for this document. Size: the default. Includes: set in Preferences for every document."));
    // Changed in the form, kept for the document; Use the default gives all three back.
    QMetaObject::invokeMethod(size, "increase");
    QMetaObject::invokeMethod(size, "valueModified");
    QTRY_COMPARE(store->store().document(document->id()).maxIncludeSize.value_or(0), qint64(101) * 1024 * 1024);
    QQuickItem *reset = findItem(form, QStringLiteral("includeLimitsReset"));
    QVERIFY(reset && reset->property("visible").toBool());
    QMetaObject::invokeMethod(reset, "clicked");
    QTRY_VERIFY(!store->store().document(document->id()).maxIncludeDepth);
    QVERIFY(!store->store().document(document->id()).maxIncludeSize);
    QTRY_COMPARE(depth->property("value").toInt(), 32);
    QTest::qWait(50);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());
}

void checkDrawsFormulasAndDiagrams()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonicalPath(files.path());
    const QString helpersFolder = root + QStringLiteral("/helpers");
    const QString log = root + QStringLiteral("/helpers.log");
    writeFakeHelpers(helpersFolder, log);
    writeFile(helpersFolder + QStringLiteral("/.pins"), "ratex 9.9.1 0123abcd\nmerman 9.9.2 4567cdef\n");
    const HelperSet helpers = HelperSet::in(helpersFolder);
    const AppPaths paths = AppPaths::under(root + QStringLiteral("/app"));
    const QString path = root + QStringLiteral("/doc.md");
    const QString chapter = root + QStringLiteral("/chapter.md");
    writeFile(path, "# Sums\n"
                    "\n"
                    "Good $x+1$ and bad $refuse this$.\n"
                    "\n"
                    "$$\n"
                    "refuse \\frac\n"
                    "$$\n"
                    "\n"
                    "```mermaid\n"
                    "flowchart LR\n"
                    "  A --> B\n"
                    "```\n"
                    "\n"
                    "```mermaid\n"
                    "refuse\n"
                    "```\n"
                    "\n"
                    "chapter.md\n"
                    "\n"
                    "chapter.md\n");
    writeFile(chapter, "Once more $refuse again$.\n");

    // Each formula and diagram is drawn by its helper, and what the helper
    // refuses is a problem, with its line and column, in the file it is
    // written in, once even when that file is included twice.
    Ran ran = runCliWith(root, paths, helpers, {QStringLiteral("check"), path, QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Problems, qPrintable(ran.said()));
    QJsonObject result = ran.json();
    QCOMPARE(problemPlaces(result), (QStringList{QStringLiteral("3:20 invalid-math math"),
                                                 QStringLiteral("5:1 invalid-math math"),
                                                 QStringLiteral("14:1 invalid-diagram diagram"),
                                                 QStringLiteral("1:11 invalid-math math")}));
    const QJsonArray problems =
        result.value(QStringLiteral("documents")).toArray().first().toObject().value(QStringLiteral("problems")).toArray();
    QCOMPARE(problems.at(0).toObject().value(QStringLiteral("file")).toString(), path);
    QCOMPARE(problems.at(3).toObject().value(QStringLiteral("file")).toString(), chapter);
    QVERIFY2(problems.at(0).toObject().value(QStringLiteral("message")).toString().contains(
                 QStringLiteral("fake refusal of refuse this")),
             qPrintable(ran.said()));
    QVERIFY2(problems.at(2).toObject().value(QStringLiteral("message")).toString().contains(QStringLiteral("fake refusal")),
             qPrintable(ran.said()));
    // The versions of what drew them, and where the helpers are.
    const QJsonObject renderers = result.value(QStringLiteral("renderers")).toObject();
    QCOMPARE(renderers.value(QStringLiteral("math")).toString(), QStringLiteral("RaTeX 9.9.1"));
    QCOMPARE(renderers.value(QStringLiteral("diagram")).toString(), QStringLiteral("Merman 9.9.2"));
    QVERIFY(renderers.value(QStringLiteral("markdown")).toString().startsWith(QStringLiteral("parch ")));
    const QJsonObject used = result.value(QStringLiteral("helpers")).toObject();
    QCOMPARE(used.value(QStringLiteral("render-svg")).toString(), helpers.ratex);
    QCOMPARE(used.value(QStringLiteral("merman-cli")).toString(), helpers.merman);
    QVERIFY(result.value(QStringLiteral("skipped")).toArray().isEmpty());
    QVERIFY(helperRuns(log) >= 5);

    // As text, a line each, as for every other problem.
    ran = runCliWith(root, paths, helpers, {QStringLiteral("check"), path});
    QVERIFY2(ran.code == Cli::Problems, qPrintable(ran.said()));
    QVERIFY2(ran.out.contains(QStringLiteral("doc.md:3:20: warning: ")) && ran.out.contains(QStringLiteral("[invalid-math]")),
             qPrintable(ran.said()));
    QVERIFY2(ran.out.contains(QStringLiteral("doc.md:14:1: warning: ")) && ran.out.contains(QStringLiteral("[invalid-diagram]")),
             qPrintable(ran.said()));
    QVERIFY2(ran.out.contains(QStringLiteral("chapter.md:1:11: warning: ")), qPrintable(ran.said()));

    // A helper that fails is a problem too: the reader would not see the formula.
    const QString crashing = root + QStringLiteral("/crash.md");
    writeFile(crashing, "Then $crash now$.\n");
    ran = runCliWith(root, paths, helpers, {QStringLiteral("check"), crashing, QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Problems, qPrintable(ran.said()));
    QCOMPARE(problemPlaces(ran.json()), QStringList{QStringLiteral("1:6 math-not-drawn math")});

    // A document whose formulas and diagrams all draw has nothing wrong.
    const QString clean = root + QStringLiteral("/clean.md");
    writeFile(clean, "Fine $x^2$.\n\n```mermaid\nflowchart LR\n  A --> B\n```\n");
    const int before = helperRuns(log);
    ran = runCliWith(root, paths, helpers, {QStringLiteral("check"), clean});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    QVERIFY(helperRuns(log) > before);

    // Without the record of their versions, Merman says its own; render-svg cannot.
    QVERIFY(QFile::remove(helpersFolder + QStringLiteral("/.pins")));
    ran = runCliWith(root, paths, helpers, {QStringLiteral("check"), clean, QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    QCOMPARE(ran.json().value(QStringLiteral("renderers")).toObject().value(QStringLiteral("diagram")).toString(),
             QStringLiteral("Merman 0.0.7"));
    QCOMPARE(ran.json().value(QStringLiteral("renderers")).toObject().value(QStringLiteral("math")).toString(),
             QStringLiteral("RaTeX (version unknown)"));

    // Without the helpers, formulas and diagrams are not checked, and check
    // says so without failing for it.
    ran = runCliWith(root, paths, HelperSet{}, {QStringLiteral("check"), path, QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    result = ran.json();
    QVERIFY(problemPlaces(result).isEmpty());
    QCOMPARE(result.value(QStringLiteral("skipped")).toArray(),
             (QJsonArray{QStringLiteral("math"), QStringLiteral("diagram")}));
    QVERIFY(!result.value(QStringLiteral("renderers")).toObject().contains(QStringLiteral("math")));
    ran = runCliWith(root, paths, HelperSet{}, {QStringLiteral("check"), path});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    QVERIFY2(ran.err.contains(QStringLiteral("4 formulas and 2 diagrams were not checked")), qPrintable(ran.said()));
    // A document with neither has nothing to skip.
    const QString plain = root + QStringLiteral("/plain.md");
    writeFile(plain, "Words only.\n");
    ran = runCliWith(root, paths, HelperSet{}, {QStringLiteral("check"), plain, QStringLiteral("--json")});
    QVERIFY2(ran.code == Cli::Ok, qPrintable(ran.said()));
    QVERIFY(ran.json().value(QStringLiteral("skipped")).toArray().isEmpty());
    QVERIFY(ran.err.isEmpty());
}

namespace {

bool runCommand(QQuickWindow *window, const char *command)
{
    return QMetaObject::invokeMethod(window, "run", Q_ARG(QVariant, QString::fromLatin1(command)));
}

// The problems a Check document dialog lists, as "place code".
QStringList listedPlaces(QObject *dialog)
{
    QStringList places;
    for (const QVariant &problem : dialog->property("problems").toList()) {
        const QVariantMap row = problem.toMap();
        places.append(row.value(QStringLiteral("place")).toString() + u' ' + row.value(QStringLiteral("code")).toString());
    }
    return places;
}

ViewState activeView(ShellFixture &shell)
{
    TabModel *tabs = shell.context()->tabs();
    return tabs->viewState(tabs->activeIndex());
}

// Pictures that are all still to come, as a preview's are before they are drawn.
class WaitingResources : public preview::Resources {
public:
    preview::Picture math(const QString &tex, bool, qreal) override { return waiting(QStringLiteral("math/") + tex); }
    preview::Picture diagram(const QString &source) override { return waiting(QStringLiteral("diagram/") + source); }
    preview::Picture image(const md::Destination &, const QString &written) override
    {
        return waiting(QStringLiteral("image/") + written);
    }

private:
    static preview::Picture waiting(const QString &key)
    {
        preview::Picture picture;
        picture.key = key;
        return picture;
    }
};

QString messageOf(const md::Document &document, md::DiagnosticCode code)
{
    for (const md::Diagnostic &diagnostic : document.diagnostics)
        if (diagnostic.code == code) return diagnostic.message;
    return QStringLiteral("(no such diagnostic)");
}

} // namespace

void checkDocumentListsEveryProblem()
{
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonicalPath(files.path());
    const QString path = root + QStringLiteral("/doc.md");
    const QString chapter = root + QStringLiteral("/chapter.md");
    writeFile(path, "# Top\n"
                    "\n"
                    "![gone](gone.png) and [up](#nowhere).\n"
                    "\n"
                    "Dear [%customer].\n"
                    "\n"
                    "chapter.md\n"
                    "\n"
                    "Bad $refuse this$ here.\n");
    writeFile(chapter, "Chapter [%nobody] ends.\n");
    writeFakeHelpers(root + QStringLiteral("/helpers"), root + QStringLiteral("/helpers.log"));
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    shell.backend->setRenderService(std::make_unique<RenderService>(
        renderOptions(HelperSet::in(root + QStringLiteral("/helpers")), root + QStringLiteral("/render"))));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1000);

    // The command keeps its name in the registry, its keys and its place in
    // the menu; it is called Check document now.
    QCOMPARE(shell.backend->commands()->title(QStringLiteral("checkLinks")), QStringLiteral("Check document…"));
    QObject *entry = shell.object(QStringLiteral("checkLinksEntry"));
    QVERIFY(entry && entry->property("enabled").toBool());
    QObject *dialog = shell.object(QStringLiteral("checkDocumentDialog"));
    QVERIFY(dialog);
    QVERIFY(runCommand(window, "checkLinks"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_VERIFY(!dialog->property("checking").toBool());
    // Every problem as `parch check` finds it, the link checks among them,
    // and the formula its helper refused, in file and source order, the
    // included file's in that file.
    QCOMPARE(listedPlaces(dialog), (QStringList{QStringLiteral("doc.md:3:1 unresolved-asset"),
                                                QStringLiteral("doc.md:3:23 unresolved-anchor"),
                                                QStringLiteral("doc.md:5:6 unresolved-variable"),
                                                QStringLiteral("doc.md:9:5 invalid-math"),
                                                QStringLiteral("chapter.md:1:9 unresolved-variable")}));
    QCOMPARE(dialog->property("summary").toString(), QStringLiteral("5 problems."));
    const QString listing = dialog->property("listing").toString();
    QVERIFY2(listing.startsWith(QStringLiteral("doc.md:3:1: The image \"gone.png\" is not there")), qPrintable(listing));
    QVERIFY2(listing.contains(QStringLiteral("\ndoc.md:9:5: The formula cannot be drawn: ")), qPrintable(listing));
    QTest::qWait(50);
    QStringList problems;
    QVERIFY2(everyVisibleItemFitsWithin(window, &problems), qPrintable(problems.join("; ")));
    QVERIFY2(unnamedAccessibleItems(window).isEmpty(), qPrintable(unnamedAccessibleItems(window).join(", ")));
    // The keys go to the list, on the first problem, and Tab reaches Close.
    QTRY_VERIFY(shell.item(QStringLiteral("checkProblems"))->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Tab);
    QCOMPARE(window->activeFocusItem()->objectName(), QStringLiteral("closeChoice"));
    QTest::keyClick(window, Qt::Key_Tab);
    QVERIFY(shell.item(QStringLiteral("checkProblems"))->hasActiveFocus());
    // Enter on the chapter's opens the chapter, its place selected.
    for (int i = 0; i < 4; ++i) QTest::keyClick(window, Qt::Key_Down);
    QCOMPARE(shell.item(QStringLiteral("checkProblems"))->property("currentIndex").toInt(), 4);
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QTRY_COMPARE(shell.document()->path(), chapter);
    QCOMPARE(activeView(shell).anchor, 8);
    QCOMPARE(activeView(shell).cursor, 8 + int(QStringLiteral("[%nobody]").size()));

    // Off the UI thread, in the text as it was when the check began: an
    // edit made meanwhile is not read, and the place is found where it is now.
    shell.backend->openFiles({path});
    QTRY_COMPARE(shell.document()->path(), path);
    DocumentChecker::setWorkerDelay(300);
    const auto undelayed = qScopeGuard([] { DocumentChecker::setWorkerDelay(0); });
    QVERIFY(runCommand(window, "checkLinks"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QVERIFY(dialog->property("checking").toBool());
    QCOMPARE(dialog->property("summary").toString(), QStringLiteral("Checking…"));
    shell.document()->buffer()->replace({0, 0}, QStringLiteral("Intro.\n\n"));
    QTest::qWait(50);
    QVERIFY(dialog->property("checking").toBool());
    QTRY_VERIFY_WITH_TIMEOUT(!dialog->property("checking").toBool(), 10000);
    QCOMPARE(listedPlaces(dialog).first(), QStringLiteral("doc.md:3:1 unresolved-asset"));
    QTRY_VERIFY(shell.item(QStringLiteral("checkProblems"))->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QTRY_COMPARE(activeView(shell).anchor, 15);
    DocumentChecker::setWorkerDelay(0);

    // Without the math helper, formulas are not checked, and the dialog says so.
    RenderService::Options none = renderOptions(HelperSet(), root + QStringLiteral("/render"));
    shell.backend->setRenderService(std::make_unique<RenderService>(none));
    QVERIFY(runCommand(window, "checkLinks"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_VERIFY(!dialog->property("checking").toBool());
    QCOMPARE(dialog->property("summary").toString(),
             QStringLiteral("4 problems. One formula was not checked: render-svg was not found."));
    QTest::keyClick(window, Qt::Key_Escape);
    QTRY_VERIFY(!dialog->property("opened").toBool());

    // A draft's problems open in the draft.
    shell.backend->newDraft();
    Document *draft = shell.document();
    QVERIFY(draft->isDraft());
    draft->buffer()->replace({0, 0}, QStringLiteral("Hello [%who].\n"));
    QVERIFY(runCommand(window, "checkLinks"));
    QTRY_VERIFY(dialog->property("opened").toBool());
    QTRY_VERIFY(!dialog->property("checking").toBool());
    QCOMPARE(dialog->property("summary").toString(), QStringLiteral("One problem."));
    QCOMPARE(listedPlaces(dialog), QStringList{draft->title() + QStringLiteral(":1:7 unresolved-variable")});
    QTRY_VERIFY(shell.item(QStringLiteral("checkProblems"))->hasActiveFocus());
    QTest::keyClick(window, Qt::Key_Return);
    QTRY_VERIFY(!dialog->property("opened").toBool());
    QCOMPARE(shell.document(), draft);
    QTRY_COMPARE(activeView(shell).anchor, 6);
}

void previewMarksWhatLeadsNowhere()
{
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonicalPath(files.path());
    const QString path = root + QStringLiteral("/doc.md");
    writeFile(root + QStringLiteral("/chapter.md"), "Chapter text.\n");
    writeFile(root + QStringLiteral("/loop.md"), "Loop.\n\nloop.md\n");
    const QString text = QStringLiteral("---\ncustomer: A. Example\n---\n\n"
                                        "Dear [%customer] and [%nobody].\n\n"
                                        "See [the part][Missing label], [up](#nowhere) and the note[^gone].\n\n"
                                        "missing.md\n\n"
                                        "loop.md\n\n"
                                        "chapter.md\n");
    writeFile(path, text.toUtf8());
    const auto read = [&] {
        md::FileIncludeResolver resolver;
        md::Source source;
        source.path = path;
        source.resolver = &resolver;
        return DocumentReading::read(text, {}, source);
    };
    const DocumentReading::Reading reading = read();
    const md::Document &document = reading.document;

    // Built as the preview builds it: what leads nowhere is in the danger
    // ink, with the engine's diagnostic as its tooltip.
    QObject owner;
    QTextDocument built;
    WaitingResources resources;
    preview::BuildOptions options;
    options.islandParent = &owner;
    options.citations = &reading.formatted;
    preview::Builder builder(document, PreviewStyle::paper(), &resources, options);
    builder.buildAll(&built);
    const auto formatOf = [&](const QString &needle) {
        QTextCursor found = built.find(needle, 0, QTextDocument::FindCaseSensitively);
        if (found.isNull()) return QTextCharFormat();
        found.setPosition(found.selectionStart() + 1);
        return found.charFormat();
    };
    const auto marked = [&](const QString &needle, md::DiagnosticCode code) {
        const QTextCharFormat format = formatOf(needle);
        return format.intProperty(PreviewStyle::InkProperty) == int(PreviewStyle::Danger)
               && format.toolTip() == messageOf(document, code);
    };
    QVERIFY(marked(QStringLiteral("[%nobody]"), md::DiagnosticCode::UnresolvedVariable));
    QVERIFY(marked(QStringLiteral("the part"), md::DiagnosticCode::UnresolvedReference));
    QVERIFY(marked(QStringLiteral("Missing label"), md::DiagnosticCode::UnresolvedReference));
    QVERIFY(marked(QStringLiteral("up"), md::DiagnosticCode::UnresolvedAnchor));
    QVERIFY(marked(QStringLiteral("[^gone]"), md::DiagnosticCode::MissingFootnote));
    QVERIFY(marked(QStringLiteral("missing.md"), md::DiagnosticCode::IncludeNotFound));
    // A cycle shows the chain of files that leads back.
    QVERIFY(marked(QStringLiteral("loop.md"), md::DiagnosticCode::IncludeCycle));
    QVERIFY2(formatOf(QStringLiteral("loop.md")).toolTip().contains(QStringLiteral("doc.md → loop.md → loop.md")),
             qPrintable(formatOf(QStringLiteral("loop.md")).toolTip()));
    // What resolves reads as ever.
    for (const char *fine : {"A. Example", "Chapter text.", "Loop.", "See"})
        QVERIFY2(formatOf(QString::fromUtf8(fine)).intProperty(PreviewStyle::InkProperty) != int(PreviewStyle::Danger), fine);
    QCOMPARE(preview::Unresolved(document).messages(),
             (QStringList{messageOf(document, md::DiagnosticCode::UnresolvedVariable),
                          messageOf(document, md::DiagnosticCode::UnresolvedReference),
                          messageOf(document, md::DiagnosticCode::UnresolvedAnchor),
                          messageOf(document, md::DiagnosticCode::MissingFootnote),
                          messageOf(document, md::DiagnosticCode::IncludeNotFound),
                          messageOf(document, md::DiagnosticCode::IncludeCycle)}));
    // A part whose reason changes is built again: an include that is now a
    // folder rather than nothing at all.
    const std::vector<preview::Part> before = preview::parts(document, &reading.formatted);
    QVERIFY(QDir(root).mkdir(QStringLiteral("missing.md")));
    const DocumentReading::Reading again = read();
    const std::vector<preview::Part> after = preview::parts(again.document, &again.formatted);
    QCOMPARE(after.size(), before.size());
    int changed = 0;
    for (size_t i = 0; i < before.size(); ++i) changed += before[i].key != after[i].key;
    QCOMPARE(changed, 1);
    QVERIFY(QDir(root).rmdir(QStringLiteral("missing.md")));

    // In the window, the preview tells assistive technology too.
    QAccessible::setActive(true);
    const auto inactive = qScopeGuard([] { QAccessible::setActive(false); });
    ShellFixture shell({.files = {path}});
    QVERIFY2(shell.ok(), qPrintable(shell.error));
    QQuickWindow *window = shell.window.get();
    activate(window);
    resizeWindow(window, 1280);
    QVERIFY(runCommand(window, "viewSplit"));
    Preview *preview = nullptr;
    QTRY_VERIFY((preview = qobject_cast<Preview *>(shell.object(QStringLiteral("preview")))));
    QTRY_VERIFY_WITH_TIMEOUT(preview->current() && !preview->working(), 10000);
    QCOMPARE(preview->unresolved(), preview::Unresolved(document).messages().join(QStringLiteral(". ")) + u'.');
    QAccessibleInterface *access = QAccessible::queryAccessibleInterface(shell.item(QStringLiteral("previewSurface")));
    QVERIFY(access);
    QVERIFY2(access->text(QAccessible::Description).contains(messageOf(document, md::DiagnosticCode::IncludeCycle)),
             qPrintable(access->text(QAccessible::Description)));
    // Once nothing leads nowhere, it says nothing of it.
    SourceBuffer *buffer = shell.document()->buffer();
    buffer->replace({0, int(buffer->text().size())}, QStringLiteral("Plain words.\n"));
    QTRY_VERIFY_WITH_TIMEOUT(preview->current() && preview->unresolved().isEmpty(), 10000);
    QCOMPARE(access->text(QAccessible::Description), QStringLiteral("The document as it reads. It cannot be edited here."));
}

namespace {

const QString mathCorpusPath = QStringLiteral(PARCH_SOURCE_DIR "/tests/fixtures/math/katex-0.16.json");

QJsonObject mathCorpus()
{
    return QJsonDocument::fromJson(readFile(mathCorpusPath)).object();
}

QString guideText()
{
    return QString::fromUtf8(readFile(QStringLiteral(PARCH_SOURCE_DIR "/docs/guide.md")));
}

// A section of the guide, from its heading to the next heading of its level or above.
QString guideSection(const QString &heading)
{
    const QString guide = guideText();
    const qsizetype start = guide.indexOf(u'\n' + heading + u'\n');
    if (start < 0) return {};
    const int level = int(heading.indexOf(u' '));
    qsizetype end = start + heading.size() + 2;
    while (true) {
        end = guide.indexOf(QStringLiteral("\n#"), end);
        if (end < 0) return guide.mid(start);
        int hashes = 0;
        while (end + 1 + hashes < guide.size() && guide[end + 1 + hashes] == u'#') ++hashes;
        if (hashes <= level) return guide.mid(start, end - start);
        ++end;
    }
}

} // namespace

void katexCorpusDrawsAsRecorded()
{
    // Each expression of the corpus drawn as the preview draws a display
    // formula, with the pinned helper; what draws and what does not is
    // recorded, so that another version of the helper shows what it changes.
    const HelperSet helpers = realHelpers();
    if (helpers.ratex.isEmpty())
        QSKIP("The real math helper is not in build/helpers: run bin/fetch-helpers to test it");
    QJsonObject corpus = mathCorpus();
    QJsonArray expressions = corpus.value(QStringLiteral("expressions")).toArray();
    QVERIFY(expressions.size() > 1000);
    QTemporaryDir folder;
    RenderService service(renderOptions(helpers, folder.path()));
    RenderResults results(service);
    std::vector<quint64> tickets;
    for (const QJsonValue &value : std::as_const(expressions)) {
        RenderRequest request;
        request.kind = RenderKind::DisplayMath;
        request.source = value.toObject().value(QStringLiteral("tex")).toString();
        request.fontSize = 20;
        tickets.push_back(service.render(request));
    }
    QStringList changes;
    int drawn = 0;
    for (qsizetype i = 0; i < expressions.size(); ++i) {
        QJsonObject expression = expressions.at(i).toObject();
        const QString command = expression.value(QStringLiteral("command")).toString();
        QVERIFY2(results.wait(tickets[size_t(i)]), qPrintable(command));
        const RenderResult result = results[tickets[size_t(i)]];
        QVERIFY2(result.status != RenderResult::Failed, qPrintable(command + QStringLiteral(": ") + result.diagnostic));
        drawn += result.ok();
        const QJsonValue recorded = expression.value(QStringLiteral("drawn"));
        if (!recorded.isBool() || recorded.toBool() != result.ok())
            changes << QStringLiteral("%1 %2 (%3)%4")
                           .arg(result.ok() ? QStringLiteral("now draws") : QStringLiteral("no longer draws"), command,
                                expression.value(QStringLiteral("tex")).toString(),
                                result.ok() ? QString() : QStringLiteral(": ") + result.diagnostic);
        expression.insert(QStringLiteral("drawn"), result.ok());
        if (result.ok()) expression.remove(QStringLiteral("diagnostic"));
        else expression.insert(QStringLiteral("diagnostic"), result.diagnostic);
        expressions[i] = expression;
    }
    qInfo().noquote() << QStringLiteral("%1 of %2 expressions draw with %3").arg(drawn).arg(expressions.size())
                                                                            .arg(helpers.ratexVersion());
    // PARCH_RECORD_MATH_CORPUS=1 writes what the helper does now as the record.
    if (qEnvironmentVariableIsSet("PARCH_RECORD_MATH_CORPUS")) {
        corpus.insert(QStringLiteral("helper"), helpers.ratexVersion());
        corpus.insert(QStringLiteral("expressions"), expressions);
        QSaveFile out(mathCorpusPath);
        QVERIFY(out.open(QIODevice::WriteOnly));
        out.write(QJsonDocument(corpus).toJson(QJsonDocument::Indented));
        QVERIFY(out.commit());
        return;
    }
    QCOMPARE(corpus.value(QStringLiteral("helper")).toString(), helpers.ratexVersion());
    QVERIFY2(changes.isEmpty(), qPrintable(QStringLiteral("%1 changes from the record:\n").arg(changes.size())
                                           + changes.join(u'\n')));
}

void guideListsWhatMathCannotDraw()
{
    // The guide's math section says how much of the corpus draws and names
    // each command that does not, as the record has them.
    const QJsonArray expressions = mathCorpus().value(QStringLiteral("expressions")).toArray();
    QStringList undrawn;
    int drawn = 0;
    for (const QJsonValue &value : expressions) {
        const QJsonObject expression = value.toObject();
        if (expression.value(QStringLiteral("drawn")).toBool()) ++drawn;
        else undrawn << expression.value(QStringLiteral("command")).toString();
    }
    const QString section = guideSection(QStringLiteral("### Math"));
    QVERIFY2(!section.isEmpty(), "The guide has no ### Math section");
    const QLocale english(QLocale::English);
    const QString counted = QStringLiteral("%1 of its %2").arg(english.toString(drawn), english.toString(expressions.size()));
    QVERIFY2(section.contains(counted), qPrintable(QStringLiteral("The guide does not say “%1”").arg(counted)));
    // The paragraph that names them, after "do not draw:".
    const qsizetype from = section.indexOf(QStringLiteral("do not draw:"));
    QVERIFY2(from >= 0, "The guide does not list what does not draw");
    const QString paragraph = section.mid(from, section.indexOf(QStringLiteral("\n\n"), from) - from);
    // Each command named there, but for \left and \right, which say how
    // the delimiters are used, and each one once.
    QStringList named;
    static const QRegularExpression code(QStringLiteral("`([^`]+)`"));
    for (auto match = code.globalMatch(paragraph); match.hasNext();) {
        const QString command = match.next().captured(1);
        if (command != u"\\left" && command != u"\\right" && !named.contains(command)) named << command;
    }
    undrawn.sort();
    named.sort();
    QCOMPARE(named, undrawn);
    // The helper it names is the one parch is pinned to.
    const QString pins = QString::fromUtf8(readFile(QStringLiteral(PARCH_SOURCE_DIR "/bin/fetch-helpers")));
    const QString ratex = QRegularExpression(QStringLiteral("RATEX_VERSION=(\\S+)")).match(pins).captured(1);
    QVERIFY(!ratex.isEmpty());
    QVERIFY2(section.contains(QStringLiteral("RaTeX\n") + ratex) || section.contains(QStringLiteral("RaTeX ") + ratex),
             qPrintable(ratex));
}

void guideNamesEveryDiagramFamily()
{
    // The guide names the Mermaid version Merman follows and the pinned
    // Merman, and every family the diagram corpus draws, by the keyword
    // each of its diagrams starts with.
    const QString section = guideSection(QStringLiteral("### Diagrams"));
    QVERIFY2(!section.isEmpty(), "The guide has no ### Diagrams section");
    const QString flat = section.simplified();
    const QString pins = QString::fromUtf8(readFile(QStringLiteral(PARCH_SOURCE_DIR "/bin/fetch-helpers")));
    const QString merman = QRegularExpression(QStringLiteral("MERMAN_VERSION=(\\d+\\.\\d+)")).match(pins).captured(1);
    QVERIFY(!merman.isEmpty());
    QVERIFY2(flat.contains(QStringLiteral("Mermaid 12.3 syntax and drawn through Merman ") + merman), qPrintable(merman));
    QFile file(QStringLiteral(PARCH_SOURCE_DIR "/tests/fixtures/mermaid/corpus.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonArray fixtures = QJsonDocument::fromJson(file.readAll()).array();
    QSet<QString> families;
    for (const QJsonValue &value : fixtures) {
        const QJsonObject fixture = value.toObject();
        if (!fixture.value(QStringLiteral("valid")).toBool()) continue;
        families.insert(fixture.value(QStringLiteral("family")).toString());
        // The keyword: the first word after any front matter and comments.
        QStringList lines = fixture.value(QStringLiteral("source")).toString().split(u'\n');
        if (lines.value(0).trimmed() == u"---") {
            lines.removeFirst();
            while (!lines.isEmpty() && lines.takeFirst().trimmed() != u"---") {
            }
        }
        QString keyword;
        for (const QString &line : std::as_const(lines)) {
            const QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith(QStringLiteral("%%"))) continue;
            keyword = trimmed.section(u' ', 0, 0);
            break;
        }
        QVERIFY2(section.contains(u'`' + keyword + u'`'), qPrintable(QStringLiteral("The guide does not name `%1`").arg(keyword)));
    }
    // One row of the table for each family.
    const qsizetype rows = section.count(QStringLiteral("\n| ")) - 2; // the header and its rule
    QCOMPARE(int(rows), int(families.size()));
    QVERIFY2(flat.contains(QStringLiteral("These %1 families").arg(families.size())), qPrintable(section));
}

namespace {

// The examples of a specification's JSON whose HTML the engine does not
// give, by their numbers.
QList<int> failingExamples(const QJsonArray &examples, const md::Options &options, const md::HtmlOptions &html,
                           bool closeInputs = false)
{
    QList<int> failed;
    for (const QJsonValue &value : examples) {
        const QJsonObject example = value.toObject();
        QString expected = example.value(QStringLiteral("html")).toString();
        // The renderer closes the empty input element, as it closes br and img.
        if (closeInputs) expected.replace(QStringLiteral("type=\"checkbox\">"), QStringLiteral("type=\"checkbox\" />"));
        const md::Document document = md::parse(example.value(QStringLiteral("markdown")).toString(), options);
        if (md::toHtml(document, html) != expected) failed.append(example.value(QStringLiteral("example")).toInt());
    }
    return failed;
}

// The backticked words of a piece of the guide.
QStringList codeIn(const QString &text)
{
    QStringList words;
    static const QRegularExpression code(QStringLiteral("`([^`]+)`"));
    for (auto match = code.globalMatch(text); match.hasNext();) words << match.next().captured(1);
    return words;
}

// The line of a section that starts with a prefix, without it.
QString lineStarting(const QString &section, const QString &prefix)
{
    for (const QString &line : section.split(u'\n'))
        if (line.startsWith(prefix)) return line.mid(prefix.size());
    return {};
}

} // namespace

void guideConformanceFiguresHold()
{
    // The figures the guide's conformance section gives, measured here
    // against the specifications' own examples.
    const QJsonArray commonMark = QJsonDocument::fromJson(
        readFile(QStringLiteral(PARCH_SOURCE_DIR "/tests/fixtures/commonmark-0.31.2.json"))).array();
    const QJsonArray gfm = QJsonDocument::fromJson(
        readFile(QStringLiteral(PARCH_SOURCE_DIR "/tests/fixtures/gfm-extensions.json"))).array();
    QCOMPARE(commonMark.size(), 652);
    QCOMPARE(gfm.size(), 23);
    const md::HtmlOptions passthrough = md::HtmlOptions::commonMark();
    md::HtmlOptions safe = passthrough;
    safe.rawHtml = md::HtmlOptions::RawHtml::Safe;
    md::Options core = md::Options::standard();
    core.autolinks = false;
    const QList<int> coreFailures = failingExamples(commonMark, core, passthrough);
    const QList<int> standardFailures = failingExamples(commonMark, md::Options::standard(), passthrough);
    const QList<int> safeFailures = failingExamples(commonMark, md::Options::standard(), safe);
    const QList<int> writerFailures = failingExamples(commonMark, md::Options::writer(), passthrough);
    const QList<int> gfmStandard = failingExamples(gfm, md::Options::standard(), passthrough, true);
    const QList<int> gfmWriter = failingExamples(gfm, md::Options::writer(), passthrough, true);
    qInfo().noquote() << QStringLiteral("CommonMark: %1 fail as CommonMark, %2 in Standard, %3 in Standard with the "
                                        "HTML policy, %4 in Writer; GFM: %5 in Standard, %6 in Writer")
                             .arg(coreFailures.size())
                             .arg(standardFailures.size())
                             .arg(safeFailures.size())
                             .arg(writerFailures.size())
                             .arg(gfmStandard.size())
                             .arg(gfmWriter.size());
    const auto listed = [](const QList<int> &examples) {
        QStringList numbers;
        for (int example : examples) numbers << QString::number(example);
        return numbers.join(u' ');
    };
    qInfo().noquote() << "Differing with the HTML policy:" << listed(safeFailures);
    qInfo().noquote() << "Differing in the Writer profile:" << listed(writerFailures);
    QVERIFY(coreFailures.isEmpty());
    QVERIFY(gfmStandard.isEmpty() && gfmWriter.isEmpty());

    const QString section = guideSection(QStringLiteral("## Conformance"));
    QVERIFY2(!section.isEmpty(), "The guide has no ## Conformance section");
    const QString flat = section.simplified();
    const auto says = [&](const QString &words) {
        if (!flat.contains(words)) qWarning().noquote() << "The guide's conformance section does not say:" << words;
        return flat.contains(words);
    };
    QVERIFY(says(QStringLiteral("all 652 examples of the CommonMark 0.31.2 specification")));
    QStringList numbers;
    for (int example : standardFailures) numbers << QString::number(example);
    const QString differing = numbers.size() > 1 ? numbers.mid(0, numbers.size() - 1).join(QStringLiteral(", "))
                                                       + QStringLiteral(" and ") + numbers.last()
                                                 : numbers.join(QString());
    QVERIFY(says(QStringLiteral("%1 of the 652 pass; examples %2").arg(652 - standardFailures.size()).arg(differing)));
    QVERIFY(says(QStringLiteral("all 23 examples of the GFM 0.29 extensions")));
    QVERIFY(says(QStringLiteral("%1 of the 652 examples give the same HTML").arg(652 - safeFailures.size())));
    QVERIFY(says(QStringLiteral("Writer-compatible profile gives the same HTML for %1 of the 652")
                     .arg(652 - writerFailures.size())));

    // The elements and attributes it lists are the engine's policy.
    const md::HtmlPolicy policy = md::htmlPolicy();
    QCOMPARE(codeIn(lineStarting(section, QStringLiteral("- **Text level**: "))), policy.textElements);
    QCOMPARE(codeIn(lineStarting(section, QStringLiteral("- **Blocks**: "))), policy.blockElements);
    QHash<QString, QStringList> attributes;
    for (const QString &line : section.split(u'\n')) {
        if (!line.startsWith(QStringLiteral("| `"))) continue;
        const QStringList cells = line.split(u'|');
        if (cells.size() < 4) continue;
        for (const QString &element : codeIn(cells.at(1))) attributes[element] = codeIn(cells.at(2));
    }
    QCOMPARE(attributes, policy.attributes);
    // What the policy shows as text, by example.
    for (const char *shown : {"<div>", "<span>", "<script>", "<style>", "<iframe>"}) {
        QVERIFY2(section.contains(u'`' + QString::fromLatin1(shown) + u'`'), shown);
        const md::Document document = md::parse(QString::fromLatin1(shown) + QStringLiteral("x"));
        QVERIFY2(std::any_of(document.diagnostics.cbegin(), document.diagnostics.cend(),
                             [](const md::Diagnostic &d) { return d.code == md::DiagnosticCode::UnsupportedHtml; }),
                 shown);
    }
}

namespace {

// A style of one's own, written for a language other than English.
const char *germanStyle = R"csl(<?xml version="1.0" encoding="utf-8"?>
<style xmlns="http://purl.org/net/xbiblio/csl" class="in-text" version="1.0" default-locale="de-DE">
  <info>
    <title>A style for German</title>
    <id>http://example.org/styles/german</id>
    <updated>2024-01-01T00:00:00+00:00</updated>
  </info>
  <citation>
    <layout prefix="(" suffix=")">
      <names variable="author"><name form="short"/></names>
    </layout>
  </citation>
</style>
)csl";

// The same in CSL-M, the legal extension: an institution beside the names.
const char *cslMStyle = R"csl(<?xml version="1.0" encoding="utf-8"?>
<style xmlns="http://purl.org/net/xbiblio/csl" class="in-text" version="1.1mlz1">
  <info>
    <title>A legal style</title>
    <id>http://example.org/styles/legal</id>
    <updated>2024-01-01T00:00:00+00:00</updated>
  </info>
  <citation>
    <layout>
      <names variable="author"><name form="short"/><institution/></names>
    </layout>
  </citation>
</style>
)csl";

int lineCount(const QByteArray &text, bool skipComments)
{
    int count = 0;
    for (const QByteArray &line : text.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (!trimmed.isEmpty() && !(skipComments && trimmed.startsWith('#'))) ++count;
    }
    return count;
}

} // namespace

void guideStatesWhatCitationsCannotDo()
{
    const QString section = guideSection(QStringLiteral("## Citations and bibliographies")).simplified();
    QVERIFY2(!section.isEmpty(), "The guide has no ## Citations and bibliographies section");
    // The CSL suite's figure: every fixture but the known failures passes,
    // as citationCslTestSuite holds the suite to.
    const int fixtures = lineCount(readFile(QStringLiteral(PARCH_SOURCE_DIR "/tests/csl-suite/fixtures.sha256")), false);
    const int failing = lineCount(readFile(QStringLiteral(PARCH_SOURCE_DIR "/tests/csl-suite/known-failures")), true);
    QVERIFY(fixtures > 500 && failing > 0);
    const QString figure = QStringLiteral("%1 of the %2 pass").arg(fixtures - failing).arg(fixtures);
    QVERIFY2(section.contains(figure), qPrintable(figure));

    // A style for another language is formatted in US English, and check says so.
    QVERIFY(section.contains(QStringLiteral("Only English is bundled")));
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString root = canonicalPath(files.path());
    writeFile(root + QStringLiteral("/german.csl"), germanStyle);
    writeFile(root + QStringLiteral("/legal.csl"), cslMStyle);
    writeFile(root + QStringLiteral("/refs.json"),
              R"([{"id": "doe", "type": "book", "author": [{"family": "Doe", "given": "Jo"}], "title": "T"}])");
    const QString path = root + QStringLiteral("/doc.md");
    writeFile(path, "---\nbibliography: refs.json\ncsl: german.csl\n---\n\nAs shown [@doe].\n");
    const AppPaths paths = AppPaths::under(root + QStringLiteral("/app"));
    Ran ran = runCli(root, paths, {QStringLiteral("check"), path});
    QVERIFY2(ran.out.contains(QStringLiteral("No citation locale for “de-DE” was found; English (US) is used.")),
             qPrintable(ran.said()));
    // A CSL-M style is refused, naming the element CSL does not define.
    QVERIFY(section.contains(QStringLiteral("A CSL-M style, the legal extension of CSL, is refused with the element it "
                                            "uses that CSL does not define")));
    writeFile(path, "---\nbibliography: refs.json\ncsl: legal.csl\n---\n\nAs shown [@doe].\n");
    ran = runCli(root, paths, {QStringLiteral("check"), path});
    QVERIFY2(ran.code == Cli::Problems, qPrintable(ran.said()));
    QVERIFY2(ran.out.contains(QStringLiteral("<institution>")), qPrintable(ran.said()));
}

} // namespace DiagnosticsTests
