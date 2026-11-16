#pragma once

#include "recovery.h"

#include <QHash>
#include <QObject>
#include <QThreadPool>
#include <optional>

class Document;
class DocumentStore;
class QTimer;

// Keeps unsaved text safe while someone writes: a recovery snapshot soon
// after each pause in typing, and a save of the document's own file a little
// later. Each has an idle delay, restarted by every edit, and a maximum
// interval, so that continuous typing is still captured. Timers run on the UI
// thread; encoding and writing recovery files happen on one worker thread, in
// order, so that a discard never overtakes the write before it.
class Autosave : public QObject {
    Q_OBJECT
public:
    static constexpr int RecoveryIdleMs = 500, RecoveryMaxMs = 2000, SaveIdleMs = 750, SaveMaxMs = 5000;
    // A recovery write that failed is tried again after RecoveryRetryMs, and
    // each further failure doubles the wait, up to RecoveryRetryMaxMs.
    static constexpr int RecoveryRetryMs = 1000, RecoveryRetryMaxMs = 60000;
    // How long a clean close waits for saves to land, at most.
    static constexpr int FlushWaitMs = 3000;

    Autosave(DocumentStore *store, Recovery *recovery, QObject *parent = nullptr);
    ~Autosave() override;

    void watch(Document *document);
    // For a clean close: saves every modified document and waits for it, and
    // writes recovery for text that cannot be saved. Returns false, with a
    // line for each document whose text is still not in its file (failed save,
    // conflict, a save that has not ended) and whether recovery holds it, so
    // that a close can warn. `unkept` names the documents whose text is in
    // neither its file nor recovery. Saves get at most waitMs in all to land:
    // one still running then, as on a file system that stopped answering,
    // has its text kept in recovery instead of holding the close up.
    bool flush(QString *error = nullptr, QStringList *unkept = nullptr, int waitMs = FlushWaitMs);
    // For an end that cannot wait or be refused, as when the session ends:
    // every text that its file lacks goes to recovery first, then modified
    // documents start saving, and the saves get at most waitMs to land.
    // Recovery is kept for whatever they did not save. False when a save is
    // still running at the end.
    bool secure(int waitMs);
    // Writes the document's text to recovery now, after the writes already
    // waiting, for a view about to close. False, with the reason, when
    // recovery cannot take it.
    bool keep(Document *document, QString *error = nullptr);
    // Writes the document's text to recovery now, off the UI thread, as a
    // retry after a failed write; the outcome comes as recoveryWritten() or
    // recoveryFailed(). False when its text needs no recovery, so nothing
    // was started.
    bool writeNow(Document *document);
    // Removes a closed document's recovery entry once the writes already
    // waiting are done, as when its text is given up.
    void discard(const QString &documentId);
    // How many recovery writes have been started, in order. Each
    // recoveryWritten() carries the number of its write, so that a report
    // can be told apart from one about an earlier write.
    quint64 recoveryWrites() const { return m_writes; }

signals:
    void recoveryFailed(const QString &documentId, const QString &error);
    // A snapshot of the document's text is now in recovery, by the write
    // numbered `write`.
    void recoveryWritten(const QString &documentId, quint64 write);

private:
    struct Watch;

    void edited(Watch *watch);
    void changed(Watch *watch);
    void snapshotNow(Watch *watch);
    void saveNow(Watch *watch);
    void stopTimers(Watch *watch);
    void unwatch(const QString &documentId);
    // Text that exists only in memory, which a recovery entry must hold.
    static bool needsRecovery(const Document *document);

    DocumentStore *m_store;
    Recovery *m_recovery;
    QThreadPool m_pool;
    QHash<QString, Watch *> m_watches;
    quint64 m_writes = 0;
};
