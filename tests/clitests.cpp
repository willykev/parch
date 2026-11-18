#include "clitests.h"

#include "apppaths.h"
#include "cli.h"
#include "clicommands.h"
#include "dialectchoice.h"
#include "documentcheck.h"
#include "exporter.h"
#include "htmlexport.h"
#include "markdownexport.h"
#include "mdhtml.h"
#include "mdinclude.h"
#include "renderhelpers.h"
#include "testfiles.h"
#include "zippackage.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QImage>
#include <QScopeGuard>
#include <QSet>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <sys/stat.h>

namespace CliTests {
namespace {

// What a command printed and how it ended.
struct Ran {
    int code = -1;
    QString out;
    QString err;

    QJsonObject json() const { return QJsonDocument::fromJson(out.toUtf8()).object(); }
    QString said() const { return QStringLiteral("exit %1\n--- out\n%2\n--- err\n%3").arg(code).arg(out, err); }
};

// A folder of documents for the commands to run in, with a home and parch's
// own folders of its own.
class Sandbox {
public:
    Sandbox() : m_root(QFileInfo(m_dir.path()).canonicalFilePath())
    {
        QDir().mkpath(home());
    }

    bool isValid() const { return m_dir.isValid(); }
    QString root() const { return m_root; }
    QString path(const QString &relative) const { return m_root + u'/' + relative; }
    QString home() const { return path(QStringLiteral("home")); }
    AppPaths paths() const { return AppPaths::under(path(QStringLiteral("app"))); }

    void write(const QString &relative, const QString &text) const { writeFile(path(relative), text.toUtf8()); }
    QByteArray read(const QString &relative) const { return readFile(path(relative)); }

    // Runs a command in the test process, as main() would.
    Ran run(QStringList arguments) const
    {
        QBuffer out, err;
        out.open(QIODevice::WriteOnly);
        err.open(QIODevice::WriteOnly);
        Cli::Context context{m_root, home(), paths(), &out, &err};
        context.renderCache = path(QStringLiteral("app/cache/render"));
        arguments.prepend(QStringLiteral("parch"));
        // No math or diagram helpers, unless a test names some: what an
        // export writes does not hang on what is installed, and nothing is
        // drawn into the cache the application keeps.
        const QByteArray helpers = qgetenv("PARCH_HELPERS");
        if (helpers.isNull()) qputenv("PARCH_HELPERS", QFile::encodeName(m_root + QStringLiteral("/no-helpers")));
        const auto restore = qScopeGuard([&] {
            if (helpers.isNull()) qunsetenv("PARCH_HELPERS");
        });
        Ran ran;
        ran.code = Cli::run(arguments, context);
        ran.out = QString::fromUtf8(out.data());
        ran.err = QString::fromUtf8(err.data());
        return ran;
    }

private:
    QTemporaryDir m_dir;
    QString m_root;
};

const QStringList commands{QStringLiteral("check"), QStringLiteral("render"), QStringLiteral("export"),
                           QStringLiteral("stats"), QStringLiteral("skill")};

// What is wrong with a JSON result's common fields, or nothing.
QString headerProblem(const QJsonObject &result, const QString &command)
{
    if (result.value(QStringLiteral("parch")).toString() != QStringLiteral(PARCH_VERSION))
        return QStringLiteral("no parch version");
    if (result.value(QStringLiteral("command")).toString() != command) return QStringLiteral("no command");
    if (command == u"skill") return {};
    const QJsonObject renderers = result.value(QStringLiteral("renderers")).toObject();
    if (renderers.value(QStringLiteral("markdown")).toString() != QStringLiteral("parch " PARCH_VERSION))
        return QStringLiteral("no Markdown renderer version");
    if (!result.value(QStringLiteral("helpers")).isObject()) return QStringLiteral("no helpers");
    return {};
}

QString dialectProblem(const QJsonObject &dialect)
{
    static const QSet<QString> layers{QStringLiteral("flag"), QStringLiteral("document"), QStringLiteral("library"),
                                      QStringLiteral("default")};
    if (dialect.value(QStringLiteral("version")).toInt() != md::DialectVersion) return QStringLiteral("version");
    const QString profile = dialect.value(QStringLiteral("profile")).toString();
    if (profile != u"writer" && profile != u"standard") return QStringLiteral("profile");
    if (!dialect.value(QStringLiteral("extensions")).isArray()) return QStringLiteral("extensions");
    for (const QJsonValue &name : dialect.value(QStringLiteral("extensions")).toArray())
        if (!md::Options::extensionNames().contains(name.toString())) return QStringLiteral("extension ") + name.toString();
    const QJsonValue root = dialect.value(QStringLiteral("libraryRoot"));
    if (!root.isNull() && !root.isString()) return QStringLiteral("libraryRoot");
    if (!dialect.value(QStringLiteral("savedSettings")).isBool()) return QStringLiteral("savedSettings");
    const QJsonObject from = dialect.value(QStringLiteral("from")).toObject();
    if (!layers.contains(from.value(QStringLiteral("profile")).toString())) return QStringLiteral("from.profile");
    if (!layers.contains(from.value(QStringLiteral("libraryRoot")).toString())) return QStringLiteral("from.libraryRoot");
    const QJsonObject extensions = from.value(QStringLiteral("extensions")).toObject();
    for (auto it = extensions.begin(); it != extensions.end(); ++it)
        if (!md::Options::extensionNames().contains(it.key()) || !layers.contains(it.value().toString()))
            return QStringLiteral("from.extensions");
    return {};
}

QString problemProblem(const QJsonObject &problem)
{
    static const QSet<QString> kinds{QStringLiteral("include"),  QStringLiteral("reference"), QStringLiteral("link"),
                                     QStringLiteral("asset"),    QStringLiteral("footnote"),  QStringLiteral("citation"),
                                     QStringLiteral("variable"), QStringLiteral("table"),     QStringLiteral("math"),
                                     QStringLiteral("front-matter"), QStringLiteral("html")};
    static const QSet<QString> severities{QStringLiteral("error"), QStringLiteral("warning"), QStringLiteral("info")};
    if (!problem.value(QStringLiteral("file")).isString()) return QStringLiteral("file");
    if (problem.value(QStringLiteral("line")).toInt() < 1) return QStringLiteral("line");
    if (problem.value(QStringLiteral("column")).toInt() < 1) return QStringLiteral("column");
    const QJsonArray range = problem.value(QStringLiteral("range")).toArray();
    if (range.size() != 2 || range[0].toInt() > range[1].toInt()) return QStringLiteral("range");
    if (!severities.contains(problem.value(QStringLiteral("severity")).toString())) return QStringLiteral("severity");
    if (problem.value(QStringLiteral("code")).toString().isEmpty()) return QStringLiteral("code");
    if (!kinds.contains(problem.value(QStringLiteral("kind")).toString())) return QStringLiteral("kind");
    if (problem.value(QStringLiteral("message")).toString().isEmpty()) return QStringLiteral("message");
    return {};
}

QString nodeProblem(const QJsonObject &node, int files)
{
    const QString type = node.value(QStringLiteral("type")).toString();
    if (type.isEmpty() || type == u"unknown") return QStringLiteral("type");
    const int file = node.value(QStringLiteral("file")).toInt(-1);
    if (file < 0 || file >= files) return type + QStringLiteral(": file");
    const QJsonArray range = node.value(QStringLiteral("range")).toArray();
    if (range.size() != 2 || range[0].toInt() > range[1].toInt()) return type + QStringLiteral(": range");
    if (node.contains(QStringLiteral("line")) && node.value(QStringLiteral("line")).toInt() < 1)
        return type + QStringLiteral(": line");
    for (const QJsonValue &child : node.value(QStringLiteral("children")).toArray())
        if (const QString problem = nodeProblem(child.toObject(), files); !problem.isEmpty()) return problem;
    return {};
}

void collectTypes(const QJsonObject &node, QSet<QString> &types)
{
    types.insert(node.value(QStringLiteral("type")).toString());
    for (const QJsonValue &child : node.value(QStringLiteral("children")).toArray()) collectTypes(child.toObject(), types);
}

QSet<QString> treeTypes(const Sandbox &box, QStringList arguments)
{
    const Ran ran = box.run(QStringList{QStringLiteral("render")} + arguments + QStringList{QStringLiteral("--to"), QStringLiteral("json-tree")});
    QSet<QString> types;
    collectTypes(ran.json().value(QStringLiteral("root")).toObject(), types);
    return types;
}

QStringList codesOf(const QJsonObject &document)
{
    QStringList codes;
    for (const QJsonValue &problem : document.value(QStringLiteral("problems")).toArray())
        codes.append(problem.toObject().value(QStringLiteral("code")).toString());
    return codes;
}

QJsonObject problemCoded(const QJsonObject &document, const QString &code)
{
    for (const QJsonValue &problem : document.value(QStringLiteral("problems")).toArray())
        if (problem.toObject().value(QStringLiteral("code")).toString() == code) return problem.toObject();
    return {};
}

// A document read as the commands read it.
DocumentCheck::Loaded loadedFrom(const QString &path, const Dialect::Choice &choice = Dialect::choose({}))
{
    DocumentCheck::Loaded loaded;
    QString error;
    if (!DocumentCheck::load(path, choice, &loaded, &error)) qWarning() << error;
    return loaded;
}

// Every file beneath a folder, with its bytes.
QMap<QString, QByteArray> filesBeneath(const QString &folder)
{
    QMap<QString, QByteArray> found;
    QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString file = it.next();
        found.insert(QDir(folder).relativeFilePath(file), readFile(file));
    }
    return found;
}

// The book the export tests assemble: a document with two chapters, each
// with definitions, notes and variables of its own.
void writeBook(const Sandbox &box)
{
    box.write(QStringLiteral("book/main.md"),
              QStringLiteral("---\ntitle: Book\nwho: Root\n---\n\n"
                             "# Book\n\n"
                             "Hello [%who]. See [site][1] and [home][].\n"
                             "Note[^1] and a citation[p. 2][#Doe].\n\n"
                             "![cover](images/cover.png)\n\n"
                             "chapters/one.md \"One\"\n\n"
                             "chapters/two.md\n\n"
                             "[^1]: Root note.\n\n"
                             "[1]: https://example.org/root\n"
                             "[home]: https://example.org/home\n\n"
                             "[#Doe]: Jane Doe. *A Book*.\n"));
    box.write(QStringLiteral("book/chapters/one.md"),
              QStringLiteral("---\nwho: Chapter\n---\n"
                             "## One\n\n"
                             "Hi [%who] of [%title]. A [link][1], [home][], [home] and [Book][]. Note[^1].\n"
                             "![fig](fig.png) <img src=\"fig.png\" alt=\"f\"> [up](../main.md#book)\n\n"
                             "[^1]: One's note.\n\n"
                             "[1]: https://example.org/one\n"
                             "[home]: https://example.org/one-home\n"));
    box.write(QStringLiteral("book/chapters/two.md"), QStringLiteral("## Two\n\nLast[^1].\n\n```\nunclosed fence"));
    QDir().mkpath(box.path(QStringLiteral("book/images")));
    QImage picture(2, 2, QImage::Format_RGB32);
    picture.fill(Qt::green);
    QVERIFY(picture.save(box.path(QStringLiteral("book/images/cover.png"))));
    picture.fill(Qt::blue);
    QVERIFY(picture.save(box.path(QStringLiteral("book/chapters/fig.png"))));
}

// What a reader takes from a document: its text, headings, notes and where
// each link and image leads.
struct Meaning {
    QString text;
    QStringList headings;
    QStringList notes;
    QStringList destinations;
    bool operator==(const Meaning &) const = default;
};

void destinationsOf(const md::Document &document, const md::Node &node, QStringList &out)
{
    if (!node.generated && (node.type == md::NodeType::Link || node.type == md::NodeType::Image)) {
        const md::Destination target = md::classifyDestination(document, node);
        out.append(target.kind == md::Destination::Kind::LocalFile ? target.path + u'#' + target.fragment : node.dest);
    }
    if (node.type == md::NodeType::HtmlElement)
        for (const md::Attribute &attribute : node.attributes)
            if (attribute.name == u"src" || attribute.name == u"href")
                out.append(md::classifyDestination(attribute.value, document.files[size_t(node.file)].directory).path);
    for (const md::Node &child : node.children) destinationsOf(document, child, out);
}

Meaning meaningOf(const md::Document &document)
{
    Meaning meaning;
    meaning.text = document.plainText();
    for (const md::Heading &heading : document.headings)
        meaning.headings.append(QStringLiteral("%1 %2 #%3").arg(heading.level).arg(heading.text, heading.slug));
    for (const md::Footnote &note : document.footnotes)
        meaning.notes.append(QStringLiteral("%1: %2").arg(note.number).arg(md::plainText(*note.content).simplified()));
    destinationsOf(document, document.root, meaning.destinations);
    return meaning;
}

// parch run as a process, with nothing of a desktop to reach.
const QString parchBinary = QStringLiteral(PARCH_SOURCE_DIR "/build/parch");

Ran runProcess(const Sandbox &box, const QStringList &arguments)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const char *name : {"HYPRLAND_INSTANCE_SIGNATURE", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "DISPLAY",
                             "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID", "QT_QPA_PLATFORMTHEME"})
        environment.remove(QString::fromLatin1(name));
    const QList<std::pair<const char *, const char *>> folders{
        {"HOME", "home"},           {"XDG_CONFIG_HOME", "app/config"}, {"XDG_DATA_HOME", "app/data"},
        {"XDG_STATE_HOME", "app/state"}, {"XDG_CACHE_HOME", "app/cache"},   {"XDG_RUNTIME_DIR", "app/runtime"},
        {"TMPDIR", "app/tmp"}};
    for (const auto &[name, folder] : folders) {
        QDir().mkpath(box.path(QString::fromLatin1(folder)));
        environment.insert(QString::fromLatin1(name), box.path(QString::fromLatin1(folder)));
    }
    QFile::setPermissions(box.path(QStringLiteral("app/runtime")), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    environment.insert(QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("unix:path=") + box.path(QStringLiteral("no-bus")));
    // A GUI application could not start with this; the commands make none,
    // and an export makes one on Qt's offscreen platform.
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("no-such-platform"));
    environment.insert(QStringLiteral("QT_QPA_PLATFORMTHEME"), QStringLiteral("gtk3"));
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setWorkingDirectory(box.root());
    process.start(parchBinary, arguments);
    Ran ran;
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished();
        ran.err = QStringLiteral("did not finish");
        return ran;
    }
    ran.code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -2;
    ran.out = QString::fromUtf8(process.readAllStandardOutput());
    ran.err = QString::fromUtf8(process.readAllStandardError());
    return ran;
}

} // namespace

void launchFindsCommands()
{
    for (const QString &command : commands + QStringList{QStringLiteral("help")}) {
        const Launch launch = parseLaunch({QStringLiteral("parch"), command, QStringLiteral("a.md")});
        QCOMPARE(launch.kind, Launch::Command);
        QCOMPARE(launch.commandArguments, (QStringList{QStringLiteral("parch"), command, QStringLiteral("a.md")}));
        QVERIFY(Cli::isCommand(command));
    }
    QVERIFY(!Cli::isCommand(QStringLiteral("open")));
    QVERIFY(!Cli::isCommand(QStringLiteral("notes.md")));
    // A file named like a command still opens, after "open" or "--", or after another file.
    QCOMPARE(parseLaunch({"parch", "open", "check"}).kind, Launch::Gui);
    QCOMPARE(parseLaunch({"parch", "open", "check"}).files, QStringList{"check"});
    QCOMPARE(parseLaunch({"parch", "--", "check"}).files, QStringList{"check"});
    QCOMPARE(parseLaunch({"parch", "a.md", "check"}).files, (QStringList{"a.md", "check"}));
    QCOMPARE(parseLaunch({"parch", "--help", "check"}).kind, Launch::Help);
    QCOMPARE(parseLaunch({"parch", "check", "--help"}).kind, Launch::Command);
    // Qt's own options stay Qt's, wherever they are.
    QCOMPARE(parseLaunch({"parch", "-platform", "offscreen", "check", "a.md"}).commandArguments,
             (QStringList{"parch", "check", "a.md"}));
    // Opening is as it was.
    QCOMPARE(parseLaunch({"parch", "open", "a.md", "b.md"}).files, (QStringList{"a.md", "b.md"}));
    QCOMPARE(parseLaunch({"parch", "notes.md"}).kind, Launch::Gui);
}

void helpExplainsEveryCommand()
{
    Sandbox box;
    QVERIFY(box.isValid());
    const QString help = helpText();
    QVERIFY(help.startsWith(QStringLiteral("Usage: parch [open] [--theme DIR] [--benchmark OUT] [FILE...]")));
    for (const char *line : {"  check FILE...", "  render FILE", "  export FILE", "  stats FILE", "  skill [install]",
                             "  help [COMMAND]", "  --theme DIR", "  --benchmark OUT"})
        QVERIFY2(help.contains(QLatin1StringView(line)), line);
    QVERIFY(help.contains(openHelpText().section(u'\n', 2)));

    const Ran all = box.run({QStringLiteral("help")});
    QCOMPARE(all.code, 0);
    QCOMPARE(all.out, help);
    const Ran open = box.run({QStringLiteral("help"), QStringLiteral("open")});
    QCOMPARE(open.code, 0);
    QCOMPARE(open.out, openHelpText());

    for (const QString &command : commands) {
        const Ran asked = box.run({command, QStringLiteral("--help")});
        QCOMPARE(asked.code, 0);
        QVERIFY2(asked.out.startsWith(QStringLiteral("Usage: parch ") + command + u' '), qPrintable(asked.out));
        QVERIFY2(asked.out.contains(QStringLiteral("--json")), qPrintable(command));
        QVERIFY2(asked.out.contains(QStringLiteral("Exit status:")), qPrintable(command));
        QCOMPARE(box.run({QStringLiteral("help"), command}).out, asked.out);
        QCOMPARE(box.run({command, QStringLiteral("-h")}).out, asked.out);
        if (command != u"skill")
            for (const char *option : {"--profile NAME", "--extension NAME", "--no-extension NAME", "--library DIR",
                                       "--use-saved-settings"})
                QVERIFY2(asked.out.contains(QLatin1StringView(option)), qPrintable(command + u' ' + QLatin1StringView(option)));
    }
    const Ran exportHelp = box.run({QStringLiteral("help"), QStringLiteral("export")});
    QVERIFY(exportHelp.out.contains(QStringLiteral("md, html, pdf, docx, epub")));
    QVERIFY(exportHelp.out.contains(QStringLiteral("--title TEXT")));
    QVERIFY(exportHelp.out.contains(QStringLiteral("--force")));
    QVERIFY(!exportHelp.out.contains(QStringLiteral("not available yet")));
    for (const QString &name : md::Options::extensionNames())
        QVERIFY2(box.run({QStringLiteral("help"), QStringLiteral("check")}).out.contains(name), qPrintable(name));

    // The guide shows the help as it is.
    const QString guide = QString::fromUtf8(readFile(QStringLiteral(PARCH_SOURCE_DIR "/docs/guide.md")));
    const qsizetype section = guide.indexOf(QStringLiteral("## Command line\n\n```text\n"));
    QVERIFY(section >= 0);
    const qsizetype start = guide.indexOf(u'\n', guide.indexOf(QStringLiteral("```text"), section)) + 1;
    QCOMPARE(guide.mid(start, guide.indexOf(QStringLiteral("```\n"), start) - start), help);

    const Ran unknown = box.run({QStringLiteral("help"), QStringLiteral("frobnicate")});
    QCOMPARE(unknown.code, 1);
    QVERIFY(unknown.err.contains(QStringLiteral("frobnicate")));
}

void usageErrorsExitOne()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("a.md"), QStringLiteral("# A\n"));
    const QList<QStringList> wrong{
        {"check"},
        {"check", "a.md", "--profile", "fancy"},
        {"check", "a.md", "--extension", "sparkles"},
        {"check", "a.md", "--extension", "math", "--no-extension", "math"},
        {"check", "a.md", "--bogus"},
        {"check", "a.md", "--library", "no-such-folder"},
        {"render"},
        {"render", "a.md", "b.md"},
        {"render", "a.md", "--to", "pdf"},
        {"export", "a.md"},
        {"export", "a.md", "--to", "rtf", "-o", "a.rtf"},
        {"export", "a.md", "-o", "a.unknown"},
        {"export", "a.md", "--to", "md", "-o", "out.md", "--title", "Not for Markdown"},
        {"export"},
        {"stats"},
        {"stats", "a.md", "b.md"},
        {"skill", "uninstall"},
    };
    for (const QStringList &arguments : wrong) {
        const Ran ran = box.run(arguments);
        QVERIFY2(ran.code == 1, qPrintable(arguments.join(u' ') + u'\n' + ran.said()));
        QVERIFY2(ran.err.startsWith(QStringLiteral("parch ") + arguments.first() + u':'), qPrintable(ran.said()));
        QVERIFY2(ran.err.contains(QStringLiteral("parch help ") + arguments.first()), qPrintable(ran.said()));
        QVERIFY(ran.out.isEmpty());
    }
    QVERIFY(box.run({"check", "a.md", "--profile", "fancy"}).err.contains(QStringLiteral("writer or standard")));
    QVERIFY(box.run({"check", "a.md", "--extension", "sparkles"}).err.contains(QStringLiteral("footnotes")));
    QVERIFY(box.run({"export", "a.md", "--to", "md", "-o", "o.md", "--title", "x"})
                .err.contains(QStringLiteral("--title is an option of HTML, PDF, DOCX and EPUB export, not Markdown")));
    // Nothing was written by a refused export.
    QVERIFY(!QFileInfo::exists(box.path(QStringLiteral("out.md"))));
}

void checkReportsEveryKindOfProblem()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("main.md"),
              QStringLiteral("---\nkey: !tag value\n---\n\n"
                             "# Twin\n\n# Twin\n\n"
                             "[a][Missing] [b][Twin] [c](#nowhere) [^nonote] [%novar] [][#nokey] <blink>x</blink>\n\n"
                             "| a | b |\n| - | - |\n| 1 || 2 | 3 |\n\n"
                             "![gone](pictures/gone.png) ![here](pictures/here.png) [file](nothere.md) "
                             "[heading](other.md#nope) [fine](other.md#there)\n\n"
                             "<a href=\"gone.html\">a</a> <img src=\"pictures/gone2.png\" alt=\"b\">\n\n"
                             "$$\nunclosed\n\n"
                             "missing.md\n\n"
                             "loop.md\n\n"
                             "/rooted.md\n"));
    box.write(QStringLiteral("loop.md"), QStringLiteral("loop.md\n"));
    box.write(QStringLiteral("other.md"), QStringLiteral("# There\n"));
    box.write(QStringLiteral("pictures/here.png"), QStringLiteral("png"));

    const Ran ran = box.run({QStringLiteral("check"), QStringLiteral("main.md")});
    QVERIFY2(ran.code == 2, qPrintable(ran.said()));
    const QStringList lines = ran.out.split(u'\n', Qt::SkipEmptyParts);
    // file:line:column: severity: message [code], one a line, in order.
    QVERIFY2(lines.contains(QStringLiteral("main.md:9:1: warning: Nothing is defined for the reference \"Missing\" "
                                           "[unresolved-reference]")),
             qPrintable(ran.out));
    static const QRegularExpression shape(QStringLiteral("^(main|loop)\\.md:\\d+:\\d+: (error|warning|info): .+ \\[[a-z-]+\\]$"));
    for (const QString &line : lines.mid(0, lines.size() - 1))
        QVERIFY2(shape.match(line).hasMatch(), qPrintable(line));
    QCOMPARE(lines.last(), QStringLiteral("main.md: 3 errors, 14 warnings, 2 notes (writer profile)"));

    const Ran asJson = box.run({QStringLiteral("check"), QStringLiteral("main.md"), QStringLiteral("--json")});
    QCOMPARE(asJson.code, 2);
    QVERIFY(asJson.err.isEmpty());
    const QJsonObject result = asJson.json();
    QCOMPARE(headerProblem(result, QStringLiteral("check")), QString());
    QCOMPARE(result.value(QStringLiteral("ok")).toBool(true), false);
    const QJsonArray documents = result.value(QStringLiteral("documents")).toArray();
    QCOMPARE(documents.size(), 1);
    const QJsonObject document = documents.first().toObject();
    QCOMPARE(document.value(QStringLiteral("file")).toString(), box.path(QStringLiteral("main.md")));
    QCOMPARE(dialectProblem(document.value(QStringLiteral("dialect")).toObject()), QString());
    QCOMPARE(document.value(QStringLiteral("incomplete")).toBool(), true);
    QCOMPARE(document.value(QStringLiteral("files")).toArray().size(), 2);
    for (const QJsonValue &problem : document.value(QStringLiteral("problems")).toArray())
        QCOMPARE(problemProblem(problem.toObject()), QString());

    // Every kind, with its code and severity.
    const QList<std::tuple<const char *, const char *, const char *>> expected{
        {"front-matter", "front-matter", "warning"},
        {"unresolved-reference", "reference", "warning"},
        {"ambiguous-heading-reference", "reference", "warning"},
        {"unresolved-anchor", "link", "warning"},
        {"missing-footnote", "footnote", "warning"},
        {"unresolved-variable", "variable", "warning"},
        {"missing-citation", "citation", "warning"},
        {"unsupported-html", "html", "info"},
        {"malformed-table-row", "table", "warning"},
        {"unresolved-asset", "asset", "warning"},
        {"broken-link", "link", "warning"},
        {"broken-link-anchor", "link", "warning"},
        {"unclosed-math", "math", "warning"},
        {"include-not-found", "include", "error"},
        {"include-no-library", "include", "error"},
        {"include-cycle", "include", "error"},
    };
    const QStringList codes = codesOf(document);
    for (const auto &[code, kind, severity] : expected) {
        const QJsonObject problem = problemCoded(document, QLatin1StringView(code));
        QVERIFY2(!problem.isEmpty(), code);
        QCOMPARE(problem.value(QStringLiteral("kind")).toString(), QLatin1StringView(kind));
        QCOMPARE(problem.value(QStringLiteral("severity")).toString(), QLatin1StringView(severity));
    }
    // Both images and both links, in Markdown and in HTML; the link to a
    // heading that is there is fine.
    QCOMPARE(codes.count(QStringLiteral("unresolved-asset")), 2);
    QCOMPARE(codes.count(QStringLiteral("broken-link")), 2);
    QCOMPARE(codes.count(QStringLiteral("broken-link-anchor")), 1);
    // Lines and columns, from 1, in characters.
    const QJsonObject twin = problemCoded(document, QStringLiteral("ambiguous-heading-reference"));
    QCOMPARE(twin.value(QStringLiteral("line")).toInt(), 9);
    QCOMPARE(twin.value(QStringLiteral("column")).toInt(), 14);
    const QJsonObject asset = problemCoded(document, QStringLiteral("unresolved-asset"));
    QCOMPARE(asset.value(QStringLiteral("line")).toInt(), 15);
    QCOMPARE(asset.value(QStringLiteral("column")).toInt(), 1);
    QVERIFY(asset.value(QStringLiteral("message")).toString().contains(QStringLiteral("pictures/gone.png")));
    // A problem of an included file names that file.
    const QJsonObject cycle = problemCoded(document, QStringLiteral("include-cycle"));
    QCOMPARE(cycle.value(QStringLiteral("file")).toString(), box.path(QStringLiteral("loop.md")));
    QCOMPARE(cycle.value(QStringLiteral("line")).toInt(), 1);
    const QJsonObject counts = document.value(QStringLiteral("counts")).toObject();
    QCOMPARE(counts.value(QStringLiteral("error")).toInt(), 3);
    QCOMPARE(counts.value(QStringLiteral("warning")).toInt(), 14);
    QCOMPARE(counts.value(QStringLiteral("info")).toInt(), 2);
}

void checkPassesACleanDocument()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("clean.md"),
              QStringLiteral("# Title\n\nSome *text* with a [link](#title), a [file](other.md#part) and "
                             "![a picture](pic.png).\n\nchapter.md\n"));
    box.write(QStringLiteral("other.md"), QStringLiteral("## Part\n"));
    box.write(QStringLiteral("chapter.md"), QStringLiteral("More[^1].\n\n[^1]: A note.\n"));
    box.write(QStringLiteral("pic.png"), QStringLiteral("png"));
    const Ran ran = box.run({QStringLiteral("check"), QStringLiteral("clean.md")});
    QVERIFY2(ran.code == 0, qPrintable(ran.said()));
    QCOMPARE(ran.out, QStringLiteral("clean.md: nothing wrong (writer profile)\n"));
    const Ran asJson = box.run({QStringLiteral("check"), QStringLiteral("clean.md"), QStringLiteral("--json")});
    QCOMPARE(asJson.code, 0);
    QCOMPARE(asJson.json().value(QStringLiteral("ok")).toBool(), true);
    const QJsonObject document = asJson.json().value(QStringLiteral("documents")).toArray().first().toObject();
    QCOMPARE(document.value(QStringLiteral("ok")).toBool(), true);
    QVERIFY(document.value(QStringLiteral("problems")).toArray().isEmpty());

    // Notes of severity info alone are not problems.
    box.write(QStringLiteral("notes.md"), QStringLiteral("Some <span>styled</span> text.\n"));
    const Ran notes = box.run({QStringLiteral("check"), QStringLiteral("notes.md")});
    QVERIFY2(notes.code == 0, qPrintable(notes.said()));
    QVERIFY(notes.out.contains(QStringLiteral("notes.md: 2 notes")));
    QVERIFY(notes.out.contains(QStringLiteral(": info: ")));

    // Several files: each is reported, and one with problems makes it 2.
    box.write(QStringLiteral("bad.md"), QStringLiteral("A [ref][nothing].\n"));
    const Ran several = box.run({QStringLiteral("check"), QStringLiteral("clean.md"), QStringLiteral("bad.md")});
    QCOMPARE(several.code, 2);
    QVERIFY(several.out.contains(QStringLiteral("clean.md: nothing wrong")));
    QVERIFY(several.out.contains(QStringLiteral("bad.md: 1 warning")));
    QCOMPARE(box.run({"check", "clean.md", "bad.md", "--json"}).json().value(QStringLiteral("documents")).toArray().size(), 2);
}

void checkSaysWhatCannotBeRead()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("bad.md"), QStringLiteral("A [ref][nothing].\n"));
    writeFile(box.path(QStringLiteral("latin1.md")), QByteArray("caf\xe9\n"));
    QDir().mkpath(box.path(QStringLiteral("folder")));

    const Ran missing = box.run({QStringLiteral("check"), QStringLiteral("missing.md")});
    QCOMPARE(missing.code, 3);
    QVERIFY2(missing.err.contains(box.path(QStringLiteral("missing.md")) + QStringLiteral(" does not exist")),
             qPrintable(missing.said()));
    QCOMPARE(box.run({QStringLiteral("check"), QStringLiteral("folder")}).code, 3);
    QVERIFY(box.run({QStringLiteral("check"), QStringLiteral("folder")}).err.contains(QStringLiteral("is a folder")));
    const Ran latin1 = box.run({QStringLiteral("check"), QStringLiteral("latin1.md")});
    QCOMPARE(latin1.code, 3);
    QVERIFY(latin1.err.contains(QStringLiteral("UTF-8")));

    writeFile(box.path("truncated.md"), QByteArray("ending \xe2\x82"));
    box.write("including.md", "truncated.md\n");
    QCOMPARE(box.run({"check", "truncated.md"}).code, Cli::Io);
    const Ran incomplete = box.run({"check", "including.md"});
    QCOMPARE(incomplete.code, Cli::Problems);
    QVERIFY2(incomplete.said().contains("UTF-8"), qPrintable(incomplete.said()));
    QCOMPARE(box.run({"export", "including.md", "-o", "result.html"}).code, Cli::Problems);
    QVERIFY(!QFileInfo::exists(box.path("result.html")));

    // Unreadable wins over problems found; the JSON says which is which.
    const Ran both = box.run({"check", "bad.md", "missing.md", "--json"});
    QCOMPARE(both.code, 3);
    const QJsonArray documents = both.json().value(QStringLiteral("documents")).toArray();
    QCOMPARE(documents.size(), 2);
    QCOMPARE(documents[0].toObject().value(QStringLiteral("problems")).toArray().size(), 1);
    QCOMPARE(documents[1].toObject().value(QStringLiteral("ok")).toBool(true), false);
    QVERIFY(documents[1].toObject().value(QStringLiteral("error")).toString().contains(QStringLiteral("does not exist")));
    QCOMPARE(box.run({"render", "missing.md"}).code, 3);
    QCOMPARE(box.run({"stats", "missing.md"}).code, 3);
    QCOMPARE(box.run({"export", "missing.md", "-o", "x.html"}).code, 3);
}

void profileAndExtensionsFollowTheFlags()
{
    // The layers, highest first: flags, the document's, the library's, the defaults.
    Dialect::Layer library, document, flags;
    QCOMPARE(Dialect::choose({}).options, md::Options::writer());
    QCOMPARE(Dialect::choose({}).profileFrom, QStringLiteral("default"));
    library.profile = md::Profile::Standard;
    library.extensions = {{QStringLiteral("math"), true}};
    library.libraryRoot = QStringLiteral("/library");
    QCOMPARE(Dialect::choose({}, {}, library).options.profile, md::Profile::Standard);
    QCOMPARE(Dialect::choose({}, {}, library).extensionsFrom.value(QStringLiteral("math")), QStringLiteral("library"));
    QCOMPARE(Dialect::choose({}, {}, library).libraryRootFrom, QStringLiteral("library"));
    document.profile = md::Profile::Writer;
    document.extensions = {{QStringLiteral("citations"), false}};
    Dialect::Choice chosen = Dialect::choose({}, document, library);
    QCOMPARE(chosen.options.profile, md::Profile::Writer);
    QCOMPARE(chosen.profileFrom, QStringLiteral("document"));
    QVERIFY(!chosen.options.citations);
    QVERIFY(chosen.options.math);
    QVERIFY(!chosen.extensionsFrom.contains(QStringLiteral("math"))); // Writer's own
    QCOMPARE(chosen.extensionsFrom.value(QStringLiteral("citations")), QStringLiteral("document"));
    flags.extensions = {{QStringLiteral("citations"), true}, {QStringLiteral("toc"), false}};
    flags.libraryRoot = QStringLiteral("/flagged");
    chosen = Dialect::choose(flags, document, library);
    QVERIFY(chosen.options.citations);
    QVERIFY(!chosen.extensionsFrom.contains(QStringLiteral("citations")));
    QCOMPARE(chosen.extensionsFrom.value(QStringLiteral("toc")), QStringLiteral("flag"));
    QCOMPARE(chosen.libraryRoot, QStringLiteral("/flagged"));
    QCOMPARE(chosen.libraryRootFrom, QStringLiteral("flag"));
    QCOMPARE(chosen.describe(), QStringLiteral("writer profile, without toc, library /flagged"));
    QCOMPARE(dialectProblem(chosen.toJson()), QString());

    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("doc.md"), QStringLiteral("Text[^1] ==mark== $x$\n\n[^1]: Note.\n"));
    const QSet<QString> writer = treeTypes(box, {QStringLiteral("doc.md")});
    for (const char *type : {"footnote-reference", "highlight", "math-inline"})
        QVERIFY2(writer.contains(QLatin1StringView(type)), type);
    const QSet<QString> standard = treeTypes(box, {"doc.md", "--profile", "standard"});
    for (const char *type : {"footnote-reference", "highlight", "math-inline"})
        QVERIFY2(!standard.contains(QLatin1StringView(type)), type);
    const QSet<QString> notes = treeTypes(box, {"doc.md", "--profile", "standard", "--extension", "footnotes"});
    QVERIFY(notes.contains(QStringLiteral("footnote-reference")));
    QVERIFY(!notes.contains(QStringLiteral("highlight")));
    const QSet<QString> noMath = treeTypes(box, {"doc.md", "--no-extension", "math"});
    QVERIFY(!noMath.contains(QStringLiteral("math-inline")));
    QVERIFY(noMath.contains(QStringLiteral("highlight")));

    // The JSON says which, and where it came from.
    const QJsonObject dialect = box.run({"stats", "doc.md", "--json", "--profile", "standard", "--extension",
                                         "footnotes"})
                                    .json()
                                    .value(QStringLiteral("dialect"))
                                    .toObject();
    QCOMPARE(dialectProblem(dialect), QString());
    QCOMPARE(dialect.value(QStringLiteral("profile")).toString(), QStringLiteral("standard"));
    QVERIFY(dialect.value(QStringLiteral("extensions")).toArray().contains(QStringLiteral("footnotes")));
    QVERIFY(!dialect.value(QStringLiteral("extensions")).toArray().contains(QStringLiteral("math")));
    const QJsonObject from = dialect.value(QStringLiteral("from")).toObject();
    QCOMPARE(from.value(QStringLiteral("profile")).toString(), QStringLiteral("flag"));
    QCOMPARE(from.value(QStringLiteral("extensions")).toObject().value(QStringLiteral("footnotes")).toString(),
             QStringLiteral("flag"));
    const QJsonObject plain = box.run({"stats", "doc.md", "--json"}).json().value(QStringLiteral("dialect")).toObject();
    QCOMPARE(plain.value(QStringLiteral("profile")).toString(), QStringLiteral("writer"));
    QCOMPARE(plain.value(QStringLiteral("from")).toObject().value(QStringLiteral("profile")).toString(),
             QStringLiteral("default"));
    QCOMPARE(plain.value(QStringLiteral("extensions")).toArray().size(), md::Options::extensionNames().size());
    QCOMPARE(plain.value(QStringLiteral("savedSettings")).toBool(true), false);
    QVERIFY(box.run({"check", "doc.md", "--profile", "STANDARD"}).out.contains(QStringLiteral("(standard profile)")));
}

void savedSettingsApplyOnlyWhenAsked()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("library/doc.md"), QStringLiteral("Text[^1] $x$\n\n[^1]: Note.\n"));
    box.write(QStringLiteral("library/other.md"), QStringLiteral("Some $x$.\n\n/chapters/one.md\n"));
    box.write(QStringLiteral("library/chapters/one.md"), QStringLiteral("One.\n"));
    box.write(QStringLiteral("loose.md"), QStringLiteral("Loose $x$.\n"));
    const QString library = box.path(QStringLiteral("library"));
    const AppPaths paths = box.paths();
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    writeFile(paths.registryFile(),
              QJsonDocument(QJsonObject{{box.path(QStringLiteral("library/doc.md")), id}}).toJson());
    writeFile(paths.settingsFile(),
              QJsonDocument(QJsonObject{{QStringLiteral("libraryFolders"), QJsonArray{library}},
                                        {QStringLiteral("profile"), QStringLiteral("standard")}})
                  .toJson());
    writeFile(paths.documentSettingsFile(),
              QJsonDocument(
                  QJsonObject{
                      {QStringLiteral("documents"),
                       QJsonObject{{id, QJsonObject{{QStringLiteral("profile"), QStringLiteral("standard")},
                                                    {QStringLiteral("extensions"),
                                                     QJsonObject{{QStringLiteral("footnotes"), true}}}}}}},
                      {QStringLiteral("libraries"),
                       QJsonObject{{library, QJsonObject{{QStringLiteral("extensions"),
                                                          QJsonObject{{QStringLiteral("math"), false}}}}}}}})
                  .toJson());

    const auto dialectOf = [&box](const QStringList &arguments) {
        return box.run(QStringList{QStringLiteral("stats")} + arguments + QStringList{QStringLiteral("--json")})
            .json()
            .value(QStringLiteral("dialect"))
            .toObject();
    };
    // Unasked, nothing saved is read: the stable defaults.
    QJsonObject dialect = dialectOf({QStringLiteral("library/doc.md")});
    QCOMPARE(dialect.value(QStringLiteral("profile")).toString(), QStringLiteral("writer"));
    QCOMPARE(dialect.value(QStringLiteral("savedSettings")).toBool(true), false);
    QVERIFY(dialect.value(QStringLiteral("libraryRoot")).isNull());
    // Asked, the document's own settings, over its library's.
    dialect = dialectOf({"library/doc.md", "--use-saved-settings"});
    QCOMPARE(dialectProblem(dialect), QString());
    QCOMPARE(dialect.value(QStringLiteral("profile")).toString(), QStringLiteral("standard"));
    QCOMPARE(dialect.value(QStringLiteral("from")).toObject().value(QStringLiteral("profile")).toString(),
             QStringLiteral("document"));
    QCOMPARE(dialect.value(QStringLiteral("savedSettings")).toBool(), true);
    QVERIFY(dialect.value(QStringLiteral("extensions")).toArray().contains(QStringLiteral("footnotes")));
    QCOMPARE(dialect.value(QStringLiteral("libraryRoot")).toString(), library);
    QCOMPARE(dialect.value(QStringLiteral("from")).toObject().value(QStringLiteral("libraryRoot")).toString(),
             QStringLiteral("library"));
    // A document with no settings of its own takes its library's.
    dialect = dialectOf({"library/other.md", "--use-saved-settings"});
    QCOMPARE(dialect.value(QStringLiteral("profile")).toString(), QStringLiteral("writer"));
    QVERIFY(!dialect.value(QStringLiteral("extensions")).toArray().contains(QStringLiteral("math")));
    QCOMPARE(dialect.value(QStringLiteral("from")).toObject().value(QStringLiteral("extensions")).toObject().value(
                 QStringLiteral("math")),
             QStringLiteral("library"));
    // The library's folder is where /rooted includes are found, but only when asked.
    QCOMPARE(box.run({"check", "library/other.md", "--use-saved-settings"}).code, 0);
    const Ran unasked = box.run({"check", "library/other.md", "--json"});
    QCOMPARE(unasked.code, 2);
    QVERIFY(codesOf(unasked.json().value(QStringLiteral("documents")).toArray().first().toObject())
                .contains(QStringLiteral("include-no-library")));
    // Outside every library, only the defaults.
    dialect = dialectOf({"loose.md", "--use-saved-settings"});
    QCOMPARE(dialect.value(QStringLiteral("profile")).toString(), QStringLiteral("writer"));
    QVERIFY(dialect.value(QStringLiteral("libraryRoot")).isNull());
    QCOMPARE(dialect.value(QStringLiteral("savedSettings")).toBool(true), false);
    // The flags win over what is saved.
    dialect = dialectOf({"library/doc.md", "--use-saved-settings", "--profile", "writer", "--extension", "math",
                         "--library", "library/chapters"});
    QCOMPARE(dialect.value(QStringLiteral("profile")).toString(), QStringLiteral("writer"));
    QCOMPARE(dialect.value(QStringLiteral("from")).toObject().value(QStringLiteral("profile")).toString(),
             QStringLiteral("flag"));
    QVERIFY(dialect.value(QStringLiteral("extensions")).toArray().contains(QStringLiteral("math")));
    QCOMPARE(dialect.value(QStringLiteral("libraryRoot")).toString(), box.path(QStringLiteral("library/chapters")));
    QCOMPARE(dialect.value(QStringLiteral("from")).toObject().value(QStringLiteral("libraryRoot")).toString(),
             QStringLiteral("flag"));
    // The tree follows the saved profile.
    const QSet<QString> types = treeTypes(box, {"library/doc.md", "--use-saved-settings"});
    QVERIFY(types.contains(QStringLiteral("footnote-reference")));
    QVERIFY(!types.contains(QStringLiteral("math-inline")));

    // Settings that cannot be read are an error only when asked for.
    writeFile(paths.documentSettingsFile(), QByteArray("{not json"));
    const Ran damaged = box.run({"check", "library/doc.md", "--use-saved-settings"});
    QCOMPARE(damaged.code, 3);
    QVERIFY2(damaged.err.contains(QStringLiteral("document-settings.json")), qPrintable(damaged.said()));
    QCOMPARE(box.run({"check", "library/doc.md"}).code, 0);
}

void renderWritesEveryForm()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("doc.md"),
              QStringLiteral("# Title\n\nSome *text*.\n\n## Part [Label]\n\nMore.\n\nchapter.md\n"));
    box.write(QStringLiteral("chapter.md"), QStringLiteral("### Inside\n\nIncluded.\n"));

    const Ran html = box.run({QStringLiteral("render"), QStringLiteral("doc.md")});
    QCOMPARE(html.code, 0);
    QVERIFY(html.out.startsWith(QStringLiteral("<h1 id=\"title\">Title</h1>\n<p>Some <em>text</em>.</p>")));
    QVERIFY(html.out.contains(QStringLiteral("<h3 id=\"inside\">Inside</h3>")));
    QVERIFY(!html.out.contains(QStringLiteral("<html")));
    QCOMPARE(box.run({"render", "doc.md", "--to", "html"}).out, html.out);

    const Ran text = box.run({"render", "doc.md", "--to", "text"});
    QCOMPARE(text.code, 0);
    QVERIFY(text.out.contains(QStringLiteral("Some text.")));
    QVERIFY(text.out.contains(QStringLiteral("Included.")));
    QVERIFY(!text.out.contains(u'*'));
    QVERIFY(!text.out.contains(QStringLiteral("Label")));

    const Ran outline = box.run({"render", "doc.md", "--to", "outline"});
    QCOMPARE(outline.out, QStringLiteral("Title\n  Part\n    Inside\n"));
    const QJsonArray headings = box.run({"render", "doc.md", "--to", "outline", "--json"})
                                    .json()
                                    .value(QStringLiteral("result"))
                                    .toArray();
    QCOMPARE(headings.size(), 3);
    const QJsonObject part = headings[1].toObject();
    QCOMPARE(part.value(QStringLiteral("level")).toInt(), 2);
    QCOMPARE(part.value(QStringLiteral("text")).toString(), QStringLiteral("Part"));
    QCOMPARE(part.value(QStringLiteral("slug")).toString(), QStringLiteral("part"));
    QCOMPARE(part.value(QStringLiteral("label")).toString(), QStringLiteral("Label"));
    QCOMPARE(part.value(QStringLiteral("line")).toInt(), 5);
    QCOMPARE(headings[2].toObject().value(QStringLiteral("file")).toInt(), 1);
    QVERIFY(headings[0].toObject().value(QStringLiteral("label")).isNull());

    // The tree: every node with its type, file and range.
    const Ran tree = box.run({"render", "doc.md", "--to", "json-tree"});
    QCOMPARE(tree.code, 0);
    const QJsonObject document = tree.json();
    QCOMPARE(headerProblem(document, QStringLiteral("render")), QString());
    QCOMPARE(dialectProblem(document.value(QStringLiteral("dialect")).toObject()), QString());
    const QJsonArray files = document.value(QStringLiteral("files")).toArray();
    QCOMPARE(files.size(), 2);
    QCOMPARE(files[0].toObject().value(QStringLiteral("path")).toString(), box.path(QStringLiteral("doc.md")));
    QCOMPARE(files[1].toObject().value(QStringLiteral("includedFrom")).toInt(), 0);
    const QJsonObject root = document.value(QStringLiteral("root")).toObject();
    QCOMPARE(root.value(QStringLiteral("type")).toString(), QStringLiteral("document"));
    QCOMPARE(nodeProblem(root, int(files.size())), QString());
    const QJsonArray blocks = root.value(QStringLiteral("children")).toArray();
    const QJsonObject title = blocks[0].toObject();
    QCOMPARE(title.value(QStringLiteral("type")).toString(), QStringLiteral("heading"));
    QCOMPARE(title.value(QStringLiteral("level")).toInt(), 1);
    QCOMPARE(title.value(QStringLiteral("range")).toArray(), (QJsonArray{0, 7}));
    QCOMPARE(title.value(QStringLiteral("open")).toArray(), (QJsonArray{0, 1}));
    QCOMPARE(title.value(QStringLiteral("line")).toInt(), 1);
    QCOMPARE(blocks[2].toObject().value(QStringLiteral("label")).toString(), QStringLiteral("Label"));
    QCOMPARE(blocks[2].toObject().value(QStringLiteral("line")).toInt(), 5);
    const QJsonObject include = blocks.last().toObject();
    QCOMPARE(include.value(QStringLiteral("type")).toString(), QStringLiteral("include"));
    QCOMPARE(include.value(QStringLiteral("includedFile")).toInt(), 1);
    QCOMPARE(include.value(QStringLiteral("children")).toArray().first().toObject().value(QStringLiteral("file")).toInt(), 1);
    QVERIFY(document.value(QStringLiteral("diagnostics")).toArray().isEmpty());
    QCOMPARE(document.value(QStringLiteral("incomplete")).toBool(true), false);

    // With --json, a report with the result in it.
    const Ran report = box.run({"render", "doc.md", "--json"});
    QCOMPARE(report.code, 0);
    const QJsonObject result = report.json();
    QCOMPARE(headerProblem(result, QStringLiteral("render")), QString());
    QCOMPARE(result.value(QStringLiteral("to")).toString(), QStringLiteral("html"));
    QCOMPARE(result.value(QStringLiteral("result")).toString(), html.out);
    QCOMPARE(result.value(QStringLiteral("file")).toString(), box.path(QStringLiteral("doc.md")));
    QVERIFY(result.value(QStringLiteral("renderers")).toObject().contains(QStringLiteral("html")));
    QCOMPARE(dialectProblem(result.value(QStringLiteral("dialect")).toObject()), QString());
    QCOMPARE(box.run({"render", "doc.md", "--to", "json-tree", "--json"})
                 .json()
                 .value(QStringLiteral("result"))
                 .toObject()
                 .value(QStringLiteral("root")),
             QJsonValue(root));

    // Relative links and images of an included file lead from the document's
    // folder, or from the folder the HTML is written to.
    box.write(QStringLiteral("pictured.md"), QStringLiteral("![top](top.png)\n\nparts/part.md\n"));
    box.write(QStringLiteral("parts/part.md"), QStringLiteral("![inner](inner.png) [next](next.md#a)\n"));
    const QString pictured = box.run({"render", "pictured.md"}).out;
    QVERIFY2(pictured.contains(QStringLiteral("<img src=\"top.png\" alt=\"top\" />")), qPrintable(pictured));
    QVERIFY(pictured.contains(QStringLiteral("<img src=\"parts/inner.png\" alt=\"inner\" />")));
    QVERIFY(pictured.contains(QStringLiteral("<a href=\"parts/next.md#a\">next</a>")));
    QDir().mkpath(box.path(QStringLiteral("out")));
    QCOMPARE(box.run({"render", "pictured.md", "-o", "out/pictured.html"}).code, 0);
    QVERIFY(box.read(QStringLiteral("out/pictured.html")).contains("<img src=\"../top.png\" alt=\"top\" />"));
    QVERIFY(box.read(QStringLiteral("out/pictured.html")).contains("<img src=\"../parts/inner.png\" alt=\"inner\" />"));
    // The tree keeps every destination as written.
    QVERIFY(box.run({"render", "pictured.md", "--to", "json-tree"}).out.contains(QStringLiteral("\"dest\": \"inner.png\"")));

    // A document whose include cannot be read is rendered anyway, and says so.
    box.write(QStringLiteral("partial.md"), QStringLiteral("# Here\n\nnot-there.md\n"));
    const Ran partial = box.run({"render", "partial.md"});
    QCOMPARE(partial.code, 0);
    QVERIFY(partial.out.contains(QStringLiteral("<p>not-there.md</p>")));
    QVERIFY(partial.err.contains(QStringLiteral("parch check partial.md")));
    QCOMPARE(box.run({"render", "partial.md", "--json"}).json().value(QStringLiteral("incomplete")).toBool(), true);
}

void renderWritesFilesOnlyWhereAllowed()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("doc.md"), QStringLiteral("# Doc\n\nchapter.md\n"));
    box.write(QStringLiteral("chapter.md"), QStringLiteral("Chapter.\n"));
    const QByteArray source = box.read(QStringLiteral("doc.md"));

    const Ran written = box.run({"render", "doc.md", "-o", "out.html"});
    QVERIFY2(written.code == 0, qPrintable(written.said()));
    QVERIFY(written.out.isEmpty());
    QCOMPARE(box.read(QStringLiteral("out.html")), box.run({"render", "doc.md"}).out.toUtf8());
    box.write(QStringLiteral("out.html"), QStringLiteral("kept"));
    const Ran refused = box.run({"render", "doc.md", "--output", "out.html"});
    QCOMPARE(refused.code, 3);
    QVERIFY(refused.err.contains(QStringLiteral("--force")));
    QCOMPARE(box.read(QStringLiteral("out.html")), QByteArray("kept"));
    QCOMPARE(box.run({"render", "doc.md", "-o", "out.html", "--force", "--to", "text"}).code, 0);
    QVERIFY(box.read(QStringLiteral("out.html")).contains("Chapter."));
    // Never the document or what it includes, even with --force.
    QCOMPARE(box.run({"render", "doc.md", "-o", "doc.md", "--force"}).code, 3);
    QCOMPARE(box.run({"render", "doc.md", "-o", "chapter.md", "--force"}).code, 3);
    QCOMPARE(box.read(QStringLiteral("doc.md")), source);
    QCOMPARE(box.read(QStringLiteral("chapter.md")), QByteArray("Chapter.\n"));
    QCOMPARE(box.run({"render", "doc.md", "-o", "no-folder/out.html"}).code, 3);
    // With --json the report names the file instead of holding the result.
    const QJsonObject report = box.run({"render", "doc.md", "-o", "tree.json", "--to", "json-tree", "--json"}).json();
    QCOMPARE(report.value(QStringLiteral("output")).toString(), box.path(QStringLiteral("tree.json")));
    QVERIFY(!report.contains(QStringLiteral("result")));
    QCOMPARE(QJsonDocument::fromJson(box.read(QStringLiteral("tree.json"))).object().value(QStringLiteral("command")),
             QJsonValue(QStringLiteral("render")));
}

void exportAssemblesMarkdown()
{
    Sandbox box;
    QVERIFY(box.isValid());
    writeBook(box);
    const Ran ran = box.run({"export", "book/main.md", "--to", "md", "-o", "book/assembled.md"});
    QVERIFY2(ran.code == 0, qPrintable(ran.said()));
    QCOMPARE(ran.out, box.path(QStringLiteral("book/assembled.md")) + u'\n');
    const QString text = QString::fromUtf8(box.read(QStringLiteral("book/assembled.md")));
    // The document's own text is as it was, front matter and all.
    QVERIFY(text.startsWith(QStringLiteral("---\ntitle: Book\nwho: Root\n---\n\n# Book\n\nHello [%who]. See [site][1] "
                                           "and [home][].\n")));
    QVERIFY(text.contains(QStringLiteral("![cover](images/cover.png)")));
    // Chapter one in place of its line, without its front matter, its
    // variable written out, its notes and definitions relabelled, its paths
    // from the folder the export is in.
    QVERIFY2(text.contains(QStringLiteral(
                 "\n## One\n\nHi Chapter of [%title]. A [link][1-2], [home][home-2], [home][home-2] and [Book][]. "
                 "Note[^1-2].\n![fig](chapters/fig.png) <img src=\"chapters/fig.png\" alt=\"f\"> [up](main.md#book)\n\n"
                 "[^1-2]: One's note.\n\n[1-2]: https://example.org/one\n[home-2]: https://example.org/one-home\n")),
             qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("who: Chapter")));
    QVERIFY(!text.contains(QStringLiteral("chapters/one.md")));
    // Chapter two, whose fence its file leaves open, closed where it ends.
    QVERIFY2(text.contains(QStringLiteral("## Two\n\nLast[^1].\n\n```\nunclosed fence\n```\n")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("[^1]: Root note.")));
    QVERIFY(text.contains(QStringLiteral("[#Doe]: Jane Doe. *A Book*.")));

    // Written elsewhere, pictures travel with the new document and ordinary links lead back.
    QDir().mkpath(box.path(QStringLiteral("out")));
    QCOMPARE(box.run({"export", "book/main.md", "-o", "out/book.md"}).code, 0);
    const QString elsewhere = QString::fromUtf8(box.read(QStringLiteral("out/book.md")));
    QVERIFY(elsewhere.contains(QStringLiteral("![cover](book_files/cover.png)")));
    QVERIFY(elsewhere.contains(QStringLiteral("![fig](book_files/fig.png)")));
    QVERIFY(elsewhere.contains(QStringLiteral("[up](../book/main.md#book)")));
    // Remote links are not paths.
    QVERIFY(elsewhere.contains(QStringLiteral("[1]: https://example.org/root")));
    QCOMPARE(box.run({"check", "out/book.md"}).code, Cli::Ok);
    QCOMPARE(box.read("out/book_files/cover.png"), box.read("book/images/cover.png"));
    QCOMPARE(box.read("out/book_files/fig.png"), box.read("book/chapters/fig.png"));
    QCOMPARE(box.run({"export", "book/main.md", "-o", "out/linked.md", "--images", "link"}).code, Cli::Ok);
    QVERIFY(box.read("out/linked.md").contains("![cover](../book/images/cover.png)"));
    QCOMPARE(box.run({"check", "out/linked.md"}).code, Cli::Problems);
    QCOMPARE(box.run({"check", "out/linked.md", "--library", "."}).code, Cli::Ok);

    // The JSON report.
    const Ran report = box.run({"export", "book/main.md", "-o", "out/book.md", "--force", "--json"});
    QCOMPARE(report.code, 0);
    const QJsonObject result = report.json();
    QCOMPARE(headerProblem(result, QStringLiteral("export")), QString());
    QCOMPARE(result.value(QStringLiteral("format")).toString(), QStringLiteral("md"));
    QCOMPARE(result.value(QStringLiteral("output")).toString(), box.path(QStringLiteral("out/book.md")));
    QCOMPARE(result.value(QStringLiteral("written")).toArray(),
             (QJsonArray{box.path("out/book.md"), box.path("out/book_files/cover.png"), box.path("out/book_files/fig.png")}));
    QVERIFY(result.value(QStringLiteral("limitations")).toArray().isEmpty());
    QVERIFY(result.value(QStringLiteral("warnings")).toArray().isEmpty());
    const QJsonObject recipe = result.value(QStringLiteral("recipe")).toObject();
    QCOMPARE(recipe.value(QStringLiteral("source")).toString(), box.path(QStringLiteral("book/main.md")));
    QCOMPARE(recipe.value(QStringLiteral("format")).toString(), QStringLiteral("md"));
    QCOMPARE(dialectProblem(recipe.value(QStringLiteral("dialect")).toObject()), QString());
    QCOMPARE(dialectProblem(result.value(QStringLiteral("dialect")).toObject()), QString());
}

void exportAssemblyKeepsMeaning()
{
    // The assembled Markdown, read on its own, means what the document and
    // its includes mean together: the same text, headings, notes, and
    // destinations, written beside the source or elsewhere.
    Sandbox box;
    QVERIFY(box.isValid());
    writeBook(box);
    // Citations, tables, math and more, in a third chapter in a folder of its own.
    box.write(QStringLiteral("book/main.md"),
              QString::fromUtf8(box.read(QStringLiteral("book/main.md"))) +
                  QStringLiteral("\nparts/three.md\n"));
    box.write(QStringLiteral("book/parts/three.md"),
              QStringLiteral("---\nwho: Third\n---\n# Three\n\n[%who] cites[][#Doe]. ==Marked== x^2 $e=mc^2$\n\n"
                             "| a | b |\n| - | - |\n| wide || \n\nTerm\n: Definition\n\n"
                             "![pic](../images/cover.png) [back](../chapters/one.md#one)\n\n"
                             "An inline note[^Here it is.] and a numbered one[^1].\n\n[^1]: Third note.\n"));
    const DocumentCheck::Loaded source = loadedFrom(box.path(QStringLiteral("book/main.md")));
    QVERIFY(!source.document.incomplete);
    const Meaning expected = meaningOf(source.document);
    QVERIFY(expected.notes.size() >= 4);

    for (const char *output : {"book/assembled.md", "out/deep/assembled.md"}) {
        QDir().mkpath(QFileInfo(box.path(QLatin1StringView(output))).absolutePath());
        const Ran ran = box.run({"export", "book/main.md", "-o", QLatin1StringView(output), "--force", "--images", "link"});
        QVERIFY2(ran.code == 0, qPrintable(ran.said()));
        const DocumentCheck::Loaded assembled = loadedFrom(box.path(QLatin1StringView(output)));
        QCOMPARE(assembled.document.files.size(), size_t(1));
        const Meaning got = meaningOf(assembled.document);
        QCOMPARE(got.text, expected.text);
        QCOMPARE(got.headings, expected.headings);
        QCOMPARE(got.notes, expected.notes);
        QCOMPARE(got.destinations, expected.destinations);
        QCOMPARE(md::toHtml(assembled.document).count(QStringLiteral("<a href=\"#cite-1\">")),
                 md::toHtml(source.document).count(QStringLiteral("<a href=\"#cite-1\">")));
    }
    // Two files that define one citation key differently: said, since only one can be kept.
    box.write(QStringLiteral("book/chapters/two.md"), QStringLiteral("## Two\n\n[#Doe]: Someone else.\n"));
    const Ran clash = box.run({"export", "book/main.md", "-o", "book/assembled.md", "--force", "--json"});
    QCOMPARE(clash.code, 0);
    QVERIFY2(clash.json().value(QStringLiteral("limitations")).toArray().first().toString().contains(
                 QStringLiteral("[#doe]")),
             qPrintable(clash.out));
}

void exportWritesAStandalonePage()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("docs/page.md"),
              QStringLiteral("---\ntitle: Front <Title>\nauthor: A. Writer\nlang: en-GB\ndescription: About it\n---\n\n"
                             "# Heading\n\nText with $x^2$ and [bad](javascript:alert(1)) and <script>alert(2)</script>.\n\n"
                             "$$\ny = 1\n$$\n\n```mermaid\ngraph TD; A-->B\n```\n\n"
                             "![pic](<pics/a b.png>) [other](other.md) [web](https://example.org)\n"));
    QDir().mkpath(box.path(QStringLiteral("docs/pics")));
    QImage(4, 4, QImage::Format_ARGB32).save(box.path(QStringLiteral("docs/pics/a b.png")));
    QDir().mkpath(box.path(QStringLiteral("out")));
    const Ran ran = box.run({"export", "docs/page.md", "--to", "html", "-o", "out/page.html", "--json"});
    QVERIFY2(ran.code == 0, qPrintable(ran.said()));
    const QString page = QString::fromUtf8(box.read(QStringLiteral("out/page.html")));
    QVERIFY(page.startsWith(QStringLiteral("<!DOCTYPE html>\n<html lang=\"en-GB\">\n<head>\n<meta charset=\"utf-8\">")));
    QVERIFY(page.contains(QStringLiteral("<title>Front &lt;Title&gt;</title>")));
    QVERIFY(page.contains(QStringLiteral("<meta name=\"author\" content=\"A. Writer\">")));
    QVERIFY(page.contains(QStringLiteral("<meta name=\"description\" content=\"About it\">")));
    QVERIFY(page.contains(QStringLiteral("<meta name=\"generator\" content=\"parch " PARCH_VERSION "\">")));
    // Its own typography, inside the page; nothing fetched, nothing run.
    QFile css(QStringLiteral(":/export.css"));
    QVERIFY(css.open(QIODevice::ReadOnly));
    QVERIFY(page.contains(QStringLiteral("<style>\n") + QString::fromUtf8(css.readAll()) + QStringLiteral("</style>")));
    QVERIFY(!page.contains(QStringLiteral("<link"), Qt::CaseInsensitive));
    QVERIFY(!page.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
    QVERIFY(!page.contains(QStringLiteral("javascript:"), Qt::CaseInsensitive));
    QVERIFY(page.contains(QStringLiteral("&lt;script&gt;alert(2)&lt;/script&gt;")));
    QVERIFY(page.contains(QStringLiteral("<main>\n<h1 id=\"heading\">Heading</h1>")));
    QVERIFY(page.endsWith(QStringLiteral("</main>\n</body>\n</html>\n")));
    // Pictures go into a folder beside the page; links lead from its folder.
    QVERIFY2(page.contains(QStringLiteral("<img src=\"page_files/a-b.png\" alt=\"pic\" />")), qPrintable(page));
    QCOMPARE(box.read(QStringLiteral("out/page_files/a-b.png")), box.read(QStringLiteral("docs/pics/a b.png")));
    QVERIFY(page.contains(QStringLiteral("<a href=\"../docs/other.md\">other</a>")));
    QVERIFY(page.contains(QStringLiteral("<a href=\"https://example.org\">web</a>")));
    QVERIFY(page.contains(QStringLiteral("<pre class=\"mermaid\">")));

    const QJsonObject result = ran.json();
    QCOMPARE(headerProblem(result, QStringLiteral("export")), QString());
    QCOMPARE(result.value(QStringLiteral("renderers")).toObject().value(QStringLiteral("html")).toString(),
             QStringLiteral("parch " PARCH_VERSION));
    const QJsonArray limitations = result.value(QStringLiteral("limitations")).toArray();
    QCOMPARE(limitations.size(), 2);
    QCOMPARE(limitations[0].toString(),
             QStringLiteral("Math is written as its TeX source (2 formulas): the math helper (render-svg) is not installed"));
    QCOMPARE(limitations[1].toString(), QStringLiteral("Mermaid diagrams are written as their source (1 diagram): the "
                                                       "diagram helper (merman-cli) is not installed"));
    const QJsonObject recipe = result.value(QStringLiteral("recipe")).toObject();
    QCOMPARE(recipe.value(QStringLiteral("format")).toString(), QStringLiteral("html"));
    QCOMPARE(recipe.value(QStringLiteral("output")).toString(), box.path(QStringLiteral("out/page.html")));
    QVERIFY(recipe.value(QStringLiteral("options")).toObject().isEmpty());
    // Linked where they are, they are found from the page's folder.
    const Ran linked = box.run({"export", "docs/page.md", "-o", "out/linked.html", "--images", "link", "--json"});
    QCOMPARE(linked.code, 0);
    QVERIFY(box.read(QStringLiteral("out/linked.html")).contains("<img src=\"../docs/pics/a%20b.png\" alt=\"pic\" />"));
    QVERIFY(linked.json().value(QStringLiteral("limitations")).toArray().last().toString().startsWith(
        QStringLiteral("Images are linked from the page, not inside it (1 image)")));
    QCOMPARE(linked.json().value(QStringLiteral("recipe")).toObject().value(QStringLiteral("options")).toObject(),
             (QJsonObject{{QStringLiteral("images"), QStringLiteral("link")}}));

    // Without --json: the path on the standard output, the notes on the error output.
    const Ran plain = box.run({"export", "docs/page.md", "-o", "out/page.html", "--force"});
    QCOMPARE(plain.out, box.path(QStringLiteral("out/page.html")) + u'\n');
    QVERIFY(plain.err.contains(QStringLiteral("note: Math is written as its TeX source (2 formulas)")));

    // The title: --title, the front matter's, the first heading, the file's name.
    const Ran titled = box.run({"export", "docs/page.md", "-o", "out/titled.htm", "--title", "Given", "--json"});
    QCOMPARE(titled.code, 0);
    QVERIFY(box.read(QStringLiteral("out/titled.htm")).contains("<title>Given</title>"));
    QCOMPARE(titled.json().value(QStringLiteral("recipe")).toObject().value(QStringLiteral("options")).toObject().value(
                 QStringLiteral("title")),
             QJsonValue(QStringLiteral("Given")));
    box.write(QStringLiteral("headed.md"), QStringLiteral("Intro.\n\n## First *heading*\n"));
    QCOMPARE(box.run({"export", "headed.md", "-o", "headed.html"}).code, 0);
    QVERIFY(box.read(QStringLiteral("headed.html")).contains("<title>First heading</title>"));
    QVERIFY(box.read(QStringLiteral("headed.html")).contains("<html>\n"));
    box.write(QStringLiteral("bare-name.md"), QStringLiteral("Just text.\n"));
    QCOMPARE(box.run({"export", "bare-name.md", "-o", "bare.html"}).code, 0);
    QVERIFY(box.read(QStringLiteral("bare.html")).contains("<title>bare-name</title>"));
    // A missing image is a warning, and the page is written all the same.
    box.write(QStringLiteral("missing-image.md"), QStringLiteral("![gone](gone.png)\n"));
    const Ran warned = box.run({"export", "missing-image.md", "-o", "warned.html", "--json"});
    QCOMPARE(warned.code, 0);
    const QJsonArray warnings = warned.json().value(QStringLiteral("warnings")).toArray();
    QCOMPARE(warnings.size(), 1);
    QCOMPARE(warnings[0].toObject().value(QStringLiteral("code")).toString(), QStringLiteral("unresolved-asset"));
    QCOMPARE(problemProblem(warnings[0].toObject()), QString());
    QVERIFY(box.run({"export", "missing-image.md", "-o", "warned.html", "--force"}).err.contains(
        QStringLiteral("[unresolved-asset]")));
}

void exportRefusesWhatItMustNot()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("doc.md"), QStringLiteral("# Doc\n\nchapter.md\n"));
    box.write(QStringLiteral("chapter.md"), QStringLiteral("Chapter.\n"));
    box.write(QStringLiteral("partial.md"), QStringLiteral("# Partial\n\nnot-there.md\n"));
    QFile::link(box.path(QStringLiteral("doc.md")), box.path(QStringLiteral("link-to-doc.md")));
    box.write(QStringLiteral("there.html"), QStringLiteral("kept"));

    // A file that is there needs --force.
    const Ran there = box.run({"export", "doc.md", "-o", "there.html"});
    QCOMPARE(there.code, 3);
    QVERIFY(there.err.contains(QStringLiteral("give --force to replace it")));
    QCOMPARE(box.read(QStringLiteral("there.html")), QByteArray("kept"));
    QCOMPARE(box.run({"export", "doc.md", "-o", "there.html", "--force"}).code, 0);
    QVERIFY(box.read(QStringLiteral("there.html")).startsWith("<!DOCTYPE html>"));
    // Never the document, a link to it or a file it includes.
    for (const char *output : {"doc.md", "link-to-doc.md", "chapter.md", "./doc.md"}) {
        const Ran ran = box.run({"export", "doc.md", "--to", "md", "-o", QLatin1StringView(output), "--force"});
        QVERIFY2(ran.code == 3, qPrintable(ran.said()));
        QVERIFY(ran.err.contains(QStringLiteral("parch never writes over it")));
    }
    QCOMPARE(box.read(QStringLiteral("doc.md")), QByteArray("# Doc\n\nchapter.md\n"));
    QCOMPARE(box.read(QStringLiteral("chapter.md")), QByteArray("Chapter.\n"));
    QCOMPARE(box.run({"export", "doc.md", "-o", "no-folder/doc.html"}).code, 3);
    QDir().mkpath(box.path(QStringLiteral("folder.html")));
    QCOMPARE(box.run({"export", "doc.md", "-o", "folder.html", "--force"}).code, 3);

    // A document missing an include is not exported as if it were whole.
    const Ran partial = box.run({"export", "partial.md", "-o", "partial.html"});
    QCOMPARE(partial.code, 2);
    QVERIFY2(partial.err.contains(QStringLiteral("partial.md:3:1: error:")), qPrintable(partial.said()));
    QVERIFY(!QFileInfo::exists(box.path(QStringLiteral("partial.html"))));

    // PDF, DOCX and EPUB follow the same rules: written, then refused over
    // a file that is there without --force, and never over the document.
    for (const char *format : {"pdf", "docx", "epub"}) {
        const QString output = QStringLiteral("doc.") + QLatin1StringView(format);
        const Ran written = box.run({"export", "doc.md", "-o", output});
        QVERIFY2(written.code == 0, qPrintable(written.said()));
        const QByteArray first = box.read(output);
        QVERIFY(!first.isEmpty());
        const Ran again = box.run({"export", "doc.md", "--to", QLatin1StringView(format), "-o", output});
        QCOMPARE(again.code, 3);
        QCOMPARE(box.read(output), first);
        const Ran over = box.run({"export", "doc.md", "--to", QLatin1StringView(format), "-o", "chapter.md", "--force"});
        QCOMPARE(over.code, 3);
        QCOMPARE(box.read(QStringLiteral("chapter.md")), QByteArray("Chapter.\n"));
    }

    // --quiet says nothing when all is well, and still says what is wrong.
    box.write(QStringLiteral("math.md"), QStringLiteral("$x$ ![gone](gone.png)\n"));
    const Ran quiet = box.run({"export", "math.md", "-o", "quiet.html", "--quiet"});
    QCOMPARE(quiet.code, 0);
    QVERIFY(quiet.out.isEmpty());
    QVERIFY(quiet.err.isEmpty());
    QVERIFY(QFileInfo::exists(box.path(QStringLiteral("quiet.html"))));
    QVERIFY(!box.run({"export", "math.md", "-o", "quiet.html", "-q"}).err.isEmpty());
}

void exportLeavesTheSourceAlone()
{
    Sandbox box;
    QVERIFY(box.isValid());
    writeBook(box);
    // A chapter with Windows line endings and a byte-order mark.
    writeFile(box.path(QStringLiteral("book/chapters/two.md")), QByteArray("\xef\xbb\xbf## Two\r\n\r\nLast.\r\n"));
    const QMap<QString, QByteArray> before = filesBeneath(box.path(QStringLiteral("book")));
    QDir().mkpath(box.path(QStringLiteral("out")));
    for (const QStringList &arguments :
         {QStringList{"export", "book/main.md", "-o", "out/a.md"}, QStringList{"export", "book/main.md", "-o", "out/a.html"},
          QStringList{"render", "book/main.md", "-o", "out/r.html"}, QStringList{"check", "book/main.md"},
          QStringList{"stats", "book/main.md"}, QStringList{"export", "book/main.md", "-o", "out/a.pdf"}}) {
        box.run(arguments);
        QCOMPARE(filesBeneath(box.path(QStringLiteral("book"))), before);
    }
    // The assembled text keeps the chapter's own line endings.
    QVERIFY(box.read(QStringLiteral("out/a.md")).contains("## Two\r\n\r\nLast."));
}

void exportFormatsPlugInThroughOneInterface()
{
    QCOMPARE(Exporters::formats(), (QStringList{"md", "html", "pdf", "docx", "epub"}));
    QCOMPARE(Exporters::find(QStringLiteral("HTML"))->format(), QStringLiteral("html"));
    QVERIFY(!Exporters::find(QStringLiteral("rtf")));
    QCOMPARE(Exporters::forFile(QStringLiteral("/a/b.markdown"))->format(), QStringLiteral("md"));
    QCOMPARE(Exporters::forFile(QStringLiteral("b.HTM"))->format(), QStringLiteral("html"));
    QCOMPARE(Exporters::forFile(QStringLiteral("b.EPUB"))->format(), QStringLiteral("epub"));
    QVERIFY(!Exporters::forFile(QStringLiteral("README")));
    for (const Exporter *exporter : Exporters::all()) {
        QVERIFY(!exporter->title().isEmpty());
        QVERIFY(!exporter->suffixes().isEmpty());
        // Every format is written, and each of its options is described.
        QVERIFY(exporter->available());
        for (const ExportOption &option : exporter->options()) {
            QVERIFY(!option.description.isEmpty());
            QVERIFY(!option.label.isEmpty());
            if (!option.choices.isEmpty()) QVERIFY(option.choices.contains(option.defaultValue));
        }
        Sandbox box;
        box.write(QStringLiteral("a.md"), QStringLiteral("# A\n"));
        const DocumentCheck::Loaded loaded = loadedFrom(box.path(QStringLiteral("a.md")));
        ExportRecipe recipe;
        recipe.source = loaded.path;
        recipe.output = box.path(QStringLiteral("a.") + exporter->suffixes().first());
        const ExportResult result = exporter->write(loaded.document, recipe);
        QVERIFY2(result.ok, qPrintable(exporter->format() + u' ' + result.error));
        QCOMPARE(result.written.first(), recipe.output);
        QVERIFY(QFileInfo(recipe.output).size() > 0);
    }
    // The format's options are the command line's.
    QCOMPARE(Exporters::find(QStringLiteral("html"))->options().first().name, QStringLiteral("title"));
    // A recipe records what was asked.
    ExportRecipe recipe;
    recipe.source = QStringLiteral("/d/a.md");
    recipe.format = QStringLiteral("html");
    recipe.output = QStringLiteral("/d/a.html");
    recipe.dialect = Dialect::choose({});
    recipe.options.insert(QStringLiteral("title"), QStringLiteral("T"));
    recipe.options.insert(QStringLiteral("switch"), QString());
    const QJsonObject json = recipe.toJson();
    QCOMPARE(json.value(QStringLiteral("source")).toString(), QStringLiteral("/d/a.md"));
    QCOMPARE(json.value(QStringLiteral("options")).toObject().value(QStringLiteral("switch")), QJsonValue(true));
    QCOMPARE(dialectProblem(json.value(QStringLiteral("dialect")).toObject()), QString());
    // What is the source.
    Sandbox box;
    box.write(QStringLiteral("a.md"), QStringLiteral("b.md\n"));
    box.write(QStringLiteral("b.md"), QStringLiteral("B.\n"));
    const DocumentCheck::Loaded loaded = loadedFrom(box.path(QStringLiteral("a.md")));
    QVERIFY(Exporters::isSource(loaded.document, loaded.dialect, box.path(QStringLiteral("a.md"))));
    QVERIFY(Exporters::isSource(loaded.document, loaded.dialect, box.path(QStringLiteral("b.md"))));
    QVERIFY(Exporters::isSource(loaded.document, loaded.dialect, box.path(QStringLiteral("x/../b.md"))));
    QVERIFY(!Exporters::isSource(loaded.document, loaded.dialect, box.path(QStringLiteral("c.md"))));
}

void nothingADocumentIsMadeFromIsWrittenOver()
{
    Sandbox box;
    QVERIFY(box.isValid());
    const QString refs = QStringLiteral("[{\"id\":\"doe\",\"type\":\"book\",\"title\":\"Tides\","
                                        "\"author\":[{\"family\":\"Doe\"}],\"issued\":{\"date-parts\":[[2020]]}}]");
    box.write(QStringLiteral("refs.json"), refs);
    box.write(QStringLiteral("style.csl"), QStringLiteral("<style/>"));
    box.write(QStringLiteral("pic.png"), QStringLiteral("not looked at"));
    box.write(QStringLiteral("doc.md"), QStringLiteral("---\nbibliography: refs.json\n---\n\nAs [@doe] says.\n\n![A](pic.png)\n"));
    const int before = box.run({"check", "doc.md"}).code;
    // Not its bibliography, not a picture it shows: even a forced render or
    // export of its tree is refused, and its bibliography stays its own.
    for (const char *output : {"refs.json", "pic.png"}) {
        const Ran render = box.run({"render", "doc.md", "--to", "json-tree", "-o", QLatin1StringView(output), "--force"});
        QVERIFY2(render.code == 3, qPrintable(render.said()));
        QVERIFY(render.err.contains(QStringLiteral("parch never writes over it")));
        const Ran exported = box.run({"export", "doc.md", "--to", "md", "-o", QLatin1StringView(output), "--force"});
        QVERIFY2(exported.code == 3, qPrintable(exported.said()));
    }
    QCOMPARE(box.read(QStringLiteral("refs.json")), refs.toUtf8());
    QCOMPARE(box.run({"check", "doc.md"}).code, before);
    // Its style the same.
    box.write(QStringLiteral("styled.md"), QStringLiteral("---\ncsl: style.csl\n---\n\nText.\n"));
    QCOMPARE(box.run({"render", "styled.md", "-o", "style.csl", "--force"}).code, 3);
    QCOMPARE(box.read(QStringLiteral("style.csl")), QByteArray("<style/>"));
}

void assembledMarkdownKeepsItsCitations()
{
    Sandbox box;
    QVERIFY(box.isValid());
    const auto entry = [](const char *key, const char *family) {
        return QStringLiteral("[{\"id\":\"%1\",\"type\":\"book\",\"title\":\"T\",\"author\":[{\"family\":\"%2\"}],"
                              "\"issued\":{\"date-parts\":[[2020]]}}]")
            .arg(QLatin1StringView(key), QLatin1StringView(family));
    };
    QDir().mkpath(box.path(QStringLiteral("src")));
    QDir().mkpath(box.path(QStringLiteral("out")));
    box.write(QStringLiteral("src/refs.json"), entry("doe", "Doe"));
    box.write(QStringLiteral("src/chapter.json"), entry("roe", "Roe"));
    // The book's bibliography is beside it; its chapter names one of its own.
    box.write(QStringLiteral("src/chapter.md"), QStringLiteral("---\nbibliography: chapter.json\n---\n\nAs [@roe] says.\n"));
    box.write(QStringLiteral("src/book.md"),
              QStringLiteral("---\ntitle: Book\nbibliography: refs.json\ncsl: apa\n---\n\nAs [@doe] says.\n\nchapter.md\n"));
    const Ran clean = box.run({"check", "src/book.md"});
    QVERIFY2(clean.code == 0, qPrintable(clean.said()));
    // Assembled somewhere else, and beside the book: each reads as the book did.
    for (const char *output : {"out/book.md", "src/whole.md"}) {
        const Ran exported = box.run({"export", "src/book.md", "--to", "md", "-o", QLatin1StringView(output)});
        QVERIFY2(exported.code == 0, qPrintable(exported.said()));
        const Ran checked = box.run({"check", QLatin1StringView(output)});
        QVERIFY2(checked.code == 0 && !checked.said().contains(QStringLiteral("missing-citation")),
                 qPrintable(checked.said() + u'\n' + QString::fromUtf8(box.read(QLatin1StringView(output)))));
        // What else the front matter says stays.
        QVERIFY(box.read(QLatin1StringView(output)).startsWith("---\ntitle: Book\n"));
    }
    // Written elsewhere, it carries its bibliographies along, as its pictures.
    QCOMPARE(box.read(QStringLiteral("out/book_files/refs.json")), entry("doe", "Doe").toUtf8());
    QCOMPARE(box.read(QStringLiteral("out/book_files/chapter.json")), entry("roe", "Roe").toUtf8());
}

void statsCountWhatAReaderReads()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("doc.md"),
              QStringLiteral("---\ntitle: Not counted words here\n---\n\n# Hello world\n\nOne *two* three. `code`\n\n"
                             "<!-- a hidden comment -->\n\n[A link](https://example.org/not/counted)\n\nchapter.md\n"));
    box.write(QStringLiteral("chapter.md"), QStringLiteral("## Four\n\nFive six.\n"));
    const Ran ran = box.run({"stats", "doc.md", "--json"});
    QCOMPARE(ran.code, 0);
    const QJsonObject result = ran.json();
    QCOMPARE(headerProblem(result, QStringLiteral("stats")), QString());
    QCOMPARE(dialectProblem(result.value(QStringLiteral("dialect")).toObject()), QString());
    // Hello world / One two three code / A link / Four / Five six
    QCOMPARE(result.value(QStringLiteral("words")).toInt(), 11);
    QCOMPARE(result.value(QStringLiteral("readingMinutes")).toInt(), 1);
    QCOMPARE(result.value(QStringLiteral("wordsPerMinute")).toInt(), 200);
    QCOMPARE(result.value(QStringLiteral("headings")).toInt(), 2);
    QCOMPARE(result.value(QStringLiteral("headingLevels")).toArray(), (QJsonArray{1, 1, 0, 0, 0, 0}));
    QCOMPARE(result.value(QStringLiteral("files")).toInt(), 2);
    QCOMPARE(result.value(QStringLiteral("file")).toString(), box.path(QStringLiteral("doc.md")));

    // Characters are what a reader sees, a cluster as one, line breaks not at all.
    box.write(QStringLiteral("short.md"), QStringLiteral("# Hi\n\nab cd \U0001F44D\U0001F3FD\n"));
    const QJsonObject counted = box.run({"stats", "short.md", "--json"}).json();
    QCOMPARE(counted.value(QStringLiteral("characters")).toInt(), 9);
    QCOMPARE(counted.value(QStringLiteral("charactersWithoutSpaces")).toInt(), 7);
    const Ran text = box.run({"stats", "short.md"});
    QCOMPARE(text.code, 0);
    QCOMPARE(text.out, QStringLiteral("short.md\n  Words         3\n  Characters    9 (7 without spaces)\n"
                                      "  Reading time  about 1 minute\n  Headings      1\n"));

    // Reading time rounds up; nothing to read is said.
    QString many;
    for (int i = 0; i < 401; ++i) many += QStringLiteral("word ");
    box.write(QStringLiteral("long.md"), many);
    QCOMPARE(box.run({"stats", "long.md", "--json"}).json().value(QStringLiteral("readingMinutes")).toInt(), 3);
    QVERIFY(box.run({"stats", "long.md"}).out.contains(QStringLiteral("about 3 minutes")));
    box.write(QStringLiteral("empty.md"), QString());
    QCOMPARE(box.run({"stats", "empty.md", "--json"}).json().value(QStringLiteral("words")).toInt(), 0);
    QVERIFY(box.run({"stats", "empty.md"}).out.contains(QStringLiteral("nothing to read")));
}

void skillPrintsAndInstalls()
{
    Sandbox box;
    QVERIFY(box.isValid());
    QFile bundled(QStringLiteral(":/skill.md"));
    QVERIFY(bundled.open(QIODevice::ReadOnly));
    const QByteArray skill = bundled.readAll();
    QVERIFY(skill.startsWith("---\nname: parch\ndescription: |\n"));
    for (const char *mention : {"parch check", "parch render", "parch export", "parch stats", "--force", "--json",
                                "{{TOC}}", "[^1]", "[#Doe:2006]", "+++", "mermaid", "--use-saved-settings"})
        QVERIFY2(skill.contains(mention), mention);

    const Ran printed = box.run({QStringLiteral("skill")});
    QCOMPARE(printed.code, 0);
    QCOMPARE(printed.out.toUtf8(), skill);
    const QJsonObject asJson = box.run({"skill", "--json"}).json();
    QCOMPARE(headerProblem(asJson, QStringLiteral("skill")), QString());
    QCOMPARE(asJson.value(QStringLiteral("skill")).toString().toUtf8(), skill);

    const QString file = box.home() + QStringLiteral("/.agents/skills/parch/SKILL.md");
    const QString link = box.home() + QStringLiteral("/.claude/skills/parch");
    // Without ~/.claude there is nothing to link from.
    const Ran first = box.run({"skill", "install"});
    QVERIFY2(first.code == 0, qPrintable(first.said()));
    QCOMPARE(first.out, QStringLiteral("Installed ") + file + u'\n');
    QCOMPARE(readFile(file), skill);
    QVERIFY(!QFileInfo::exists(box.home() + QStringLiteral("/.claude")));
    // With it, a link to the one copy.
    QDir().mkpath(box.home() + QStringLiteral("/.claude"));
    const Ran second = box.run({"skill", "install"});
    QCOMPARE(second.code, 0);
    QCOMPARE(second.out, QStringLiteral("Up to date: ") + file + QStringLiteral("\nLinked ") + link + u'\n');
    QVERIFY(QFileInfo(link).isSymLink());
    QCOMPARE(QFile::symLinkTarget(link), box.home() + QStringLiteral("/.agents/skills/parch"));
    QCOMPARE(readFile(link + QStringLiteral("/SKILL.md")), skill);
    // Again, nothing changes.
    const QJsonObject third = box.run({"skill", "install", "--json"}).json();
    QCOMPARE(headerProblem(third, QStringLiteral("skill")), QString());
    QCOMPARE(third.value(QStringLiteral("installed")).toString(), file);
    QCOMPARE(third.value(QStringLiteral("changed")).toBool(true), false);
    QCOMPARE(third.value(QStringLiteral("linked")).toString(), link);
    QVERIFY(QFileInfo(link).isSymLink());
    // An older parch skill is brought up to date.
    writeFile(file, QByteArray("---\nname: parch\ndescription: old\n---\n\nOld.\n"));
    const QJsonObject updated = box.run({"skill", "install", "--json"}).json();
    QCOMPARE(updated.value(QStringLiteral("changed")).toBool(), true);
    QCOMPARE(readFile(file), skill);
}

void skillLeavesOtherFilesAlone()
{
    Sandbox box;
    QVERIFY(box.isValid());
    const QString home = box.home();
    writeFile(home + QStringLiteral("/.agents/skills/other/SKILL.md"), QByteArray("---\nname: other\n---\n"));
    writeFile(home + QStringLiteral("/.claude/skills/mine/SKILL.md"), QByteArray("mine"));
    writeFile(home + QStringLiteral("/.claude/settings.json"), QByteArray("{}"));
    const QMap<QString, QByteArray> others = filesBeneath(home);
    QCOMPARE(box.run({"skill", "install"}).code, 0);
    QMap<QString, QByteArray> after = filesBeneath(home);
    after.remove(QStringLiteral(".agents/skills/parch/SKILL.md"));
    after.remove(QStringLiteral(".claude/skills/parch/SKILL.md"));
    QCOMPARE(after, others);

    // A SKILL.md that is not parch's is left as it is.
    {
        Sandbox foreign;
        const QString file = foreign.home() + QStringLiteral("/.agents/skills/parch/SKILL.md");
        writeFile(file, QByteArray("# My own notes about parch\n"));
        const Ran ran = foreign.run({"skill", "install"});
        QCOMPARE(ran.code, 3);
        QVERIFY(ran.err.contains(QStringLiteral("leaving it alone")));
        QCOMPARE(readFile(file), QByteArray("# My own notes about parch\n"));
    }
    // So is a link from ~/.claude that leads elsewhere, and a folder there.
    {
        Sandbox linked;
        QDir().mkpath(linked.home() + QStringLiteral("/.claude/skills"));
        QDir().mkpath(linked.path(QStringLiteral("elsewhere")));
        const QString link = linked.home() + QStringLiteral("/.claude/skills/parch");
        QVERIFY(QFile::link(linked.path(QStringLiteral("elsewhere")), link));
        const Ran ran = linked.run({"skill", "install"});
        QCOMPARE(ran.code, 3);
        QVERIFY(ran.err.contains(QStringLiteral("links elsewhere")));
        QCOMPARE(QFile::symLinkTarget(link), linked.path(QStringLiteral("elsewhere")));
    }
    {
        Sandbox folder;
        const QString kept = folder.home() + QStringLiteral("/.claude/skills/parch/SKILL.md");
        writeFile(kept, QByteArray("a skill of its own"));
        const Ran ran = folder.run({"skill", "install"});
        QCOMPARE(ran.code, 3);
        QVERIFY(ran.err.contains(QStringLiteral("not a link")));
        QCOMPARE(readFile(kept), QByteArray("a skill of its own"));
    }
}

void jsonAndTextAreSafeToPrint()
{
    QCOMPARE(Cli::printable(QStringLiteral("a\x1b[31mb\tc\nd\re\u009bf")),
             QStringLiteral("a�[31mb\tc\nd\re�f"));
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("doc.md"), QStringLiteral("# Title \x1b[2J\n\nText \u009b2J [x][\x1b]\n"));
    const Ran text = box.run({"render", "doc.md", "--to", "text"});
    QCOMPARE(text.code, 0);
    QVERIFY(!text.out.contains(QChar(0x1b)));
    QVERIFY(!text.out.contains(QChar(0x9b)));
    QVERIFY(text.out.contains(QChar(QChar::ReplacementCharacter)));
    const Ran check = box.run({"check", "doc.md"});
    QVERIFY(!check.out.contains(QChar(0x1b)));
    for (const char *to : {"json-tree", "html"}) {
        const Ran json = box.run({"render", "doc.md", "--to", QLatin1StringView(to), "--json"});
        QVERIFY(!json.out.contains(QChar(0x1b)));
        QVERIFY(!json.out.contains(QChar(0x9b)));
        QVERIFY(json.out.contains(QStringLiteral("\\u001b")));
        QVERIFY(json.out.contains(QStringLiteral("\\u009b")));
        // Escaped, the text is all there.
        QVERIFY(QJsonDocument::fromJson(json.out.toUtf8()).isObject());
    }
    // A file is not a terminal: it gets the text as it is.
    QCOMPARE(box.run({"render", "doc.md", "--to", "text", "-o", "doc.txt"}).code, 0);
    QVERIFY(box.read(QStringLiteral("doc.txt")).contains("\x1b[2J"));
}

void htmlDestinationsCanBeRewritten()
{
    const md::Document document = md::parse(QStringLiteral(
        "[a](x.md) ![i](p.png) <a href=\"y.md\">y</a> <img src=\"q.png\" alt=\"q\"> <https://e.org> [s](bad.md)\n"));
    md::HtmlOptions options;
    QStringList seen;
    options.destination = [&seen](const md::Node &node, const QString &dest) -> QString {
        seen.append(QString::fromLatin1(md::typeName(node.type)) + u' ' + dest);
        if (dest == u"bad.md") return QStringLiteral("javascript:alert(1)");
        return dest.startsWith(u"https:") ? dest : QStringLiteral("base/") + dest;
    };
    const QString html = md::toHtml(document, options);
    QVERIFY2(html.contains(QStringLiteral("<a href=\"base/x.md\">a</a>")), qPrintable(html));
    QVERIFY(html.contains(QStringLiteral("<img src=\"base/p.png\" alt=\"i\" />")));
    QVERIFY(html.contains(QStringLiteral("<a href=\"base/y.md\">y</a>")));
    QVERIFY(html.contains(QStringLiteral("<img src=\"base/q.png\" alt=\"q\" />")));
    QVERIFY(html.contains(QStringLiteral("<a href=\"https://e.org\">")));
    // What it returns is still held to the safe-URL rule.
    QVERIFY(html.contains(QStringLiteral("<a href=\"\">s</a>")));
    QCOMPARE(seen, (QStringList{"link x.md", "image p.png", "html-element y.md", "html-element q.png",
                                "autolink https://e.org", "link bad.md"}));
    // Unset, the destinations are as written.
    QVERIFY(md::toHtml(document).contains(QStringLiteral("<a href=\"x.md\">a</a>")));
}

void commandsNeedNoDisplay()
{
    QVERIFY2(QFileInfo(parchBinary).isExecutable(), "build parch with bin/build first");
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("clean.md"), QStringLiteral("# Clean\n\nText.\n"));
    box.write(QStringLiteral("bad.md"), QStringLiteral("A [ref][nothing].\n"));
    box.write(QStringLiteral("partial.md"), QStringLiteral("not-there.md\n"));
    QDir().mkpath(box.home() + QStringLiteral("/.claude"));
    struct Case {
        QStringList arguments;
        int code;
        const char *said;
    };
    const QList<Case> cases{
        {{"check", "clean.md"}, 0, "clean.md: nothing wrong"},
        {{"check", "bad.md", "--json"}, 2, "\"unresolved-reference\""},
        {{"check", "missing.md"}, 3, "does not exist"},
        {{"check"}, 1, "parch help check"},
        {{"render", "clean.md", "--to", "text"}, 0, "Clean"},
        {{"render", "clean.md", "--to", "json-tree"}, 0, "\"heading\""},
        {{"render", "clean.md", "--to", "outline"}, 0, "Clean"},
        {{"export", "clean.md", "-o", "clean.html"}, 0, "clean.html"},
        {{"export", "clean.md", "-o", "clean.html"}, 3, "--force"},
        {{"export", "clean.md", "--to", "md", "-o", "assembled.md"}, 0, "assembled.md"},
        {{"export", "partial.md", "-o", "partial.html"}, 2, "not-there.md"},
        {{"export", "clean.md", "-o", "clean.pdf"}, 0, "clean.pdf"},
        {{"export", "clean.md", "-o", "clean.docx"}, 0, "clean.docx"},
        {{"export", "clean.md", "-o", "clean.epub"}, 0, "clean.epub"},
        {{"stats", "clean.md", "--json"}, 0, "\"words\": 2"},
        {{"skill"}, 0, "name: parch"},
        {{"skill", "install"}, 0, "Linked "},
        {{"help"}, 0, "Commands that need no display"},
        {{"help", "render"}, 0, "Usage: parch render"},
        {{"--help"}, 0, "check FILE..."},
        {{"--version"}, 0, "parch " PARCH_VERSION},
    };
    for (const Case &each : cases) {
        const Ran ran = runProcess(box, each.arguments);
        const QString context = each.arguments.join(u' ') + u'\n' + ran.said();
        QVERIFY2(ran.code == each.code, qPrintable(context));
        QVERIFY2((ran.out + ran.err).contains(QLatin1StringView(each.said)), qPrintable(context));
        QVERIFY2(!ran.err.contains(QStringLiteral("platform plugin")), qPrintable(context));
    }
    QVERIFY(box.read(QStringLiteral("clean.html")).startsWith("<!DOCTYPE html>"));
    QCOMPARE(readFile(box.home() + QStringLiteral("/.claude/skills/parch/SKILL.md")).left(16), QByteArray("---\nname: parch\n"));
}

void bibliographyLinksAreOnlyWebMailOrDoi()
{
    Sandbox box;
    QVERIFY(box.isValid());
    // A link a bibliography gives, as its URL, its DOI or in the markup of
    // a title or a note, that would run as a script.
    box.write(QStringLiteral("refs.bib"),
              QStringLiteral("@misc{k, title={T <a href=\"javascript:alert(2)\">click</a>}, author={Doe, Jane}, year={2020},"
                             " url={javascript:alert(document.domain)}, doi={javascript:alert(3)}}\n"
                             "@misc{j, title={U}, author={Roe, R}, year={2021}, url={  JavaScript:alert(1)},"
                             " note={<a href=\"jav&#x61;script:alert(4)\">n</a>}}\n"
                             "@misc{w, title={Web}, author={Poe, P}, year={2019}, url={https://example.com/w}}\n"
                             "@misc{z, title={Zoom}, author={Zoe, Z}, year={2018}, url={zoommtg://zoom.us/join}}\n"
                             "@misc{d, title={Doi}, author={Dee, D}, year={2017}, url={doi:10.1000/182}}\n"));
    box.write(QStringLiteral("refs.json"),
              QStringLiteral("[{\"id\":\"m\",\"type\":\"webpage\",\"title\":\"<a href=\\\"javascript:alert(5)\\\">x</a> "
                             "<a href=\\\"mailto:a@example.com\\\">mail</a>\",\"URL\":\"javascript:alert(7)\","
                             "\"author\":[{\"family\":\"A\"}],\"issued\":{\"date-parts\":[[2020]]}}]"));
    box.write(QStringLiteral("doc.md"), QStringLiteral("---\nbibliography: [refs.bib, refs.json]\ncsl: apa\n---\n\n"
                                                       "A [@k] and [@j] and [@m] and [@w] and [@z] and [@d].\n"));
    // Only web, mail and DOI addresses are links; everything else is text.
    const auto linksOf = [](const QString &html) {
        QStringList links;
        static const QRegularExpression href(QStringLiteral("<a [^>]*href=\"([^\"]*)\""));
        for (const QRegularExpressionMatch &match : href.globalMatch(html)) links << match.captured(1);
        return links;
    };
    const auto check = [&](const QString &html, const QString &where) {
        for (const QString &link : linksOf(html))
            QVERIFY2(link.startsWith(u'#') || link.startsWith(QStringLiteral("https://")) || link.startsWith(QStringLiteral("http://"))
                         || link.startsWith(QStringLiteral("mailto:")),
                     qPrintable(where + QStringLiteral(": ") + link));
        QVERIFY2(linksOf(html).contains(QStringLiteral("https://example.com/w")), qPrintable(where));
        QVERIFY2(linksOf(html).contains(QStringLiteral("https://doi.org/10.1000/182")), qPrintable(where + u'\n' + html));
        QVERIFY2(linksOf(html).contains(QStringLiteral("mailto:a@example.com")), qPrintable(where));
        // A refused link keeps its text.
        QVERIFY2(html.contains(QStringLiteral("zoommtg://zoom.us/join")), qPrintable(where));
        QVERIFY2(!linksOf(html).contains(QStringLiteral("zoommtg://zoom.us/join")), qPrintable(where));
    };
    const Ran rendered = box.run({"render", "doc.md", "--to", "html"});
    QVERIFY2(rendered.code == 0, qPrintable(rendered.said()));
    check(rendered.out, QStringLiteral("render"));
    const Ran html = box.run({"export", "doc.md", "-o", "out.html"});
    QVERIFY2(html.code == 0, qPrintable(html.said()));
    check(QString::fromUtf8(box.read(QStringLiteral("out.html"))), QStringLiteral("HTML export"));
    const Ran epub = box.run({"export", "doc.md", "-o", "out.epub"});
    QVERIFY2(epub.code == 0, qPrintable(epub.said()));
    QList<ZipPackage::Entry> entries;
    QVERIFY(ZipPackage::read(box.read(QStringLiteral("out.epub")), &entries));
    QString text;
    for (const ZipPackage::Entry &entry : entries)
        if (entry.name.endsWith(QStringLiteral(".xhtml")) && !entry.name.endsWith(QStringLiteral("nav.xhtml")))
            text += QString::fromUtf8(entry.data);
    QVERIFY(!text.isEmpty());
    check(text, QStringLiteral("EPUB export"));
}

void exportNeverWritesThroughALink()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("repo/notes.md"), QStringLiteral("# Notes\n\n```sh\ncurl -s https://example.invalid/x | sh\n```\n"));
    box.write(QStringLiteral("victim_rc"), QStringLiteral("kept\n"));
    // A link planted where an export would go, to a file that is there, and
    // one to a file that is not.
    const QStringList outputs{QStringLiteral("notes.html"), QStringLiteral("notes.pdf"), QStringLiteral("notes.docx"),
                              QStringLiteral("notes.epub"), QStringLiteral("export.md")};
    for (const QString &name : outputs)
        QVERIFY(QFile::link(QStringLiteral("../victim_rc"), box.path(QStringLiteral("repo/") + name)));
    QVERIFY(QFile::link(QStringLiteral("../absent_rc"), box.path(QStringLiteral("repo/dangling.html"))));
    QVERIFY(QFile::link(QStringLiteral("../victim_rc"), box.path(QStringLiteral("repo/rendered.html"))));
    const auto stillLinks = [&] {
        for (const QString &name : outputs + QStringList{QStringLiteral("dangling.html"), QStringLiteral("rendered.html")})
            if (!QFileInfo(box.path(QStringLiteral("repo/") + name)).isSymLink()) return false;
        return true;
    };
    // Refused, even with --force, saying where the link leads; neither the
    // link nor what it leads to is touched.
    for (const QString &name : outputs + QStringList{QStringLiteral("dangling.html")}) {
        for (const bool force : {false, true}) {
            QStringList arguments{QStringLiteral("export"), QStringLiteral("repo/notes.md"), QStringLiteral("-o"),
                                  QStringLiteral("repo/") + name};
            if (force) arguments << QStringLiteral("--force");
            const Ran ran = box.run(arguments);
            QVERIFY2(ran.code == 3, qPrintable(ran.said()));
            QVERIFY2(ran.err.contains(QStringLiteral("is a link to")), qPrintable(ran.said()));
            QVERIFY2(ran.err.contains(QStringLiteral("never writes through a link")), qPrintable(ran.said()));
        }
    }
    const Ran rendered = box.run({"render", "repo/notes.md", "--to", "html", "-o", "repo/rendered.html", "--force"});
    QVERIFY2(rendered.code == 3 && rendered.err.contains(QStringLiteral("is a link to")), qPrintable(rendered.said()));
    QCOMPARE(box.read(QStringLiteral("victim_rc")), QByteArray("kept\n"));
    QVERIFY(!QFileInfo::exists(box.path(QStringLiteral("absent_rc"))));
    QVERIFY(stillLinks());

    // A folder of pictures beside the page that is a link is not replaced
    // either, and what it leads to keeps what it holds, export folder or not.
    box.write(QStringLiteral("repo/pic.md"), QStringLiteral("![p](p.png)\n"));
    QImage picture(4, 4, QImage::Format_RGB32);
    picture.fill(Qt::blue);
    QVERIFY(picture.save(box.path(QStringLiteral("repo/p.png"))));
    box.write(QStringLiteral("elsewhere/.parch-export"), QStringLiteral("parch\n"));
    box.write(QStringLiteral("elsewhere/keep.txt"), QStringLiteral("keep\n"));
    QVERIFY(QFile::link(QStringLiteral("../elsewhere"), box.path(QStringLiteral("repo/pic_files"))));
    const Ran folder = box.run({"export", "repo/pic.md", "-o", "repo/pic.html", "--force"});
    QVERIFY2(folder.code == 3 && folder.err.contains(QStringLiteral("is a link to")), qPrintable(folder.said()));
    QCOMPARE(box.read(QStringLiteral("elsewhere/keep.txt")), QByteArray("keep\n"));
    QVERIFY(QFileInfo(box.path(QStringLiteral("repo/pic_files"))).isSymLink());
    QVERIFY(!QFileInfo::exists(box.path(QStringLiteral("repo/pic.html"))));
    const Ran markdown = box.run({"export", "repo/pic.md", "--to", "md", "-o", "repo/pic.markdown", "--images", "copy"});
    QVERIFY2(markdown.code == 3 && markdown.err.contains(QStringLiteral("is a link to")), qPrintable(markdown.said()));
    QCOMPARE(box.read(QStringLiteral("elsewhere/keep.txt")), QByteArray("keep\n"));
    QVERIFY(!QFileInfo::exists(box.path(QStringLiteral("repo/pic.markdown"))));

    // What writes an export replaces a link only when asked to, and then
    // the link itself, never what it leads to.
    const QString link = box.path(QStringLiteral("repo/notes.html"));
    QString error;
    QVERIFY(!Exporters::writeFile(link, "page", &error));
    QVERIFY2(error.contains(QStringLiteral("is a link to")) && error.contains(box.path(QStringLiteral("victim_rc"))),
             qPrintable(error));
    QVERIFY(Exporters::writeFile(link, "page", &error, ExistingOutput::ReplaceLink));
    QVERIFY(!QFileInfo(link).isSymLink());
    QCOMPARE(box.read(QStringLiteral("repo/notes.html")), QByteArray("page"));
    QCOMPARE(box.read(QStringLiteral("victim_rc")), QByteArray("kept\n"));
    // A file replaced keeps its permissions, and a new one has the usual.
    const QString plain = box.path(QStringLiteral("repo/plain.html"));
    QVERIFY(Exporters::writeFile(plain, "one", &error));
    QVERIFY(QFile::setPermissions(plain, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup));
    QVERIFY(Exporters::writeFile(plain, "two", &error));
    QCOMPARE(box.read(QStringLiteral("repo/plain.html")), QByteArray("two"));
    struct stat status;
    QVERIFY(::stat(QFile::encodeName(plain).constData(), &status) == 0);
    QCOMPARE(status.st_mode & 07777, mode_t(0640));
    // Nothing is left behind beside it.
    QStringList left = QDir(box.path(QStringLiteral("repo"))).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    left.removeIf([](const QString &name) { return !name.contains(QStringLiteral("parch-")); });
    QVERIFY2(left.isEmpty(), qPrintable(left.join(u' ')));
}

void checkSaysWhetherHelpersAreIsolated()
{
    Sandbox box;
    QVERIFY(box.isValid());
    box.write(QStringLiteral("doc.md"), QStringLiteral("# Doc\n\nWords.\n"));
    const Ran ran = box.run({"check", "doc.md", "--json"});
    QVERIFY2(ran.code == 0, qPrintable(ran.said()));
    const QJsonValue isolated = ran.json().value(QStringLiteral("helpers")).toObject().value(QStringLiteral("isolated"));
    QVERIFY(isolated.isBool());
    QCOMPARE(isolated.toBool(), HelperProcess::canIsolate());
    // Where the kernel allows no user namespaces, it says so.
    HelperProcess::setIsolationProbe([] { return false; });
    const auto restore = qScopeGuard([] { HelperProcess::setIsolationProbe({}); });
    const Ran refused = box.run({"check", "doc.md", "--json"});
    QCOMPARE(refused.json().value(QStringLiteral("helpers")).toObject().value(QStringLiteral("isolated")), QJsonValue(false));
}

} // namespace CliTests
