#include "documentchecker.h"

#include "attachments.h"
#include "documentreading.h"
#include "mdinclude.h"
#include "rendercheck.h"
#include "renderservice.h"
#include "sourcebuffer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QtConcurrent>

#include <atomic>

namespace {

std::atomic<int> workerDelay{0};

QString tr(const char *text) { return QCoreApplication::translate("DocumentChecker", text); }

// How much of the text at a problem's place is kept to find it again: its
// first line, at most this many characters.
constexpr int matchedUnits = 200;

QString counted(int count, const char *one, const char *many)
{
    return count == 1 ? tr(one) : tr(many).arg(count);
}

} // namespace

// What a worker found in the text: the problems, the formulas and diagrams
// to draw, and the files they are in (file 0 is the document's own text).
struct DocumentChecker::Found {
    std::vector<DocumentCheck::Problem> problems;
    std::vector<RenderCheck::Item> items;
    QStringList paths;
    QStringList texts;
    QString folder;
};

DocumentChecker::DocumentChecker(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(1);
}

DocumentChecker::~DocumentChecker()
{
    cancel();
    m_pool.waitForDone();
}

void DocumentChecker::setBackend(Backend *backend)
{
    if (backend == m_backend) return;
    cancel();
    m_backend = backend;
    emit backendChanged();
}

void DocumentChecker::setWorkerDelay(int milliseconds) { workerDelay = std::max(0, milliseconds); }

void DocumentChecker::setChecking(bool checking)
{
    if (checking == m_checking) return;
    m_checking = checking;
    emit checkingChanged();
}

void DocumentChecker::cancel()
{
    ++m_request;
    m_batch.reset();
    setChecking(false);
}

void DocumentChecker::check(Document *document)
{
    cancel();
    m_document = document;
    m_documentId = document ? document->id() : QString();
    m_found.reset();
    m_problems.clear();
    m_skippedFormulas = m_skippedDiagrams = 0;
    if (!document) {
        m_summary.clear();
        emit checked();
        return;
    }
    setChecking(true);
    m_summary = tr("Checking…");
    emit checked();
    // Where the document is, and where its settings find `/rooted` includes:
    // the library folder holding it, unless they name another.
    QString folder, library = document->libraryRoot();
    if (m_backend) {
        folder = m_backend->attachments()->documentFolder(document);
        if (library.isEmpty()) library = m_backend->attachments()->libraryOf(document);
    } else if (!document->isDraft()) {
        folder = QFileInfo(document->path()).absolutePath();
    }
    const quint64 request = m_request;
    QtConcurrent::run(&m_pool, &DocumentChecker::work, document->buffer()->text(),
                      document->isDraft() ? QString() : document->path(), folder, library, document->savedDialect())
        .then(this, [this, request](const std::shared_ptr<Found> &found) { read(request, found); });
}

std::shared_ptr<DocumentChecker::Found> DocumentChecker::work(QString text, QString path, QString folder,
                                                              QString library, Dialect::Layers layers)
{
    if (const int delay = workerDelay; delay > 0) QThread::msleep(delay);
    md::FileIncludeResolver resolver;
    md::Source source;
    source.path = path.isEmpty() ? QString() : QFileInfo(path).canonicalFilePath();
    source.directory = folder;
    source.libraryRoot = library;
    source.resolver = &resolver;
    DocumentReading::Reading reading = DocumentReading::read(text, layers, source);
    DocumentCheck::Loaded loaded;
    loaded.path = source.path;
    loaded.dialect = reading.dialect;
    loaded.document = std::move(reading.document);
    loaded.citations = std::move(reading.citations);
    loaded.formatted = std::move(reading.formatted);
    auto found = std::make_shared<Found>();
    found->problems = DocumentCheck::problems(loaded);
    found->items = RenderCheck::items(loaded.document);
    for (const md::SourceFile &file : loaded.document.files) {
        found->paths.append(file.path);
        found->texts.append(file.text);
    }
    // A draft's text is no file.
    if (!found->paths.isEmpty() && path.isEmpty()) found->paths.first().clear();
    found->folder = folder;
    return found;
}

void DocumentChecker::read(quint64 request, const std::shared_ptr<Found> &found)
{
    if (request != m_request) return;
    m_found = found;
    // The formulas and diagrams the helpers there are can draw.
    std::vector<RenderCheck::Item> drawn;
    RenderService *service = m_backend ? m_backend->renderService() : nullptr;
    const HelperSet helpers = service ? service->helpers() : HelperSet();
    for (const RenderCheck::Item &item : found->items) {
        if (RenderCheck::canDraw(helpers, item.kind)) drawn.push_back(item);
        else ++(item.isMath() ? m_skippedFormulas : m_skippedDiagrams);
    }
    if (drawn.empty()) {
        finish();
        return;
    }
    m_batch = std::make_unique<RenderCheck::Batch>(service, std::move(drawn));
    connect(m_batch.get(), &RenderCheck::Batch::finished, this, [this, request] {
        if (request != m_request || !m_batch || !m_found) return;
        for (DocumentCheck::Problem &problem : m_batch->problems()) m_found->problems.push_back(std::move(problem));
        DocumentCheck::order(m_found->problems);
        m_batch.release()->deleteLater();
        finish();
    });
    m_batch->start();
}

void DocumentChecker::finish()
{
    m_problems.clear();
    int problems = 0, notes = 0;
    for (const DocumentCheck::Problem &problem : m_found->problems) {
        m_problems.append(row(problem));
        ++(problem.severity == md::Severity::Info ? notes : problems);
    }
    QString summary;
    if (problems == 0 && notes == 0) summary = tr("Nothing is wrong in this document.");
    else if (notes == 0) summary = counted(problems, "One problem.", "%1 problems.");
    else if (problems == 0) summary = counted(notes, "No problems; one note.", "No problems; %1 notes.");
    else summary = counted(problems, "One problem", "%1 problems") + tr(" and ") + counted(notes, "one note.", "%1 notes.");
    if (m_skippedFormulas > 0 || m_skippedDiagrams > 0) {
        QStringList what, missing;
        if (m_skippedFormulas > 0) {
            what << counted(m_skippedFormulas, "one formula", "%1 formulas");
            missing << QStringLiteral("render-svg");
        }
        if (m_skippedDiagrams > 0) {
            what << counted(m_skippedDiagrams, "one diagram", "%1 diagrams");
            missing << QStringLiteral("merman-cli");
        }
        const bool one = m_skippedFormulas + m_skippedDiagrams == 1;
        QString line = tr("%1 %2 not checked: %3 %4 not found.")
                           .arg(what.join(tr(" and ")), one ? tr("was") : tr("were"), missing.join(tr(" and ")),
                                missing.size() == 1 ? tr("was") : tr("were"));
        line[0] = line[0].toUpper();
        summary += u' ' + line;
    }
    m_summary = summary;
    setChecking(false);
    emit checked();
}

QVariantMap DocumentChecker::row(const DocumentCheck::Problem &problem) const
{
    const Found &found = *m_found;
    QString path, shown, matched;
    int start = -1, length = 0;
    if (problem.file >= 0 && problem.file < found.paths.size()) {
        path = found.paths.at(problem.file);
        start = problem.range.start;
        // The first line of what it covers, to find it again.
        const QString &text = found.texts.at(problem.file);
        const int end = std::clamp(problem.range.end, start, int(text.size()));
        QStringView covered = QStringView(text).sliced(std::clamp(start, 0, int(text.size())),
                                                       std::max(0, end - std::clamp(start, 0, int(text.size()))));
        const qsizetype lineEnd = covered.indexOf(u'\n');
        if (lineEnd >= 0) covered = covered.first(lineEnd);
        if (covered.endsWith(u'\r')) covered.chop(1);
        matched = covered.first(std::min<qsizetype>(covered.size(), matchedUnits)).toString();
        length = int(matched.size());
    } else {
        path = problem.path;
    }
    // The document by its title; another file from the document's folder.
    if (problem.file == 0) {
        shown = m_document ? m_document->title() : QFileInfo(path).fileName();
    } else if (!found.folder.isEmpty() && path.startsWith(found.folder + u'/')) {
        shown = QDir(found.folder).relativeFilePath(path);
    } else {
        shown = QFileInfo(path).fileName();
    }
    const QString place = problem.position.line > 0
                              ? QStringLiteral("%1:%2:%3").arg(shown).arg(problem.position.line).arg(problem.position.column)
                              : shown;
    return {
        {QStringLiteral("text"), place + QStringLiteral(": ") + problem.message},
        {QStringLiteral("place"), place},
        {QStringLiteral("message"), problem.message},
        {QStringLiteral("severity"), QString::fromLatin1(md::severityName(problem.severity))},
        {QStringLiteral("code"), problem.code},
        {QStringLiteral("kind"), problem.kind},
        {QStringLiteral("file"), shown},
        {QStringLiteral("path"), path},
        {QStringLiteral("documentId"), problem.file == 0 && path.isEmpty() ? m_documentId : QString()},
        {QStringLiteral("start"), start},
        {QStringLiteral("length"), length},
        {QStringLiteral("line"), std::max(1, problem.position.line)},
        {QStringLiteral("matched"), matched},
        {QStringLiteral("included"), problem.file > 0},
    };
}

void DocumentChecker::open(int index)
{
    if (!m_backend || index < 0 || index >= m_problems.size()) return;
    const QVariantMap problem = m_problems.at(index).toMap();
    const QString path = problem.value(QStringLiteral("path")).toString();
    const QString documentId = problem.value(QStringLiteral("documentId")).toString();
    if (path.isEmpty() && documentId.isEmpty()) return;
    m_backend->openSearchResult(path, documentId, problem.value(QStringLiteral("start")).toInt(),
                                problem.value(QStringLiteral("length")).toInt(),
                                problem.value(QStringLiteral("line")).toInt(),
                                problem.value(QStringLiteral("matched")).toString());
}
