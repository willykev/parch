#include "exports.h"

#include "codehighlight.h"
#include "document.h"
#include "documentreading.h"
#include "exporter.h"
#include "filerequests.h"
#include "mdinclude.h"
#include "pdfexport.h"
#include "printportal.h"
#include "privatefiles.h"
#include "sourcebuffer.h"
#include "workerfailure.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QtConcurrent>

#include <atomic>

struct Exports::Job {
    std::atomic<bool> cancelled{false};
    std::atomic<int> permille{0};
    QMutex mutex;
    QString step;
    // What the notice calls it: the file's name, or the document's title when printing.
    QString name;
    bool printing = false;
    // The document's text is parsed, and the export runs on it.
    bool started = false;
    // The writer chose to replace the link at the output.
    bool replaceLink = false;
};

struct Exports::Outcome {
    ExportResult result;
    QByteArray pdf;
    QJsonObject recipe;
    QString documentId;
    QString output;
    QString title;
    bool printing = false;
    quint32 token = 0;
};

namespace {

QString canonical(const QString &path)
{
    if (path.isEmpty()) return {};
    const QString found = QFileInfo(path).canonicalFilePath();
    return found.isEmpty() ? QDir::cleanPath(QFileInfo(path).absoluteFilePath()) : found;
}

QString kindOf(const ExportOption &option)
{
    if (option.valueName.isEmpty()) return QStringLiteral("switch");
    if (!option.choices.isEmpty()) return QStringLiteral("choice");
    if (option.number) return QStringLiteral("number");
    return QStringLiteral("text");
}

} // namespace

Exports::Exports(const AppPaths &paths, FileRequests *files, QObject *parent)
    : QObject(parent), m_paths(paths), m_files(files), m_preview(new PrintPreview(this))
{
    m_pool.setMaxThreadCount(1);
    m_poll.setInterval(100);
    connect(&m_poll, &QTimer::timeout, this, &Exports::poll);
    if (m_files) {
        connect(m_files, &FileRequests::saveFileChosen, this, [this](WindowContext *window, const QString &purpose, const QString &path) {
            if (purpose != u"export") return;
            Asked asked = std::exchange(m_asked, {});
            if (!asked.document) return;
            const Exporter *exporter = Exporters::find(asked.format);
            QString output = path;
            // The dialog may leave the suffix off; the format puts its own on.
            if (exporter && !exporter->suffixes().contains(QFileInfo(path).suffix().toLower())) {
                output += u'.' + exporter->suffixes().first();
                // The dialog asked about the name as it was typed, not this
                // one: a file of this name is asked about here, before it is
                // written over.
                const QFileInfo there(output);
                if (there.exists() || there.isSymLink()) {
                    if (!window) {
                        emit problem(tr("Not exported"), tr("“%1” is already there; choose it in the file dialog to replace it.")
                                                             .arg(there.fileName()));
                        return;
                    }
                    asked.output = output;
                    m_replacing = asked;
                    emit replaceAsked(window, output);
                    return;
                }
            }
            start(asked.document, asked.format, asked.options, output, false);
        });
    }
    loadRecipes();
}

Exports::~Exports()
{
    if (m_job) m_job->cancelled = true;
    m_pool.waitForDone();
}

void Exports::setPrinter(Printer *printer)
{
    if (m_printer) disconnect(m_printer, nullptr, this, nullptr);
    m_printer = printer;
    if (!printer) return;
    connect(printer, &Printer::prepared, this, [this](quint32 token, const QVariantMap &setup) {
        const Asked asked = std::exchange(m_printing, {});
        if (!asked.document) return;
        QHash<QString, QString> options = asked.options;
        // The paper the print dialog chose, with margins no smaller than the printer's.
        const Printer::Page page = Printer::pageOf(setup);
        if (page.size.isValid() && !page.size.isEmpty()) {
            options.insert(QStringLiteral("page-width"), QString::number(page.size.width()));
            options.insert(QStringLiteral("page-height"), QString::number(page.size.height()));
            options.remove(QStringLiteral("page-size"));
        }
        if (page.landscape) options.insert(QStringLiteral("landscape"), QString());
        else options.remove(QStringLiteral("landscape"));
        const double margin = options.value(QStringLiteral("margins"), QStringLiteral("20")).toDouble();
        if (page.margin > margin) options.insert(QStringLiteral("margins"), QString::number(std::min(60.0, page.margin)));
        start(asked.document, QStringLiteral("pdf"), options, QString(), true, token);
    });
    connect(printer, &Printer::printed, this, [this] {
        emit news(tr("Sent to the printer"), tr("The desktop's printing has the document now."));
    });
    connect(printer, &Printer::failed, this, [this](const QString &message) {
        m_printing = {};
        emit problem(tr("Not printed"), message);
    });
}

void Exports::setRenderOptions(const RenderService::Options &options)
{
    m_render = options;
    m_preview->setRenderOptions(options);
}

QVariantMap Exports::paperDefaults(const QString &templateName) const
{
    ExportRecipe recipe;
    recipe.options.insert(QStringLiteral("template"), templateName);
    const PaperOptions paper = PaperOptions::from(*Exporters::find(QStringLiteral("pdf")), recipe);
    return {{QStringLiteral("margins"), paper.margin}, {QStringLiteral("font-size"), paper.fontSize}};
}

QVariantList Exports::formats() const
{
    QVariantList formats;
    for (const Exporter *exporter : Exporters::all()) {
        QVariantList options;
        for (const ExportOption &option : exporter->options()) {
            options.append(QVariantMap{
                {QStringLiteral("name"), option.name},
                {QStringLiteral("label"), option.label.isEmpty() ? option.name : option.label},
                {QStringLiteral("kind"), kindOf(option)},
                {QStringLiteral("choices"), option.choices},
                {QStringLiteral("defaultValue"), option.defaultValue},
                {QStringLiteral("minimum"), option.minimum},
                {QStringLiteral("maximum"), option.maximum},
                {QStringLiteral("description"), option.description},
            });
        }
        formats.append(QVariantMap{
            {QStringLiteral("format"), exporter->format()},
            {QStringLiteral("title"), exporter->title()},
            {QStringLiteral("suffix"), exporter->suffixes().first()},
            {QStringLiteral("options"), options},
        });
    }
    return formats;
}

QHash<QString, QString> Exports::recipeOptions(const QString &format, const QVariantMap &options)
{
    QHash<QString, QString> given;
    const Exporter *exporter = Exporters::find(format);
    if (!exporter) return given;
    for (const ExportOption &option : exporter->options()) {
        const auto value = options.constFind(option.name);
        if (value == options.cend()) continue;
        if (option.valueName.isEmpty()) {
            if (value->toBool()) given.insert(option.name, QString());
        } else if (const QString text = value->toString().trimmed(); !text.isEmpty()) {
            given.insert(option.name, text);
        }
    }
    return given;
}

void Exports::read(Document *document)
{
    m_read.reset();
    m_readDocument = document;
    const quint64 generation = ++m_readGeneration;
    if (!document) {
        m_reading = false;
        ++m_revision;
        emit readChanged();
        return;
    }
    m_reading = true;
    emit readChanged();
    // The document's parse of its text as it is now, which every view of it
    // shares; a document that closes first is read no more.
    connect(document, &QObject::destroyed, this, [this, generation] {
        if (generation != m_readGeneration || !m_reading) return;
        m_reading = false;
        ++m_revision;
        emit readChanged();
    }, Qt::SingleShotConnection);
    m_readFailure.clear();
    document->parse()->whenParsed(
        this,
        [this, generation](const ParsedTextPtr &parse) {
            if (generation != m_readGeneration) return;
            m_read = std::shared_ptr<const DocumentReading::Reading>(parse, &parse->reading);
            m_reading = false;
            ++m_revision;
            emit readChanged();
        },
        [this, generation](const QString &reason) {
            if (generation != m_readGeneration) return;
            m_readFailure = reason;
            m_reading = false;
            ++m_revision;
            emit readChanged();
        });
}

QStringList Exports::limitations(const QString &format, const QVariantMap &options) const
{
    if (!m_read && !m_readFailure.isEmpty()) return {tr("The document could not be read: %1.").arg(m_readFailure)};
    if (!m_read) return {};
    const QString name = format == u"print" ? QStringLiteral("pdf") : format;
    const Exporter *exporter = Exporters::find(name);
    if (!exporter) return {};
    ExportRecipe recipe;
    recipe.format = name;
    recipe.options = recipeOptions(name, options);
    if (format == u"print") recipe.options.remove(QStringLiteral("page-size"));
    QStringList limitations;
    if (m_read->document.incomplete)
        limitations.append(tr("A file it includes cannot be read, so it cannot be exported until it can."));
    const HelperSet helpers = m_render.findHelpers ? HelperSet::find() : m_render.helpers;
    limitations += exporter->limitations(m_read->document, recipe, helpers);
    return limitations;
}

void Exports::exportDocument(Document *document, const QString &format, const QVariantMap &options, WindowContext *window)
{
    const Exporter *exporter = Exporters::find(format);
    if (!document || !exporter) return;
    if (m_job) {
        emit problem(tr("An export is running"), tr("Wait for it to end, or cancel it, and then export again."));
        return;
    }
    askWhere(document, format, recipeOptions(format, options), window);
}

void Exports::askWhere(Document *document, const QString &format, const QHash<QString, QString> &options,
                       WindowContext *window)
{
    const Exporter *exporter = Exporters::find(format);
    if (!document || !exporter) return;
    m_replacing = {};
    m_asked = {document, format, options};
    QString folder = document->path().isEmpty() ? QString() : QFileInfo(document->path()).absolutePath();
    if (folder.isEmpty() && m_files) folder = m_files->startFolder();
    QString base = document->path().isEmpty() ? document->title() : QFileInfo(document->path()).completeBaseName();
    base.replace(u'/', u'-');
    if (base.trimmed().isEmpty()) base = tr("Untitled");
    const QString suffix = exporter->suffixes().first();
    const QString suggested = QDir(folder.isEmpty() ? QDir::homePath() : folder).filePath(base + u'.' + suffix);
    QStringList patterns;
    for (const QString &each : exporter->suffixes()) patterns.append(QStringLiteral("*.") + each);
    if (!m_files || !m_files->askForSaveFile(window, QStringLiteral("export"), tr("Export as %1").arg(exporter->title()),
                                             suggested, tr("%1 files").arg(exporter->title()), patterns))
        m_asked = {};
}

void Exports::answerReplace(bool replace)
{
    const Asked asked = std::exchange(m_replacing, {});
    if (!replace || !asked.document || asked.output.isEmpty()) return;
    start(asked.document, asked.format, asked.options, asked.output, false);
}

void Exports::exportTo(Document *document, const QString &format, const QVariantMap &options, const QString &path)
{
    if (!document || !Exporters::find(format)) return;
    start(document, format, recipeOptions(format, options), path, false);
}

void Exports::print(Document *document, const QVariantMap &options)
{
    if (!document) return;
    if (m_job) {
        emit problem(tr("An export is running"), tr("Wait for it to end, or cancel it, and then print."));
        return;
    }
    if (!m_printer) {
        emit problem(tr("No printing"), tr("The desktop's printing (xdg-desktop-portal) cannot be reached."));
        return;
    }
    QHash<QString, QString> given = recipeOptions(QStringLiteral("pdf"), options);
    given.remove(QStringLiteral("page-size"));
    m_printing = {document, QStringLiteral("pdf"), given};
    m_printer->prepare(document->title());
}

void Exports::exportElsewhere()
{
    const Asked linked = std::exchange(m_linked, {});
    m_linkedOutput.clear();
    if (linked.document) askWhere(linked.document, linked.format, linked.options);
}

void Exports::replaceLink()
{
    const Asked linked = std::exchange(m_linked, {});
    const QString output = std::exchange(m_linkedOutput, {});
    if (linked.document) start(linked.document, linked.format, linked.options, output, false, 0, true);
}

void Exports::keepLink()
{
    m_linked = {};
    m_linkedOutput.clear();
}

void Exports::cancel()
{
    if (m_job) m_job->cancelled = true;
}

bool Exports::stop(int waitMs)
{
    cancel();
    return m_pool.waitForDone(waitMs);
}

void Exports::start(Document *document, const QString &format, const QHash<QString, QString> &options,
                    const QString &output, bool printing, quint32 token, bool replaceLink)
{
    if (!document) return;
    if (m_job) {
        emit problem(tr("An export is running"), tr("Wait for it to end, or cancel it, and then export again."));
        return;
    }
    const Exporter *exporter = Exporters::find(format);
    if (!exporter) return;
    keepLink();
    // A link where the file would be is never written through: the writer
    // is asked first.
    if (!printing && !replaceLink && !Exporters::linkRefusal(output).isEmpty()) {
        m_linked = {document, format, options};
        m_linkedOutput = output;
        emit linkInTheWay(output, QFile::symLinkTarget(output));
        return;
    }
    auto job = std::make_shared<Job>();
    job->printing = printing;
    job->replaceLink = replaceLink;
    job->name = printing ? document->title() : QFileInfo(output).fileName();
    m_job = job;
    m_progress = 0;
    m_status = printing ? tr("Making “%1” ready to print").arg(job->name) : tr("Exporting “%1”").arg(job->name);
    emit busyChanged();
    emit progressChanged();
    m_poll.start();

    // Code is coloured on the worker only once the languages are loaded,
    // which is done here, on the UI thread, as `parch export` does on its own.
    CodeHighlight::prepare();
    // The document's parse of its text, which every view of it shares: of
    // the text as it is now, or, when that is not parsed yet, as it is once
    // it is, a moment later. That is what the export reads, whatever is
    // typed while it runs.
    const QString path = document->path();
    const QString documentId = document->id();
    const QString title = document->title();
    const RenderService::Options render = m_render;
    // A document that closes before its text is parsed is not exported.
    connect(document, &QObject::destroyed, this, [this, job, output, printing, token, documentId] {
        if (m_job != job || job->started) return;
        Outcome outcome;
        outcome.documentId = documentId;
        outcome.output = output;
        outcome.printing = printing;
        outcome.token = token;
        outcome.result.error = QStringLiteral("the document closed before it was read");
        done(outcome);
    }, Qt::SingleShotConnection);
    document->parse()->whenParsed(
        this,
        [=, this](const ParsedTextPtr &parse) {
            if (m_job != job) return;
            job->started = true;
            exportParsed(job, parse, exporter, options, output, printing, token, path, documentId, title, render);
        },
        [=, this](const QString &reason) {
            if (m_job != job) return;
            job->started = true;
            Outcome outcome;
            outcome.documentId = documentId;
            outcome.output = output;
            outcome.printing = printing;
            outcome.token = token;
            outcome.title = title;
            outcome.result.error = QStringLiteral("the document could not be read: %1").arg(reason);
            done(outcome);
        });
}

void Exports::exportParsed(const std::shared_ptr<Job> &job, const ParsedTextPtr &parse, const Exporter *exporter,
                           const QHash<QString, QString> &options, const QString &output, bool printing,
                           quint32 token, const QString &path, const QString &documentId, const QString &title,
                           const RenderService::Options &render)
{
    const auto work = [=]() -> Outcome {
        Outcome outcome;
        outcome.documentId = documentId;
        outcome.output = output;
        outcome.printing = printing;
        outcome.token = token;
        outcome.title = title;
        {
            QMutexLocker lock(&job->mutex);
            job->step = QStringLiteral("Reading the document");
        }
        WorkerFailure::check(WorkerFailure::Job::Export);
        // The citations as the document's style formats them, kept with the
        // parse they point into.
        struct Read {
            ParsedTextPtr parse;
            DocumentCitations::Formatted formatted;
        };
        const auto read = std::make_shared<const Read>(
            Read{parse, DocumentCitations::format(parse->reading.document, parse->reading.citations)});
        const DocumentReading::Reading *reading = &parse->reading;
        if (reading->document.incomplete) {
            outcome.result.error = QStringLiteral("a file it includes could not be read, so it is not exported as if it were whole");
            return outcome;
        }
        if (!printing && Exporters::isSource(reading->document, output)) {
            outcome.result.error = QStringLiteral("%1 is the document or a file it includes, which an export never writes over")
                                       .arg(output);
            return outcome;
        }
        ExportRecipe recipe;
        recipe.source = canonical(path);
        recipe.format = exporter->format();
        recipe.output = output;
        recipe.dialect = reading->dialect;
        recipe.options = options;
        recipe.citations = std::shared_ptr<const DocumentCitations::Formatted>(read, &read->formatted);
        recipe.existing = job->replaceLink ? ExistingOutput::ReplaceLink : ExistingOutput::ReplaceFile;
        ExportRun run;
        run.cancelled = [job] { return job->cancelled.load(); };
        run.progress = [job](qreal done, const QString &step) {
            job->permille = int(std::clamp<qreal>(done, 0, 1) * 1000);
            QMutexLocker lock(&job->mutex);
            job->step = step;
        };
        run.render = render;
        if (printing) {
            outcome.pdf = static_cast<const PdfExporter *>(exporter)->render(reading->document, recipe, run, &outcome.result);
            outcome.result.ok = !outcome.pdf.isEmpty() && !outcome.result.cancelled;
            if (job->cancelled) outcome.result = Exporters::cancelled();
        } else {
            outcome.result = exporter->write(reading->document, recipe, run);
        }
        QJsonObject json = recipe.toJson();
        json.insert(QStringLiteral("parch"), QStringLiteral(PARCH_VERSION));
        json.insert(QStringLiteral("renderers"), outcome.result.renderers);
        json.insert(QStringLiteral("helpers"), outcome.result.helpers);
        json.insert(QStringLiteral("limitations"), QJsonArray::fromStringList(outcome.result.limitations));
        json.insert(QStringLiteral("at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        outcome.recipe = json;
        return outcome;
    };
    // An export that throws, as when a document made to harm runs it out of
    // memory, ends as one that failed, and says why.
    QtConcurrent::run(&m_pool, [=]() -> Outcome {
        try {
            return work();
        } catch (...) {
            Outcome outcome;
            outcome.documentId = documentId;
            outcome.output = output;
            outcome.printing = printing;
            outcome.token = token;
            outcome.title = title;
            outcome.result.error =
                QStringLiteral("it could not be made: %1").arg(WorkerFailure::reason(std::current_exception()));
            return outcome;
        }
    }).then(this, [this](const Outcome &outcome) { done(outcome); });
}

void Exports::poll()
{
    if (!m_job) return;
    QString step;
    {
        QMutexLocker lock(&m_job->mutex);
        step = m_job->step;
    }
    const qreal progress = m_job->permille / 1000.0;
    const QString status = (m_job->printing ? tr("Making “%1” ready to print") : tr("Exporting “%1”")).arg(m_job->name) +
                           (step.isEmpty() ? QString() : QStringLiteral(": ") + step.toLower()) +
                           QStringLiteral(" (%1%)").arg(qRound(progress * 100));
    if (qFuzzyCompare(progress + 1, m_progress + 1) && status == m_status) return;
    m_progress = progress;
    m_status = status;
    emit progressChanged();
}

void Exports::done(const Outcome &outcome)
{
    m_job.reset();
    m_poll.stop();
    m_progress = 0;
    m_status.clear();
    emit busyChanged();
    emit progressChanged();
    const ExportResult &result = outcome.result;
    if (result.cancelled) {
        emit news(outcome.printing ? tr("Printing cancelled") : tr("Export cancelled"),
                  outcome.printing ? tr("Nothing was sent to the printer.")
                                   : tr("Nothing was written; a file that was there is as it was."));
        emit finished(false);
        return;
    }
    if (!result.ok) {
        emit problem(outcome.printing ? tr("Not printed") : tr("Not exported"),
                     result.error.isEmpty() ? tr("The export could not be made.") : result.error);
        emit finished(false);
        return;
    }
    if (outcome.printing) {
        if (m_printer) m_printer->print(outcome.title, outcome.pdf, outcome.token);
        if (!result.limitations.isEmpty())
            emit news(tr("Printing “%1”").arg(outcome.title), result.limitations.join(u'\n'));
        emit finished(true);
        return;
    }
    remember(outcome.documentId, outcome.recipe);
    emit news(tr("Exported “%1”").arg(QFileInfo(outcome.output).fileName()),
              result.limitations.isEmpty() ? tr("Everything in the document is in it.") : result.limitations.join(u'\n'));
    emit finished(true);
}

QString Exports::lastFormat(Document *document) const
{
    return document ? m_recipes.value(document->id()).value(QStringLiteral("format")).toString() : QString();
}

QVariantMap Exports::lastOptions(Document *document) const
{
    return document ? m_recipes.value(document->id()).value(QStringLiteral("options")).toObject().toVariantMap() : QVariantMap();
}

QJsonObject Exports::lastRecipe(Document *document) const
{
    return document ? m_recipes.value(document->id()) : QJsonObject();
}

bool Exports::exportAgain(Document *document)
{
    if (!document) return false;
    const QJsonObject recipe = m_recipes.value(document->id());
    const QString output = recipe.value(QStringLiteral("output")).toString();
    if (recipe.isEmpty() || output.isEmpty()) {
        emit problem(tr("Nothing to export again"), tr("Export this document once, and Export again repeats that export."));
        return false;
    }
    QHash<QString, QString> options;
    const QJsonObject given = recipe.value(QStringLiteral("options")).toObject();
    for (auto it = given.begin(); it != given.end(); ++it)
        options.insert(it.key(), it.value().isBool() ? QString() : it.value().toVariant().toString());
    start(document, recipe.value(QStringLiteral("format")).toString(), options, output, false);
    return true;
}

QString Exports::recipesFile() const { return m_paths.data + QStringLiteral("/exports.json"); }

void Exports::loadRecipes()
{
    QFile file(recipesFile());
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject recipes = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("recipes")).toObject();
    for (auto it = recipes.begin(); it != recipes.end(); ++it)
        if (AppPaths::isDocumentId(it.key()) && it.value().isObject()) m_recipes.insert(it.key(), it.value().toObject());
}

void Exports::remember(const QString &documentId, const QJsonObject &recipe)
{
    if (documentId.isEmpty()) return;
    m_recipes.insert(documentId, recipe);
    QJsonObject recipes;
    for (auto it = m_recipes.cbegin(); it != m_recipes.cend(); ++it) recipes.insert(it.key(), it.value());
    PrivateFiles::makeFolder(m_paths.data);
    QSaveFile file(recipesFile());
    if (file.open(QIODevice::WriteOnly) && PrivateFiles::restrict(file)) {
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("recipes"), recipes}}).toJson());
        file.commit();
    }
}
