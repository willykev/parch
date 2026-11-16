#include "renderservice.h"

#include "diagramrender.h"
#include "mathrender.h"
#include "rendercache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QPointer>

#include <algorithm>
#include <functional>
#include <optional>

struct RenderService::Counters {
    std::atomic<int> requests{0};
    std::atomic<int> helperRuns{0};
    std::atomic<int> memoryHits{0};
    std::atomic<int> diskHits{0};
    std::atomic<int> joined{0};
    std::atomic<int> cancelled{0};
    std::atomic<int> mostAtOnce{0};
    std::atomic<int> evictions{0};
    std::atomic<int> running{0};
};

// Lives on the service's thread: plans each request, answers it from the
// cache or queues it, and runs at most limits.maxProcesses helpers at a time.
class RenderWorker : public QObject {
public:
    using Deliver = std::function<void(quint64, const RenderResult &)>;

    RenderWorker(const RenderService::Options &options, std::shared_ptr<RenderService::Counters> counters,
                 Deliver deliver)
        : m_options(options), m_counters(std::move(counters)), m_deliver(std::move(deliver)),
          m_cache(options.cacheFolder, options.limits.diskCacheBytes, options.limits.memoryCacheBytes)
    {
    }

    ~RenderWorker() override { shutdown(); }

    // Forgets every request and stops and reaps every helper, on the
    // helpers' own thread, finished ones included.
    void shutdown()
    {
        m_queue.clear();
        m_active.clear();
        m_byKey.clear();
        const QList<HelperProcess *> processes = findChildren<HelperProcess *>(Qt::FindDirectChildrenOnly);
        for (HelperProcess *process : processes) delete process;
    }

    void submit(quint64 ticket, const RenderRequest &request);
    void cancel(quint64 ticket);

private:
    struct Job {
        QString key;
        RenderRequest request;
        QList<HelperRun> runs;
        QList<HelperOutcome> outcomes;
        QList<quint64> tickets;
        HelperProcess *process = nullptr;
    };
    using JobPointer = std::shared_ptr<Job>;

    struct Plan {
        QString key;
        QList<HelperRun> runs;
        std::optional<RenderResult> answer;
    };
    Plan plan(const RenderRequest &request);
    void schedule();
    void runNext(const JobPointer &job);
    void ran(const JobPointer &job, HelperProcess *process, const HelperOutcome &outcome);
    void finish(const JobPointer &job, const std::optional<RenderResult> &result);
    RenderResult failure(const HelperOutcome &outcome, const RenderRequest &request) const;
    QString helperName(const RenderRequest &request) const;

    RenderService::Options m_options;
    std::shared_ptr<RenderService::Counters> m_counters;
    Deliver m_deliver;
    RenderCache m_cache;
    QList<JobPointer> m_queue;
    QList<JobPointer> m_active;
    QHash<QString, JobPointer> m_byKey;
    QHash<QString, DiagramRender::Fonts> m_fonts;
};

namespace {

RenderResult answer(RenderResult::Status status, const QString &diagnostic, const RenderRequest &request)
{
    RenderResult result;
    result.status = status;
    result.diagnostic = diagnostic;
    result.description = request.source.trimmed();
    return result;
}

QString kindKey(const RenderRequest &request)
{
    switch (request.kind) {
    case RenderKind::InlineMath: return QStringLiteral("inline");
    case RenderKind::DisplayMath: return QStringLiteral("display");
    case RenderKind::Diagram: return QStringLiteral("diagram");
    }
    return {};
}

} // namespace

QString RenderWorker::helperName(const RenderRequest &request) const
{
    return request.kind == RenderKind::Diagram ? QStringLiteral("merman-cli") : QStringLiteral("render-svg");
}

RenderWorker::Plan RenderWorker::plan(const RenderRequest &request)
{
    Plan plan;
    const bool diagram = request.kind == RenderKind::Diagram;
    const QString program = diagram ? m_options.helpers.merman : m_options.helpers.ratex;
    const QString what = diagram ? QStringLiteral("Diagrams") : QStringLiteral("Math");
    if (program.isEmpty()) {
        plan.answer = answer(RenderResult::Failed,
                             QStringLiteral("%1 cannot be drawn: %2 was not found in %3.")
                                 .arg(what, helperName(request), HelperSet::searchFolders().join(QStringLiteral(", "))),
                             request);
        return plan;
    }
    if (const QFileInfo file(program); !file.isFile() || !file.isExecutable()) {
        plan.answer = answer(RenderResult::Failed,
                             QStringLiteral("%1 cannot be drawn: %2 was not found at %3.")
                                 .arg(what, helperName(request), program),
                             request);
        return plan;
    }
    const QString identity = HelperIdentity::of(program);
    if (identity.isEmpty()) {
        plan.answer = answer(RenderResult::Failed,
                             QStringLiteral("%1 cannot be drawn: %2 cannot be read.").arg(what, program), request);
        return plan;
    }
    QCryptographicHash key(QCryptographicHash::Sha256);
    key.addData(QByteArrayLiteral("parch render 1\n"));
    key.addData(identity.toLatin1());
    key.addData(QByteArrayLiteral("\n"));
    key.addData(kindKey(request).toLatin1());
    key.addData(request.purpose == RenderPurpose::Print ? QByteArrayLiteral("\nprint\n") : QByteArrayLiteral("\npreview\n"));
    if (diagram) {
        if (request.source.trimmed().isEmpty()) {
            plan.answer = answer(RenderResult::Refused, QStringLiteral("The diagram is empty."), request);
            return plan;
        }
        auto fonts = m_fonts.constFind(request.fontFamily);
        if (fonts == m_fonts.constEnd())
            fonts = m_fonts.insert(request.fontFamily, DiagramRender::chooseFonts(request.fontFamily));
        plan.runs = DiagramRender::plan(request, program, *fonts);
        key.addData(fonts->identity.toUtf8());
    } else {
        if (MathRender::oneLine(request.source).isEmpty()) {
            plan.answer = answer(RenderResult::Refused, QStringLiteral("The formula is empty."), request);
            return plan;
        }
        plan.runs = {MathRender::plan(request, program)};
    }
    // Everything a helper is given, and nothing that is not.
    for (const HelperRun &run : std::as_const(plan.runs)) {
        key.addData(QByteArrayLiteral("\nrun\n"));
        key.addData(run.arguments.join(QChar(0x1f)).toUtf8());
        for (const auto &[name, contents] : run.files) {
            key.addData(QByteArrayLiteral("\nfile\n"));
            key.addData(name.toUtf8());
            key.addData(QCryptographicHash::hash(contents, QCryptographicHash::Sha256));
        }
        key.addData(QByteArrayLiteral("\ninput\n"));
        key.addData(QCryptographicHash::hash(run.input, QCryptographicHash::Sha256));
    }
    plan.key = QString::fromLatin1(key.result().toHex());
    return plan;
}

void RenderWorker::submit(quint64 ticket, const RenderRequest &request)
{
    ++m_counters->requests;
    const Plan planned = plan(request);
    if (planned.answer) {
        m_deliver(ticket, *planned.answer);
        return;
    }
    if (const auto kept = m_cache.find(planned.key)) {
        ++(m_cache.lastFromDisk() ? m_counters->diskHits : m_counters->memoryHits);
        RenderResult result = *kept;
        if (result.description.isEmpty()) result.description = request.source.trimmed();
        m_deliver(ticket, result);
        return;
    }
    if (const JobPointer job = m_byKey.value(planned.key)) {
        job->tickets << ticket;
        ++m_counters->joined;
        return;
    }
    auto job = std::make_shared<Job>();
    job->key = planned.key;
    job->request = request;
    job->runs = planned.runs;
    job->tickets = {ticket};
    m_byKey.insert(job->key, job);
    m_queue << job;
    schedule();
}

void RenderWorker::cancel(quint64 ticket)
{
    for (const QList<JobPointer> *jobs : {&m_queue, &m_active}) {
        for (const JobPointer &job : *jobs) {
            if (!job->tickets.removeOne(ticket)) continue;
            ++m_counters->cancelled;
            if (!job->tickets.isEmpty()) return;
            if (job->process) {
                // finish() follows when the helper has gone.
                job->process->cancel();
            } else {
                const JobPointer keep = job;
                m_queue.removeOne(keep);
                if (m_byKey.value(keep->key) == keep) m_byKey.remove(keep->key);
            }
            return;
        }
    }
}

void RenderWorker::schedule()
{
    while (m_active.size() < std::max(m_options.limits.maxProcesses, 1) && !m_queue.isEmpty()) {
        const JobPointer job = m_queue.takeFirst();
        m_active << job;
        runNext(job);
    }
}

void RenderWorker::runNext(const JobPointer &job)
{
    HelperRun run = job->runs.at(job->outcomes.size());
    if (run.inputFromPrevious && !job->outcomes.isEmpty()) run.input = run.inputFromPrevious(job->outcomes.last().output);
    auto *process = new HelperProcess(run, m_options.limits, m_options.temporaryFolder, this);
    job->process = process;
    connect(process, &HelperProcess::finished, this,
            [this, job, process](const HelperOutcome &outcome) { ran(job, process, outcome); });
    ++m_counters->helperRuns;
    const int running = ++m_counters->running;
    int most = m_counters->mostAtOnce.load();
    while (running > most && !m_counters->mostAtOnce.compare_exchange_weak(most, running)) {
    }
    process->start();
}

void RenderWorker::ran(const JobPointer &job, HelperProcess *process, const HelperOutcome &outcome)
{
    --m_counters->running;
    job->process = nullptr;
    process->deleteLater();
    if (outcome.status == HelperOutcome::Cancelled) {
        finish(job, std::nullopt);
        return;
    }
    job->outcomes << outcome;
    const RenderRequest &request = job->request;
    if (outcome.status != HelperOutcome::Finished) {
        finish(job, failure(outcome, request));
        return;
    }
    if (request.kind != RenderKind::Diagram) {
        finish(job, MathRender::read(outcome, request));
        return;
    }
    if (outcome.succeeded() && job->outcomes.size() < job->runs.size()) {
        runNext(job);
        return;
    }
    finish(job, DiagramRender::read(job->outcomes.value(0), job->outcomes.value(1), request));
}

RenderResult RenderWorker::failure(const HelperOutcome &outcome, const RenderRequest &request) const
{
    const QString helper = helperName(request);
    QString why;
    switch (outcome.status) {
    case HelperOutcome::TimedOut:
        why = QStringLiteral("%1 took longer than %2 seconds and was stopped.")
                  .arg(helper)
                  .arg(m_options.limits.timeoutMs / 1000.0);
        break;
    case HelperOutcome::TooMuchOutput:
        why = QStringLiteral("%1 wrote more than %2 KiB and was stopped.")
                  .arg(helper)
                  .arg(m_options.limits.maxOutputBytes / 1024);
        break;
    default:
        why = QStringLiteral("%1 could not draw it: %2")
                  .arg(helper, QString::fromUtf8(outcome.errors).trimmed().left(2000));
        break;
    }
    RenderResult result = answer(RenderResult::Failed, why, request);
    result.elapsedMs = outcome.elapsedMs;
    return result;
}

void RenderWorker::finish(const JobPointer &job, const std::optional<RenderResult> &result)
{
    m_active.removeOne(job);
    if (m_byKey.value(job->key) == job) m_byKey.remove(job->key);
    if (result) {
        RenderResult delivered = *result;
        delivered.elapsedMs = 0;
        for (const HelperOutcome &outcome : std::as_const(job->outcomes)) delivered.elapsedMs += outcome.elapsedMs;
        // Whatever became of it, what it says stays available.
        if (delivered.description.isEmpty()) delivered.description = job->request.source.trimmed();
        // A refusal is the same every time for the same helper and source,
        // so it is kept too; a failure may not be.
        if (delivered.status != RenderResult::Failed) {
            m_cache.insert(job->key, delivered);
            m_counters->evictions = m_cache.evictions();
        }
        for (const quint64 ticket : std::as_const(job->tickets)) m_deliver(ticket, delivered);
    }
    schedule();
}

RenderService::RenderService(QObject *parent) : RenderService(Options{}, parent) {}

RenderService::RenderService(const Options &options, QObject *parent)
    : QObject(parent), m_counters(std::make_shared<Counters>())
{
    qRegisterMetaType<RenderResult>();
    Options resolved = options;
    if (resolved.findHelpers && resolved.helpers.ratex.isEmpty() && resolved.helpers.merman.isEmpty())
        resolved.helpers = HelperSet::find();
    if (!resolved.diskCache)
        resolved.cacheFolder.clear();
    else if (resolved.cacheFolder.isEmpty())
        resolved.cacheFolder = RenderCache::standardFolder();
    if (resolved.temporaryFolder.isEmpty()) resolved.temporaryFolder = QDir::tempPath();
    m_helpers = resolved.helpers;
    m_thread.setObjectName(QStringLiteral("parch render"));
    const QPointer<RenderService> self(this);
    m_worker = new RenderWorker(resolved, m_counters, [self](quint64 ticket, const RenderResult &result) {
        // From the service's thread to the one that owns the service.
        QMetaObject::invokeMethod(
            self.data(), [self, ticket, result] {
                if (self) self->deliver(ticket, result);
            },
            Qt::QueuedConnection);
    });
    m_worker->moveToThread(&m_thread);
    m_thread.start();
}

RenderService::~RenderService()
{
    // The helpers are stopped and reaped on their own thread before it ends,
    // so that nothing they started outlives the service; then the worker,
    // whose thread has finished, holds nothing that belongs to it.
    RenderWorker *worker = std::exchange(m_worker, nullptr);
    QMetaObject::invokeMethod(worker, [worker] { worker->shutdown(); }, Qt::BlockingQueuedConnection);
    m_thread.quit();
    m_thread.wait();
    delete worker;
}

quint64 RenderService::render(const RenderRequest &request, const QString &slot)
{
    const quint64 ticket = ++m_nextTicket;
    if (!slot.isEmpty()) {
        if (const quint64 older = m_slots.value(slot)) cancel(older);
        m_slots.insert(slot, ticket);
    }
    m_live.insert(ticket);
    RenderWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker, [worker, ticket, request] { worker->submit(ticket, request); },
                              Qt::QueuedConnection);
    return ticket;
}

void RenderService::cancel(quint64 ticket)
{
    if (!m_live.remove(ticket)) return;
    for (auto it = m_slots.begin(); it != m_slots.end(); ++it)
        if (it.value() == ticket) {
            m_slots.erase(it);
            break;
        }
    RenderWorker *worker = m_worker;
    QMetaObject::invokeMethod(worker, [worker, ticket] { worker->cancel(ticket); }, Qt::QueuedConnection);
}

void RenderService::deliver(quint64 ticket, const RenderResult &result)
{
    // A ticket cancelled or superseded after its result was on its way.
    if (!m_live.remove(ticket)) return;
    for (auto it = m_slots.begin(); it != m_slots.end(); ++it)
        if (it.value() == ticket) {
            m_slots.erase(it);
            break;
        }
    emit finished(ticket, result);
}

RenderService::Statistics RenderService::statistics() const
{
    Statistics statistics;
    statistics.requests = m_counters->requests;
    statistics.helperRuns = m_counters->helperRuns;
    statistics.memoryHits = m_counters->memoryHits;
    statistics.diskHits = m_counters->diskHits;
    statistics.joined = m_counters->joined;
    statistics.cancelled = m_counters->cancelled;
    statistics.mostAtOnce = m_counters->mostAtOnce;
    statistics.evictions = m_counters->evictions;
    return statistics;
}
