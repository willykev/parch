#pragma once

#include "renderhelpers.h"
#include "renderrequest.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>

#include <atomic>
#include <memory>

class RenderWorker;

// Draws formulas and diagrams with the helper programs, away from the GUI
// thread: helpers are run, supervised, read and their results cached on a
// thread of the service's own, and results come back through finished() on
// the thread that asked.
//
// A request may name a slot, such as the block of the document it is for.
// A newer request for the same slot supersedes the older one: the older
// helper is stopped if it is still running, and its result is never
// delivered, so a stale formula never replaces a current one.
class RenderService : public QObject {
    Q_OBJECT

public:
    struct Options {
        // Empty: found as HelperSet::find() finds them.
        HelperSet helpers;
        bool findHelpers = true;
        // Empty: RenderCache::standardFolder(). Set `diskCache` false to keep
        // nothing on disk.
        QString cacheFolder;
        bool diskCache = true;
        // Where each run's private home is made. Empty: the system's temporary folder.
        QString temporaryFolder;
        RenderLimits limits;
    };

    // What the service has done so far, for measurements and tests.
    struct Statistics {
        int requests = 0;
        int helperRuns = 0;
        int memoryHits = 0;
        int diskHits = 0;
        int joined = 0; // requests that waited for the same drawing as another
        int cancelled = 0;
        int mostAtOnce = 0; // the most helpers that ran at the same time
        int evictions = 0;
    };

    explicit RenderService(QObject *parent = nullptr);
    explicit RenderService(const Options &options, QObject *parent = nullptr);
    ~RenderService() override;

    // Starts drawing; the ticket comes back with the result.
    quint64 render(const RenderRequest &request, const QString &slot = {});
    // The result for the ticket is never delivered, and its helper is
    // stopped unless another request waits for the same drawing.
    void cancel(quint64 ticket);
    Statistics statistics() const;
    HelperSet helpers() const { return m_helpers; }

signals:
    void finished(quint64 ticket, const RenderResult &result);

private:
    friend class RenderWorker;
    void deliver(quint64 ticket, const RenderResult &result);

    struct Counters;
    std::shared_ptr<Counters> m_counters;
    HelperSet m_helpers;
    QThread m_thread;
    RenderWorker *m_worker = nullptr;
    quint64 m_nextTicket = 0;
    QSet<quint64> m_live;
    QHash<QString, quint64> m_slots;
};
