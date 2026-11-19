#include "clicommands.h"

#include "citations.h"
#include "cli.h"
#include "dialectchoice.h"
#include "documentcheck.h"
#include "documentsettings.h"
#include "exporter.h"
#include "htmlexport.h"
#include "mdhtml.h"
#include "rendercheck.h"
#include "renderservice.h"
#include "treejson.h"
#include "wordcounter.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <memory>
#include <optional>

namespace Cli {

QString printable(const QString &text)
{
    QString safe = text;
    for (QChar &c : safe)
        if (c.category() == QChar::Other_Control && c != u'\n' && c != u'\t' && c != u'\r')
            c = QChar(QChar::ReplacementCharacter);
    return safe;
}

namespace {

const QString version = QStringLiteral(PARCH_VERSION);

// JSON with the control characters QJsonDocument leaves as they are (delete
// and the C1 controls, inside strings only) escaped too.
QByteArray printableJson(const QJsonDocument &document)
{
    const QString text = QString::fromUtf8(document.toJson(QJsonDocument::Indented));
    QString safe;
    safe.reserve(text.size());
    for (const QChar c : text) {
        if (c.unicode() >= 0x7f && c.unicode() <= 0x9f)
            safe += QStringLiteral("\\u%1").arg(int(c.unicode()), 4, 16, QChar(u'0'));
        else
            safe += c;
    }
    return safe.toUtf8();
}

void write(QIODevice *device, const QString &text)
{
    device->write(printable(text).toUtf8());
}
void line(QIODevice *device, const QString &text)
{
    write(device, text + u'\n');
}
void json(QIODevice *device, const QJsonObject &object)
{
    device->write(printableJson(QJsonDocument(object)));
}

// What every JSON result starts with.
QJsonObject header(const QString &command)
{
    return QJsonObject{{QStringLiteral("parch"), version}, {QStringLiteral("command"), command}};
}

QJsonObject renderers(std::initializer_list<const char *> names)
{
    QJsonObject used;
    for (const char *name : names) used.insert(QLatin1StringView(name), QStringLiteral("parch ") + version);
    return used;
}

// A path as people read it: from the working folder when it is beneath it.
QString shown(const QString &path, const Context &context)
{
    const QString base = context.workingDirectory.endsWith(u'/') ? context.workingDirectory
                                                                  : context.workingDirectory + u'/';
    return path.startsWith(base) ? path.mid(base.size()) : path;
}

QString absolute(const QString &path, const Context &context)
{
    return absoluteFiles({path}, context.workingDirectory).value(0);
}

// One command's options and help.
struct Command {
    QString name;
    QString synopsis;
    QString description;
    QList<QCommandLineOption> options;
    QString notes;
    QCommandLineParser parser;

    Command(QString commandName, QString commandSynopsis, QString commandDescription)
        : name(std::move(commandName)), synopsis(std::move(commandSynopsis)),
          description(std::move(commandDescription))
    {
    }

    void add(const QCommandLineOption &option) { options.append(option); }

    QString help() const
    {
        QString text = QStringLiteral("Usage: parch %1 %2\n\n%3\n").arg(name, synopsis, description);
        QList<std::pair<QString, QString>> rows;
        for (const QCommandLineOption &option : options) {
            QStringList names;
            for (const QString &each : option.names())
                names.append(each.size() == 1 ? u'-' + each : QStringLiteral("--") + each);
            QString left = names.join(QStringLiteral(", "));
            if (!option.valueName().isEmpty()) left += u' ' + option.valueName();
            rows.append({left, option.description()});
        }
        rows.append({QStringLiteral("-h, --help"), QStringLiteral("Print this help")});
        qsizetype width = 0;
        for (const auto &[left, right] : rows) width = std::max(width, left.size());
        text += QStringLiteral("\nOptions:\n");
        for (const auto &[left, right] : rows) text += QStringLiteral("  %1  %2\n").arg(left, -int(width)).arg(right);
        if (!notes.isEmpty()) text += u'\n' + notes + u'\n';
        return text;
    }

    // Parses the arguments after the command. Prints the help, or what is
    // wrong, and gives the exit code when the command should go no further.
    std::optional<int> parse(const QStringList &arguments, const Context &context)
    {
        parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
        parser.addOptions(options);
        parser.addOption({{QStringLiteral("h"), QStringLiteral("help")}, QString()});
        // The command word stands where QCommandLineParser expects the program.
        if (!parser.parse(arguments.mid(1))) return usage(parser.errorText(), context);
        if (parser.isSet(QStringLiteral("help"))) {
            write(context.out, help());
            return Ok;
        }
        return std::nullopt;
    }

    int usage(const QString &problem, const Context &context) const
    {
        line(context.err, QStringLiteral("parch %1: %2").arg(name, problem));
        line(context.err, QStringLiteral("Run parch help %1 for its options.").arg(name));
        return Usage;
    }

    int fail(int code, const QString &problem, const Context &context) const
    {
        line(context.err, QStringLiteral("parch %1: %2").arg(name, problem));
        return code;
    }

    bool wantsJson() const { return parser.isSet(QStringLiteral("json")); }
};

const QString jsonDescription = QStringLiteral("Print the result as JSON");

QStringList bundledStyleNames()
{
    QStringList names;
    for (const CslCatalog::StyleInfo &style : CslCatalog::bundledStyles()) names.append(style.id);
    return names;
}

// The options that choose the dialect, shared by the commands that read a document.
void addDialectOptions(Command &command)
{
    command.add({QStringLiteral("profile"), QStringLiteral("writer or standard (the default is writer)"),
                 QStringLiteral("NAME")});
    command.add({QStringLiteral("extension"), QStringLiteral("Turn an extension on; give it again for more"),
                 QStringLiteral("NAME")});
    command.add({QStringLiteral("no-extension"), QStringLiteral("Turn an extension off; give it again for more"),
                 QStringLiteral("NAME")});
    command.add({QStringLiteral("library"), QStringLiteral("The library folder that /rooted includes are found in"),
                 QStringLiteral("DIR")});
    command.add({QStringLiteral("bibliography"),
                 QStringLiteral("A BibTeX, BibLaTeX, CSL JSON or CSL YAML file of works to cite; give it again "
                                "for more"),
                 QStringLiteral("FILE")});
    command.add({QStringLiteral("csl"),
                 QStringLiteral("The citation style: a bundled one (%1) or a .csl file")
                     .arg(bundledStyleNames().join(QStringLiteral(", "))),
                 QStringLiteral("STYLE")});
    command.add({QStringLiteral("max-include-depth"),
                 QStringLiteral("How deep includes may nest (the default is %1)").arg(md::Source().maxIncludeDepth),
                 QStringLiteral("N")});
    command.add({QStringLiteral("max-include-size"),
                 QStringLiteral("How much text the document and its includes may hold together, in bytes or with "
                                "K, M or G (the default is %1)")
                     .arg(QStringLiteral("%1M").arg(md::Source().maxExpandedBytes / (1024 * 1024))),
                 QStringLiteral("SIZE")});
    command.add({QStringLiteral("max-includes"),
                 QStringLiteral("How many includes are read in all (the default is %1)").arg(md::Source().maxIncludeCount),
                 QStringLiteral("N")});
    command.add({QStringLiteral("use-saved-settings"),
                 QStringLiteral("Read the document in the settings parch keeps for it, its library and every "
                                "document")});
}

QString dialectNotes()
{
    // The extensions, a few to a line.
    QString names, line = QStringLiteral("Extensions:");
    for (const QString &name : md::Options::extensionNames()) {
        if (line.size() + name.size() + 2 > 78) {
            names += line + QStringLiteral(",\n");
            line = QStringLiteral("  ") + name;
        } else {
            line += (line.endsWith(u':') ? QStringLiteral(" ") : QStringLiteral(", ")) + name;
        }
    }
    names += line;
    return QStringLiteral(
               "The dialect is the Writer profile with all of its extensions unless the options say\n"
               "otherwise. With --use-saved-settings, the settings parch keeps for the document, its\n"
               "library and every document apply where the options say nothing. Nothing the window was\n"
               "last set to is read. The front matter's bibliography: and csl: choose the bibliography\n"
               "and the citation style unless the options or the document's saved settings do.\n") +
           names;
}

struct DialectFlags {
    Dialect::Layer layer;
    bool useSaved = false;
};

std::optional<int> readDialectFlags(Command &command, const Context &context, DialectFlags *flags)
{
    const QCommandLineParser &parser = command.parser;
    if (parser.isSet(QStringLiteral("profile"))) {
        const QString name = parser.value(QStringLiteral("profile"));
        flags->layer.profile = Dialect::profileNamed(name);
        if (!flags->layer.profile)
            return command.usage(QStringLiteral("--profile takes %1, not \"%2\"")
                                     .arg(Dialect::profileNames().join(QStringLiteral(" or ")), name),
                                 context);
    }
    const QStringList known = md::Options::extensionNames();
    const QStringList on = parser.values(QStringLiteral("extension"));
    const QStringList off = parser.values(QStringLiteral("no-extension"));
    for (const QString &name : on + off)
        if (!known.contains(name))
            return command.usage(QStringLiteral("there is no extension \"%1\"; the extensions are %2")
                                     .arg(name, known.join(QStringLiteral(", "))),
                                 context);
    for (const QString &name : on)
        if (off.contains(name))
            return command.usage(QStringLiteral("--extension %1 and --no-extension %1 cannot both be given").arg(name),
                                 context);
    for (const QString &name : off) flags->layer.extensions.append({name, false});
    for (const QString &name : on) flags->layer.extensions.append({name, true});
    if (parser.isSet(QStringLiteral("library"))) {
        const QString folder = absolute(parser.value(QStringLiteral("library")), context);
        if (!QFileInfo(folder).isDir())
            return command.usage(QStringLiteral("there is no folder %1 for --library").arg(folder), context);
        // Includes are found by canonical paths.
        flags->layer.libraryRoot = QFileInfo(folder).canonicalFilePath();
    }
    if (parser.isSet(QStringLiteral("bibliography"))) {
        QStringList files;
        for (const QString &file : parser.values(QStringLiteral("bibliography"))) {
            const QString path = absolute(file, context);
            if (!QFileInfo(path).isFile())
                return command.usage(QStringLiteral("there is no file %1 for --bibliography").arg(path), context);
            files.append(path);
        }
        flags->layer.bibliography = files;
    }
    if (parser.isSet(QStringLiteral("csl"))) {
        const QString written = parser.value(QStringLiteral("csl"));
        const QString style = Dialect::styleReference(written, context.workingDirectory);
        if (Dialect::isStyleFile(style) ? !QFileInfo(style).isFile() : CslCatalog::bundledId(style).isEmpty())
            return command.usage(QStringLiteral("--csl takes a .csl file or one of %1, not \"%2\"")
                                     .arg(bundledStyleNames().join(QStringLiteral(", ")), written),
                                 context);
        flags->layer.citationStyle = style;
    }
    // The include limits: whole numbers from 1, sizes with an optional unit.
    const auto count = [&](const QString &name, int most, std::optional<int> *into) -> std::optional<int> {
        if (!parser.isSet(name)) return std::nullopt;
        const QString written = parser.value(name);
        bool ok = false;
        const int value = written.trimmed().toInt(&ok);
        if (!ok || value < 1 || value > most)
            return command.usage(QStringLiteral("--%1 takes a whole number from 1 to %2, not \"%3\"")
                                     .arg(name).arg(most).arg(written),
                                 context);
        *into = value;
        return std::nullopt;
    };
    if (const auto done = count(QStringLiteral("max-include-depth"), Dialect::MaxIncludeDepthLimit,
                                &flags->layer.maxIncludeDepth))
        return done;
    if (const auto done = count(QStringLiteral("max-includes"), Dialect::MaxIncludesLimit, &flags->layer.maxIncludes))
        return done;
    if (parser.isSet(QStringLiteral("max-include-size"))) {
        const QString written = parser.value(QStringLiteral("max-include-size"));
        const std::optional<qint64> size = Dialect::sizeNamed(written);
        if (!size || *size > Dialect::MaxIncludeSizeLimit)
            return command.usage(QStringLiteral("--max-include-size takes a size from 1 byte to %1, such as 512K or "
                                                "20M, not \"%2\"")
                                     .arg(Dialect::sizeText(Dialect::MaxIncludeSizeLimit), written),
                                 context);
        flags->layer.maxIncludeSize = size;
    }
    flags->useSaved = parser.isSet(QStringLiteral("use-saved-settings"));
    return std::nullopt;
}

// The layers of the dialect for one document: the flags, then what is kept
// for it when asked for (its own, its library's, every document's); its
// front matter joins them as it is read. False, with why, when what is kept
// cannot be read.
bool dialectLayers(const QString &path, const DialectFlags &flags, const Context &context, Dialect::Layers *layers,
                   QString *error)
{
    *layers = {};
    layers->flags = flags.layer;
    if (!flags.useSaved) return true;
    DocumentSettings::Saved saved;
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (!DocumentSettings::read(context.paths, canonical.isEmpty() ? path : canonical, &saved, error)) return false;
    layers->document = saved.document;
    layers->library = saved.library;
    layers->application = saved.application;
    return true;
}

// Reads one document in its dialect, saying why not when it cannot be.
bool loadDocument(const QString &path, const DialectFlags &flags, const Context &context,
                  DocumentCheck::Loaded *loaded, QString *error)
{
    Dialect::Layers layers;
    if (!dialectLayers(path, flags, context, &layers, error)) return false;
    return DocumentCheck::load(path, layers, loaded, error);
}

// Whether a command may write `output`: never one of the document's own
// files, never through a symbolic link, even with --force, and a file that
// is there only with --force.
bool mayWrite(const QString &output, bool force, const md::Document &document, const Dialect::Choice &dialect,
              QString *error)
{
    const QFileInfo info(output);
    if (Exporters::isSource(document, dialect, output)) {
        *error = QStringLiteral("%1 is the document or a file it is made from (one it includes, its bibliography or "
                                "style, or a picture it shows); parch never writes over it")
                     .arg(output);
        return false;
    }
    if (const QString why = Exporters::linkRefusal(output); !why.isEmpty()) {
        *error = why + QStringLiteral(", even with --force: export to another name, or remove the link");
        return false;
    }
    if (info.isDir()) {
        *error = QStringLiteral("%1 is a folder").arg(output);
        return false;
    }
    if (!QFileInfo(info.absolutePath()).isDir()) {
        *error = QStringLiteral("there is no folder %1").arg(info.absolutePath());
        return false;
    }
    if (info.exists() && !force) {
        *error = QStringLiteral("%1 is already there; give --force to replace it").arg(output);
        return false;
    }
    return true;
}

// The file a problem is in: one of the document's, or a bibliography or style.
QString problemFile(const DocumentCheck::Problem &problem, const md::Document &document)
{
    if (problem.file < 0) return problem.path;
    return document.files[size_t(problem.file)].path;
}

// What the reader of an export misses: images that are not there, and
// citations, styles and bibliographies that could not be formatted.
bool missedByReader(const DocumentCheck::Problem &problem)
{
    if (problem.kind == u"asset") return true;
    return (problem.kind == u"citation" || problem.kind == u"bibliography") && problem.severity != md::Severity::Info;
}

QJsonObject problemJson(const DocumentCheck::Problem &problem, const md::Document &document)
{
    const QString path = problemFile(problem, document);
    return QJsonObject{
        {QStringLiteral("file"), path.isEmpty() ? QJsonValue() : QJsonValue(path)},
        {QStringLiteral("line"), problem.position.line},
        {QStringLiteral("column"), problem.position.column},
        {QStringLiteral("range"), problem.file < 0 ? QJsonValue() : QJsonValue(QJsonArray{problem.range.start, problem.range.end})},
        {QStringLiteral("severity"), QString::fromLatin1(md::severityName(problem.severity))},
        {QStringLiteral("code"), problem.code},
        {QStringLiteral("kind"), problem.kind},
        {QStringLiteral("message"), problem.message},
    };
}

QString problemLine(const DocumentCheck::Problem &problem, const md::Document &document, const Context &context)
{
    const QString file = shown(problemFile(problem, document), context);
    const QString severity = QString::fromLatin1(md::severityName(problem.severity));
    // A bibliography's problem may have no line.
    if (problem.position.line <= 0)
        return QStringLiteral("%1: %2: %3 [%4]").arg(file, severity, problem.message, problem.code);
    return QStringLiteral("%1:%2:%3: %4: %5 [%6]")
        .arg(file)
        .arg(problem.position.line)
        .arg(problem.position.column)
        .arg(severity, problem.message, problem.code);
}

QString plural(int count, const char *one, const char *many)
{
    return QStringLiteral("%1 %2").arg(count).arg(QLatin1StringView(count == 1 ? one : many));
}

// The engine's own diagnostics, with their lines, for the semantic tree.
QJsonArray diagnosticsJson(const md::Document &document)
{
    QJsonArray list;
    std::vector<std::unique_ptr<DocumentCheck::LineIndex>> lines(document.files.size());
    for (const md::Diagnostic &diagnostic : document.diagnostics) {
        auto &index = lines[size_t(diagnostic.file)];
        if (!index) index = std::make_unique<DocumentCheck::LineIndex>(document.files[size_t(diagnostic.file)].text);
        const DocumentCheck::Position at = index->at(diagnostic.range.start);
        list.append(QJsonObject{
            {QStringLiteral("file"), diagnostic.file},
            {QStringLiteral("line"), at.line},
            {QStringLiteral("column"), at.column},
            {QStringLiteral("range"), QJsonArray{diagnostic.range.start, diagnostic.range.end}},
            {QStringLiteral("severity"), QString::fromLatin1(md::severityName(diagnostic.severity))},
            {QStringLiteral("code"), QString::fromLatin1(md::codeName(diagnostic.code))},
            {QStringLiteral("message"), diagnostic.message},
        });
    }
    return list;
}

int check(const QStringList &arguments, const Context &context)
{
    Command command(QStringLiteral("check"), QStringLiteral("FILE... [options]"),
                    QStringLiteral("Read Markdown documents, with the files they include, and report what is wrong:\n"
                                   "includes that cannot be read, references, footnotes, citations and variables\n"
                                   "that lead nowhere, images whose files are missing, links to files or headings\n"
                                   "that are not there, malformed table rows, unclosed math, formulas and diagrams\n"
                                   "that cannot be drawn, front matter, and HTML that is shown as text. Each\n"
                                   "problem has its file, line and column."));
    addDialectOptions(command);
    command.add({QStringLiteral("no-render"), QStringLiteral("Do not draw formulas and diagrams to check them")});
    command.add({QStringLiteral("json"), jsonDescription});
    command.notes = dialectNotes() +
                    QStringLiteral("\n\nFormulas are drawn with render-svg (RaTeX) and diagrams with merman-cli (Merman),\n"
                                   "as the preview draws them. When one cannot be found, or with --no-render, what it\n"
                                   "draws is not checked, and check says so without counting it as a problem.\n\n"
                                   "Exit status: 0 nothing wrong (notes of severity info alone are not problems),\n"
                                   "1 a wrong command line, 2 problems found, 3 a document could not be read.");
    if (const auto done = command.parse(arguments, context)) return *done;
    const QStringList files = command.parser.positionalArguments();
    if (files.isEmpty()) return command.usage(QStringLiteral("say which files: parch check FILE..."), context);
    DialectFlags flags;
    if (const auto done = readDialectFlags(command, context, &flags)) return *done;

    // Formulas and diagrams are drawn as the preview draws them, by a
    // service made the first time a document has one.
    const HelperSet helpers = context.helpers ? *context.helpers : HelperSet::find();
    const bool drawing = !command.parser.isSet(QStringLiteral("no-render"));
    std::unique_ptr<RenderService> service;
    int skippedFormulas = 0, skippedDiagrams = 0;
    QJsonArray documents;
    bool anyProblem = false, anyUnreadable = false;
    for (const QString &file : files) {
        const QString path = absolute(file, context);
        DocumentCheck::Loaded loaded;
        QString error;
        if (!loadDocument(path, flags, context, &loaded, &error)) {
            anyUnreadable = true;
            documents.append(QJsonObject{{QStringLiteral("file"), path},
                                         {QStringLiteral("ok"), false},
                                         {QStringLiteral("error"), error}});
            if (!command.wantsJson()) line(context.err, QStringLiteral("parch check: %1").arg(error));
            continue;
        }
        std::vector<RenderCheck::Item> drawn;
        for (RenderCheck::Item &item : RenderCheck::items(loaded.document)) {
            if (drawing && RenderCheck::canDraw(helpers, item.kind)) drawn.push_back(std::move(item));
            else ++(item.isMath() ? skippedFormulas : skippedDiagrams);
        }
        std::vector<DocumentCheck::Problem> drawingProblems;
        if (!drawn.empty()) {
            if (!service) {
                RenderService::Options options;
                options.helpers = helpers;
                options.findHelpers = false;
                options.cacheFolder = context.renderCache;
                service = std::make_unique<RenderService>(options);
            }
            drawingProblems = RenderCheck::wait(*service, drawn);
        }
        const std::vector<DocumentCheck::Problem> problems = DocumentCheck::problems(loaded, std::move(drawingProblems));
        const bool problemsFound = DocumentCheck::anyProblem(problems);
        anyProblem = anyProblem || problemsFound;
        int counts[3] = {0, 0, 0};
        QJsonArray list;
        for (const DocumentCheck::Problem &problem : problems) {
            ++counts[int(problem.severity)];
            list.append(problemJson(problem, loaded.document));
        }
        documents.append(QJsonObject{
            {QStringLiteral("file"), loaded.path},
            {QStringLiteral("ok"), !problemsFound},
            {QStringLiteral("incomplete"), loaded.document.incomplete},
            {QStringLiteral("dialect"), loaded.dialect.toJson()},
            {QStringLiteral("files"), TreeJson::files(loaded.document)},
            {QStringLiteral("counts"), QJsonObject{{QStringLiteral("error"), counts[int(md::Severity::Error)]},
                                                   {QStringLiteral("warning"), counts[int(md::Severity::Warning)]},
                                                   {QStringLiteral("info"), counts[int(md::Severity::Info)]}}},
            {QStringLiteral("problems"), list},
        });
        if (command.wantsJson()) continue;
        for (const DocumentCheck::Problem &problem : problems)
            line(context.out, problemLine(problem, loaded.document, context));
        QStringList said;
        if (counts[int(md::Severity::Error)]) said << plural(counts[int(md::Severity::Error)], "error", "errors");
        if (counts[int(md::Severity::Warning)])
            said << plural(counts[int(md::Severity::Warning)], "warning", "warnings");
        if (counts[int(md::Severity::Info)]) said << plural(counts[int(md::Severity::Info)], "note", "notes");
        line(context.out, QStringLiteral("%1: %2 (%3)")
                              .arg(shown(path, context),
                                   said.isEmpty() ? QStringLiteral("nothing wrong") : said.join(QStringLiteral(", ")),
                                   loaded.dialect.describe()));
    }
    const int code = anyUnreadable ? Io : anyProblem ? Problems : Ok;
    if (command.wantsJson()) {
        QJsonObject result = header(QStringLiteral("check"));
        result.insert(QStringLiteral("ok"), code == Ok);
        QJsonObject used = renderers({"markdown"});
        QJsonObject helperPaths;
        if (!helpers.ratex.isEmpty()) {
            used.insert(QStringLiteral("math"), helpers.ratexVersion());
            helperPaths.insert(QStringLiteral("render-svg"), helpers.ratex);
        }
        if (!helpers.merman.isEmpty()) {
            used.insert(QStringLiteral("diagram"), helpers.mermanVersion());
            helperPaths.insert(QStringLiteral("merman-cli"), helpers.merman);
        }
        // Whether the helpers run cut off from the network, as the kernel allows.
        helperPaths.insert(QStringLiteral("isolated"), HelperProcess::canIsolate());
        result.insert(QStringLiteral("renderers"), used);
        result.insert(QStringLiteral("helpers"), helperPaths);
        // What the documents hold that could not be checked, for want of its helper.
        QJsonArray skipped;
        if (skippedFormulas > 0) skipped.append(QStringLiteral("math"));
        if (skippedDiagrams > 0) skipped.append(QStringLiteral("diagram"));
        result.insert(QStringLiteral("skipped"), skipped);
        result.insert(QStringLiteral("documents"), documents);
        json(context.out, result);
    } else if (skippedFormulas > 0 || skippedDiagrams > 0) {
        QStringList what, missing;
        if (skippedFormulas > 0) {
            what << plural(skippedFormulas, "formula", "formulas");
            missing << QStringLiteral("render-svg");
        }
        if (skippedDiagrams > 0) {
            what << plural(skippedDiagrams, "diagram", "diagrams");
            missing << QStringLiteral("merman-cli");
        }
        const bool one = skippedFormulas + skippedDiagrams == 1;
        if (!drawing)
            line(context.err, QStringLiteral("parch check: %1 %2 not checked, as --no-render asks")
                                  .arg(what.join(QStringLiteral(" and ")), one ? QStringLiteral("was") : QStringLiteral("were")));
        else
            line(context.err, QStringLiteral("parch check: %1 %2 not checked, since %3 %4 not found")
                                  .arg(what.join(QStringLiteral(" and ")), one ? QStringLiteral("was") : QStringLiteral("were"),
                                       missing.join(QStringLiteral(" and ")),
                                       missing.size() == 1 ? QStringLiteral("was") : QStringLiteral("were")));
    }
    return code;
}

// An incomplete document can be previewed on stdout, but cannot be published to a file.
int incompleteOutput(const Command &command, const DocumentCheck::Loaded &loaded, const Context &context)
{
    const int code = command.fail(Problems, QStringLiteral("%1 cannot be written: an included file could not be read")
                                                .arg(shown(loaded.path, context)), context);
    for (const auto &problem : DocumentCheck::problems(loaded))
        if (problem.kind == u"include") line(context.err, problemLine(problem, loaded.document, context));
    return code;
}

// Writes a result to -o, or to the standard output.
std::optional<int> deliver(Command &command, const Context &context, const QString &text,
                           const md::Document &document, const Dialect::Choice &dialect, QString *output)
{
    if (!command.parser.isSet(QStringLiteral("output"))) {
        if (!command.wantsJson()) write(context.out, text);
        return std::nullopt;
    }
    *output = absolute(command.parser.value(QStringLiteral("output")), context);
    QString error;
    if (!mayWrite(*output, command.parser.isSet(QStringLiteral("force")), document, dialect, &error))
        return command.fail(Io, error, context);
    const auto existing = command.parser.isSet(QStringLiteral("force")) ? ExistingOutput::ReplaceFile : ExistingOutput::Refuse;
    if (!Exporters::writeFile(*output, text.toUtf8(), &error, existing)) return command.fail(Io, error, context);
    return std::nullopt;
}

int render(const QStringList &arguments, const Context &context)
{
    Command command(QStringLiteral("render"), QStringLiteral("FILE [--to FORMAT] [-o OUT] [options]"),
                    QStringLiteral("Write what a Markdown document means, with the files it includes: its safe HTML,\n"
                                   "its plain text, its outline of headings, or its whole semantic tree as JSON."));
    command.add({QStringLiteral("to"),
                 QStringLiteral("html (the default; a fragment, with no page around it), text, outline, json-tree"),
                 QStringLiteral("FORMAT")});
    command.add({{QStringLiteral("o"), QStringLiteral("output")},
                 QStringLiteral("Write to this file instead of the standard output"),
                 QStringLiteral("OUT")});
    command.add({QStringLiteral("force"), QStringLiteral("Replace OUT if it is there")});
    addDialectOptions(command);
    command.add({QStringLiteral("json"),
                 QStringLiteral("Print a JSON report, with the result in it unless it went to OUT")});
    command.notes = dialectNotes() +
                    QStringLiteral("\n\nRelative links and images in html lead from OUT's folder, or else from the "
                                   "document's,\nwhichever file they are written in. The tree keeps them as written. "
                                   "Ranges in json-tree\nare UTF-16 offsets into each file, start included and end "
                                   "not; lines count from 1.\n\n"
                                   "A render to a file refuses a document with an unreadable include. A render to stdout "
                                   "can preview it.\n\n"
                                   "Exit status: 0 written, 1 a wrong command line, 2 the document is incomplete "
                                   "for file output, 3 a file could not be read or "
                                   "written.");
    if (const auto done = command.parse(arguments, context)) return *done;
    const QStringList files = command.parser.positionalArguments();
    if (files.size() != 1) return command.usage(QStringLiteral("say which one file: parch render FILE"), context);
    const QString to = command.parser.isSet(QStringLiteral("to")) ? command.parser.value(QStringLiteral("to")).toLower()
                                                                   : QStringLiteral("html");
    static const QStringList formats{QStringLiteral("html"), QStringLiteral("text"), QStringLiteral("outline"),
                                     QStringLiteral("json-tree")};
    if (!formats.contains(to))
        return command.usage(QStringLiteral("--to takes %1, not \"%2\"").arg(formats.join(QStringLiteral(", ")), to),
                             context);
    DialectFlags flags;
    if (const auto done = readDialectFlags(command, context, &flags)) return *done;
    DocumentCheck::Loaded loaded;
    QString error;
    if (!loadDocument(absolute(files.first(), context), flags, context, &loaded, &error))
        return command.fail(Io, error, context);
    const md::Document &document = loaded.document;
    if (document.incomplete && command.parser.isSet(QStringLiteral("output")))
        return incompleteOutput(command, loaded, context);

    QString text;
    QJsonValue result;
    if (to == u"html") {
        // Links and images lead from where the HTML goes, or else from the
        // document's folder, whichever file of it they are written in.
        md::HtmlOptions options;
        const QString folder = command.parser.isSet(QStringLiteral("output"))
                                   ? QFileInfo(absolute(command.parser.value(QStringLiteral("output")), context)).absolutePath()
                                   : QFileInfo(loaded.path).absolutePath();
        options.destination = HtmlExporter::destinationsFrom(document, folder);
        text = md::toHtml(document, loaded.htmlOptions(options));
        result = text;
    } else if (to == u"text") {
        text = document.plainText();
        result = text;
    } else if (to == u"outline") {
        for (const md::Heading &heading : document.headings)
            text += QString(2 * (heading.level - 1), u' ') + heading.text + u'\n';
        result = TreeJson::outline(document);
    } else {
        QJsonObject tree = header(QStringLiteral("render"));
        tree.insert(QStringLiteral("dialect"), loaded.dialect.toJson());
        tree.insert(QStringLiteral("renderers"), renderers({"markdown"}));
        tree.insert(QStringLiteral("helpers"), QJsonObject());
        const QJsonObject parts = TreeJson::document(document);
        for (auto it = parts.begin(); it != parts.end(); ++it) tree.insert(it.key(), it.value());
        tree.insert(QStringLiteral("diagnostics"), diagnosticsJson(document));
        text = QString::fromUtf8(printableJson(QJsonDocument(tree)));
        result = tree;
    }

    QString output;
    if (const auto done = deliver(command, context, text, document, loaded.dialect, &output)) return *done;
    if (command.wantsJson()) {
        QJsonObject report = header(QStringLiteral("render"));
        report.insert(QStringLiteral("file"), loaded.path);
        report.insert(QStringLiteral("to"), to);
        report.insert(QStringLiteral("dialect"), loaded.dialect.toJson());
        report.insert(QStringLiteral("renderers"),
                      to == u"html" ? renderers({"markdown", "html"}) : renderers({"markdown"}));
        report.insert(QStringLiteral("helpers"), QJsonObject());
        report.insert(QStringLiteral("incomplete"), document.incomplete);
        report.insert(QStringLiteral("diagnostics"), int(document.diagnostics.size()));
        if (output.isEmpty()) report.insert(QStringLiteral("result"), result);
        else report.insert(QStringLiteral("output"), output);
        json(context.out, report);
    } else if (document.incomplete) {
        line(context.err, QStringLiteral("parch render: %1 includes a file that could not be read; parch check %2 "
                                         "says which")
                              .arg(shown(loaded.path, context), shown(loaded.path, context)));
    }
    return Ok;
}

int exportDocument(const QStringList &arguments, const Context &context)
{
    QStringList formats;
    for (const Exporter *exporter : Exporters::all()) formats.append(exporter->format());
    Command command(QStringLiteral("export"), QStringLiteral("FILE --to FORMAT -o OUT [options]"),
                    QStringLiteral("Write a Markdown document, with the files it includes, as another format, the\n"
                                   "way the window exports it. The document itself is never changed."));
    command.add({QStringLiteral("to"),
                 QStringLiteral("%1 (the default is the one OUT's name ends in)").arg(formats.join(QStringLiteral(", "))),
                 QStringLiteral("FORMAT")});
    command.add({{QStringLiteral("o"), QStringLiteral("output")}, QStringLiteral("The file to write"),
                 QStringLiteral("OUT")});
    command.add({QStringLiteral("force"), QStringLiteral("Replace OUT if it is there")});
    command.add({{QStringLiteral("q"), QStringLiteral("quiet")},
                 QStringLiteral("Print nothing but errors (no file name, no notes)")});
    addDialectOptions(command);
    command.add({QStringLiteral("json"), jsonDescription});
    // Each format's own options; a format takes only its own, and an option
    // several formats have is one option, said once for all of them.
    QHash<QString, QList<const Exporter *>> owners;
    QStringList order;
    for (const Exporter *exporter : Exporters::all()) {
        for (const ExportOption &option : exporter->options()) {
            if (!owners.contains(option.name)) order.append(option.name);
            owners[option.name].append(exporter);
        }
    }
    const auto optionOf = [](const Exporter *exporter, const QString &name) -> std::optional<ExportOption> {
        for (const ExportOption &option : exporter->options())
            if (option.name == name) return option;
        return std::nullopt;
    };
    for (const QString &name : std::as_const(order)) {
        // Formats that say the same are named together.
        QList<std::pair<QStringList, QString>> said;
        QString valueName;
        for (const Exporter *exporter : owners.value(name)) {
            const ExportOption option = *optionOf(exporter, name);
            valueName = option.valueName;
            auto same = std::find_if(said.begin(), said.end(), [&](const auto &s) { return s.second == option.description; });
            if (same == said.end()) said.append({{exporter->title()}, option.description});
            else same->first.append(exporter->title());
        }
        QStringList parts;
        for (const auto &[titles, description] : std::as_const(said))
            parts.append(titles.join(QStringLiteral(", ")) + QStringLiteral(": ") + description);
        command.add(QCommandLineOption(name, parts.join(QStringLiteral("; ")), valueName));
    }
    command.notes =
        dialectNotes() +
        QStringLiteral("\n\nmd writes the document with its includes in place; html writes a page of its own;\n"
                       "pdf sets it on pages; docx writes a Word document; epub packs it as a book.\n"
                       "Formulas and diagrams are drawn by parch's helpers where they are installed;\n"
                       "what a format cannot hold as the document has it is said as a note.\n"
                       "A document that includes a file that cannot be read is not exported.\n\n"
                       "Exit status: 0 written, 1 a wrong command line, 2 the document is incomplete,\n"
                       "3 a file could not be read or written, or OUT is there without --force,\n"
                       "4 the export could not be made.");
    if (const auto done = command.parse(arguments, context)) return *done;
    const QCommandLineParser &parser = command.parser;
    const QStringList files = parser.positionalArguments();
    if (files.size() != 1)
        return command.usage(QStringLiteral("say which one file: parch export FILE --to FORMAT -o OUT"), context);
    if (!parser.isSet(QStringLiteral("output")))
        return command.usage(QStringLiteral("say where to write it with -o OUT"), context);
    const QString output = absolute(parser.value(QStringLiteral("output")), context);
    const Exporter *exporter = parser.isSet(QStringLiteral("to"))
                                   ? Exporters::find(parser.value(QStringLiteral("to")))
                                   : Exporters::forFile(output);
    if (!exporter) {
        if (!parser.isSet(QStringLiteral("to")))
            return command.usage(
                QStringLiteral("say which format with --to: %1").arg(formats.join(QStringLiteral(", "))), context);
        return command.usage(QStringLiteral("--to takes %1, not \"%2\"")
                                 .arg(formats.join(QStringLiteral(", ")), parser.value(QStringLiteral("to"))),
                             context);
    }
    ExportRecipe recipe;
    recipe.format = exporter->format();
    recipe.output = output;
    for (auto it = owners.cbegin(); it != owners.cend(); ++it) {
        if (!parser.isSet(it.key())) continue;
        if (!it.value().contains(exporter)) {
            QStringList titles;
            for (const Exporter *owner : it.value()) titles.append(owner->title());
            const QString last = titles.takeLast();
            const QString owned = titles.isEmpty() ? last : titles.join(QStringLiteral(", ")) + QStringLiteral(" and ") + last;
            return command.usage(QStringLiteral("--%1 is an option of %2 export, not %3").arg(it.key(), owned, exporter->title()),
                                 context);
        }
        const ExportOption option = *optionOf(exporter, it.key());
        QString value = parser.value(it.key());
        if (!option.choices.isEmpty()) {
            const auto chosen = std::find_if(option.choices.cbegin(), option.choices.cend(), [&](const QString &choice) {
                return choice.compare(value, Qt::CaseInsensitive) == 0;
            });
            if (chosen == option.choices.cend())
                return command.usage(QStringLiteral("--%1 takes %2, not \"%3\"")
                                         .arg(it.key(), option.choices.join(QStringLiteral(", ")), value),
                                     context);
            value = *chosen;
        } else if (option.number) {
            bool ok = false;
            const double number = value.toDouble(&ok);
            if (!ok || number < option.minimum || number > option.maximum)
                return command.usage(QStringLiteral("--%1 takes a number from %2 to %3, not \"%4\"")
                                         .arg(it.key())
                                         .arg(option.minimum)
                                         .arg(option.maximum)
                                         .arg(value),
                                     context);
        }
        recipe.options.insert(it.key(), value);
    }
    DialectFlags flags;
    if (const auto done = readDialectFlags(command, context, &flags)) return *done;
    QString why;
    if (!exporter->available(&why)) return command.fail(ExportFailed, why, context);

    DocumentCheck::Loaded loaded;
    QString error;
    if (!loadDocument(absolute(files.first(), context), flags, context, &loaded, &error))
        return command.fail(Io, error, context);
    const md::Document &document = loaded.document;
    recipe.source = loaded.path;
    recipe.dialect = loaded.dialect;
    recipe.citations = std::make_shared<const DocumentCitations::Formatted>(loaded.formatted);
    if (document.incomplete) return incompleteOutput(command, loaded, context);
    const std::vector<DocumentCheck::Problem> problems = DocumentCheck::problems(loaded);
    if (!mayWrite(output, parser.isSet(QStringLiteral("force")), document, loaded.dialect, &error))
        return command.fail(Io, error, context);

    recipe.existing = parser.isSet(QStringLiteral("force")) ? ExistingOutput::ReplaceFile : ExistingOutput::Refuse;
    const ExportResult result = exporter->write(document, recipe);
    if (!result.ok) return command.fail(result.ioFailure ? Io : ExportFailed, result.error, context);

    // What the reader of the export will miss.
    QStringList warnings;
    for (const DocumentCheck::Problem &problem : problems)
        if (missedByReader(problem)) warnings.append(problemLine(problem, document, context));
    if (command.wantsJson()) {
        QJsonObject report = header(QStringLiteral("export"));
        report.insert(QStringLiteral("ok"), true);
        report.insert(QStringLiteral("file"), loaded.path);
        report.insert(QStringLiteral("format"), exporter->format());
        report.insert(QStringLiteral("output"), output);
        report.insert(QStringLiteral("written"), QJsonArray::fromStringList(result.written));
        report.insert(QStringLiteral("limitations"), QJsonArray::fromStringList(result.limitations));
        QJsonArray assets;
        for (const DocumentCheck::Problem &problem : problems)
            if (missedByReader(problem)) assets.append(problemJson(problem, document));
        report.insert(QStringLiteral("warnings"), assets);
        report.insert(QStringLiteral("dialect"), loaded.dialect.toJson());
        QJsonObject used = renderers({"markdown"});
        for (auto it = result.renderers.begin(); it != result.renderers.end(); ++it) used.insert(it.key(), it.value());
        report.insert(QStringLiteral("renderers"), used);
        report.insert(QStringLiteral("helpers"), result.helpers);
        report.insert(QStringLiteral("recipe"), recipe.toJson());
        json(context.out, report);
    } else if (!parser.isSet(QStringLiteral("quiet"))) {
        line(context.out, output);
        for (const QString &limitation : result.limitations) line(context.err, QStringLiteral("note: ") + limitation);
        for (const QString &warning : warnings) line(context.err, warning);
    }
    return Ok;
}

int stats(const QStringList &arguments, const Context &context)
{
    Command command(QStringLiteral("stats"), QStringLiteral("FILE [options]"),
                    QStringLiteral("Count what a reader of a Markdown document reads, with the files it includes:\n"
                                   "its words, characters and headings, and about how long it takes to read."));
    addDialectOptions(command);
    command.add({QStringLiteral("json"), jsonDescription});
    command.notes =
        dialectNotes() +
        QStringLiteral("\n\nWords are found by the window's rules, in the text a reader sees: without front\n"
                       "matter, comments, link addresses or Markdown punctuation. Reading time is at %1\n"
                       "words a minute. Characters do not count line breaks.\n\n"
                       "Exit status: 0 counted, 1 a wrong command line, 3 the document could not be read.")
            .arg(WordCounter::WordsPerMinute);
    if (const auto done = command.parse(arguments, context)) return *done;
    const QStringList files = command.parser.positionalArguments();
    if (files.size() != 1) return command.usage(QStringLiteral("say which one file: parch stats FILE"), context);
    DialectFlags flags;
    if (const auto done = readDialectFlags(command, context, &flags)) return *done;
    DocumentCheck::Loaded loaded;
    QString error;
    if (!loadDocument(absolute(files.first(), context), flags, context, &loaded, &error))
        return command.fail(Io, error, context);
    const md::Document &document = loaded.document;

    const QString text = document.plainText();
    const int words = WordCounter::countWords(text);
    const int minutes = (words + WordCounter::WordsPerMinute - 1) / WordCounter::WordsPerMinute;
    int characters = 0, visible = 0;
    QTextBoundaryFinder graphemes(QTextBoundaryFinder::Grapheme, text);
    for (qsizetype at = 0, next = graphemes.toNextBoundary(); next > 0; at = next, next = graphemes.toNextBoundary()) {
        const QStringView grapheme = QStringView(text).sliced(at, next - at);
        if (grapheme == u"\n" || grapheme == u"\r" || grapheme == u"\r\n") continue;
        ++characters;
        if (!grapheme.front().isSpace()) ++visible;
    }
    int levels[6] = {0, 0, 0, 0, 0, 0};
    for (const md::Heading &heading : document.headings) ++levels[std::clamp(heading.level, 1, 6) - 1];

    if (command.wantsJson()) {
        QJsonObject report = header(QStringLiteral("stats"));
        report.insert(QStringLiteral("file"), loaded.path);
        report.insert(QStringLiteral("words"), words);
        report.insert(QStringLiteral("characters"), characters);
        report.insert(QStringLiteral("charactersWithoutSpaces"), visible);
        report.insert(QStringLiteral("readingMinutes"), minutes);
        report.insert(QStringLiteral("wordsPerMinute"), WordCounter::WordsPerMinute);
        report.insert(QStringLiteral("headings"), int(document.headings.size()));
        report.insert(QStringLiteral("headingLevels"),
                      QJsonArray{levels[0], levels[1], levels[2], levels[3], levels[4], levels[5]});
        report.insert(QStringLiteral("files"), int(document.files.size()));
        report.insert(QStringLiteral("incomplete"), document.incomplete);
        report.insert(QStringLiteral("dialect"), loaded.dialect.toJson());
        report.insert(QStringLiteral("renderers"), renderers({"markdown"}));
        report.insert(QStringLiteral("helpers"), QJsonObject());
        json(context.out, report);
        return Ok;
    }
    const QString time = words == 0     ? QStringLiteral("nothing to read")
                         : minutes == 1 ? QStringLiteral("about 1 minute")
                                        : QStringLiteral("about %1 minutes").arg(minutes);
    line(context.out, shown(loaded.path, context));
    line(context.out, QStringLiteral("  Words         %1").arg(words));
    line(context.out, QStringLiteral("  Characters    %1 (%2 without spaces)").arg(characters).arg(visible));
    line(context.out, QStringLiteral("  Reading time  %1").arg(time));
    line(context.out, QStringLiteral("  Headings      %1").arg(document.headings.size()));
    if (document.incomplete)
        line(context.err, QStringLiteral("parch stats: %1 includes a file that could not be read, which is not counted")
                              .arg(shown(loaded.path, context)));
    return Ok;
}

// The skill's own file, as `skill install` writes it. A file there that
// does not start like it belongs to something else, and is left alone.
bool isParchSkill(const QByteArray &text)
{
    return text.startsWith("---\nname: parch\n");
}

int skill(const QStringList &arguments, const Context &context)
{
    Command command(QStringLiteral("skill"), QStringLiteral("[install] [options]"),
                    QStringLiteral("Print the skill that tells coding agents how to use parch, or install it for\n"
                                   "them: ~/.agents/skills/parch/SKILL.md, and a link to it from\n"
                                   "~/.claude/skills/parch when ~/.claude is there. Installing again updates it;\n"
                                   "no other skill is touched, and a file or link there that is not parch's\n"
                                   "is left alone."));
    command.add({QStringLiteral("json"), jsonDescription});
    command.notes = QStringLiteral("Exit status: 0 done, 1 a wrong command line, 3 the skill could not be installed.");
    if (const auto done = command.parse(arguments, context)) return *done;
    QFile bundled(QStringLiteral(":/skill.md"));
    if (!bundled.open(QIODevice::ReadOnly)) return command.fail(Io, QStringLiteral("the skill is missing from this build"), context);
    const QByteArray text = bundled.readAll();
    const QStringList words = command.parser.positionalArguments();
    if (words.isEmpty()) {
        if (command.wantsJson()) {
            QJsonObject report = header(QStringLiteral("skill"));
            report.insert(QStringLiteral("name"), QStringLiteral("parch"));
            report.insert(QStringLiteral("skill"), QString::fromUtf8(text));
            json(context.out, report);
        } else {
            context.out->write(text);
        }
        return Ok;
    }
    if (words.size() > 1 || words.first() != u"install")
        return command.usage(QStringLiteral("use parch skill to print the skill, or parch skill install"), context);

    const QDir home(context.home);
    const QString folder = home.filePath(QStringLiteral(".agents/skills/parch"));
    const QString file = folder + QStringLiteral("/SKILL.md");
    bool changed = true;
    if (QFileInfo(file).isSymLink() || (QFileInfo::exists(file) && !QFileInfo(file).isFile()))
        return command.fail(Io, QStringLiteral("%1 is not parch's skill; leaving it alone").arg(file), context);
    if (QFileInfo::exists(file)) {
        QFile existing(file);
        if (!existing.open(QIODevice::ReadOnly))
            return command.fail(Io, QStringLiteral("cannot read %1: %2").arg(file, existing.errorString()), context);
        const QByteArray there = existing.readAll();
        if (!isParchSkill(there))
            return command.fail(Io, QStringLiteral("%1 is not parch's skill; leaving it alone").arg(file), context);
        changed = there != text;
    }
    // Claude Code finds skills in ~/.claude/skills; a link there keeps one
    // copy. Whatever is in the way is found before anything is written.
    const bool linking = home.exists(QStringLiteral(".claude"));
    const QString link = home.filePath(QStringLiteral(".claude/skills/parch"));
    const QFileInfo existingLink(link);
    const bool linkedAlready = existingLink.isSymLink();
    if (linking && linkedAlready &&
        QDir::cleanPath(existingLink.symLinkTarget()) != QDir::cleanPath(QFileInfo(folder).absoluteFilePath()))
        return command.fail(Io, QStringLiteral("%1 links elsewhere; leaving it alone").arg(link), context);
    if (linking && !linkedAlready && existingLink.exists())
        return command.fail(Io, QStringLiteral("%1 is there and is not a link; leaving it alone").arg(link), context);

    if (changed) {
        QString error;
        if (!QDir().mkpath(folder) || !Exporters::writeFile(file, text, &error))
            return command.fail(Io, error.isEmpty() ? QStringLiteral("cannot make %1").arg(folder) : error, context);
    }
    if (linking && !linkedAlready &&
        (!home.mkpath(QStringLiteral(".claude/skills")) ||
         !QFile::link(QStringLiteral("../../.agents/skills/parch"), link)))
        return command.fail(Io, QStringLiteral("cannot link %1").arg(link), context);
    const QString linked = linking ? link : QString();
    if (command.wantsJson()) {
        QJsonObject report = header(QStringLiteral("skill"));
        report.insert(QStringLiteral("installed"), file);
        report.insert(QStringLiteral("changed"), changed);
        report.insert(QStringLiteral("linked"), linked.isEmpty() ? QJsonValue() : QJsonValue(linked));
        json(context.out, report);
    } else {
        line(context.out, (changed ? QStringLiteral("Installed ") : QStringLiteral("Up to date: ")) + file);
        if (!linked.isEmpty()) line(context.out, QStringLiteral("Linked ") + linked);
    }
    return Ok;
}

int help(const QStringList &arguments, const Context &context)
{
    const QStringList topics = arguments.mid(2);
    if (topics.isEmpty() || topics == QStringList{QStringLiteral("help")}) {
        write(context.out, helpText());
        return Ok;
    }
    const QString topic = topics.first();
    if (topics.size() == 1 && topic == u"open") {
        write(context.out, openHelpText());
        return Ok;
    }
    if (topics.size() == 1 && isCommand(topic) && topic != u"help")
        return run({arguments.value(0), topic, QStringLiteral("--help")}, context);
    line(context.err, QStringLiteral("parch help: there is no command \"%1\"; the commands are open, %2")
                          .arg(topics.join(u' '), QStringList{QStringLiteral("check"), QStringLiteral("render"),
                                                              QStringLiteral("export"), QStringLiteral("stats"),
                                                              QStringLiteral("skill")}
                                                      .join(QStringLiteral(", "))));
    return Usage;
}

} // namespace

bool isCommand(const QString &word)
{
    static const QStringList commands{QStringLiteral("check"), QStringLiteral("render"), QStringLiteral("export"),
                                      QStringLiteral("stats"), QStringLiteral("skill"),  QStringLiteral("help")};
    return commands.contains(word);
}

QString summary()
{
    return QStringLiteral(
        "  check FILE...    Report what is wrong in documents, with file, line and column\n"
        "  render FILE      Write a document's HTML, plain text, outline or semantic tree\n"
        "  export FILE      Write a document as Markdown, HTML, PDF, DOCX or EPUB\n"
        "  stats FILE       Count words and characters, reading time and headings\n"
        "  skill [install]  Print the skill for coding agents, or install it\n"
        "  help [COMMAND]   Print this help, or a command's options\n");
}

int run(const QStringList &arguments, const Context &context)
{
    const QString command = arguments.value(1);
    if (command == u"check") return check(arguments, context);
    if (command == u"render") return render(arguments, context);
    if (command == u"export") return exportDocument(arguments, context);
    if (command == u"stats") return stats(arguments, context);
    if (command == u"skill") return skill(arguments, context);
    return help(arguments, context);
}

} // namespace Cli
