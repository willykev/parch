#pragma once

#include "apppaths.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QWaitCondition>
#include <functional>
#include <optional>
#include <thread>

struct HistoryLimits {
    int versions = 100;
    int days = 30;
    qint64 bytes = qint64(1) << 30;
};

// Bounded local versions of each document, kept beneath AppPaths::history()
// as <document id>/<time>-<hash>.md plus an index.json. History is only a
// convenience beside the document's own file, so every failure is reported by
// an empty result and never thrown at the caller.
//
// Calls may come from any thread, since a save records its version on the
// thread that wrote the file; a mutex makes each call whole.
//
// The bytes all documents' versions take are kept as running totals, worked
// out once, on a thread of their own as History starts, which also removes
// unnamed versions past the age limit, and kept up by each change. A capture
// reads only its own document's index, and looks at other documents'
// versions only when the totals exceed the byte budget. Whether history is
// paused follows from the totals alone; versions removed behind its back
// count until their document's index is read again, or history starts again.
class History : public QObject {
    Q_OBJECT
public:
    static constexpr int CaptureIntervalMinutes = 5;

    explicit History(const AppPaths &paths, HistoryLimits limits = {},
                     std::function<QDateTime()> now = [] { return QDateTime::currentDateTimeUtc(); },
                     QObject *parent = nullptr);
    // Waits for the totals to be worked out, should they not be yet.
    ~History() override;

    // Why history kept a version by itself: a save, or one of the moments
    // before text is replaced or set aside.
    enum class Reason { Save, Opened, Reload, Overwrite, Restore, Recovery, Draft, Closed };
    Q_ENUM(Reason)

    struct Version {
        // The file name without .md.
        QString id;
        QDateTime taken;
        QString name;
        qint64 bytes = 0;
        Reason reason = Reason::Save;
    };

    // Records the bytes always, as a named version when a name is given. The
    // result is the version's id, or empty when nothing was recorded: history
    // is paused, the id is not a document id, or a file could not be written.
    // A caller that must tell a pause from a failure uses status below.
    // status, when given, says why nothing was recorded, decided under the
    // same lock as the capture so that no other thread's capture can change
    // the answer: Paused when the size limit refuses it, Failed when a file
    // could not be written or the id is not a document id.
    // Held: the document's newest version holds the bytes already, so they
    // are kept without a second copy.
    enum class CaptureStatus { Recorded, Held, Paused, Failed };
    // A version history keeps by itself, other than at a save, keeps its
    // reason in the index; one read from an index without it is a save.
    QString capture(const QString &documentId, const QByteArray &bytes, const QString &name = {},
                    CaptureStatus *status = nullptr, Reason reason = Reason::Save);
    // Records the bytes only when the document's newest version is at least
    // CaptureIntervalMinutes old or there is none. Empty when not due.
    QString captureIfDue(const QString &documentId, const QByteArray &bytes);
    // Records the bytes unless the document's newest version holds them
    // already, as every capture history makes by itself does.
    QString captureUnlessNewest(const QString &documentId, const QByteArray &bytes, Reason reason,
                                CaptureStatus *status = nullptr);
    // Newest first.
    QList<Version> versions(const QString &documentId) const;
    // Empty for a version that is missing as well as for an empty one, so a
    // caller that must tell them apart passes found.
    QByteArray read(const QString &documentId, const QString &versionId, bool *found = nullptr) const;
    // Why nothing more is recorded: named versions alone fill the byte
    // budget, or the last capture could not be kept even after every unnamed
    // version went.
    enum class Pause { None, NamedVersionsFill, VersionDidNotFit };
    Q_ENUM(Pause)
    bool paused() const;
    Pause pause() const;
    // Names a version, or names it anew. A named version is never pruned and
    // counts toward the named versions' share of the budget. False when the
    // name is empty, the version is missing, or a file could not be written.
    bool rename(const QString &documentId, const QString &versionId, const QString &name);
    // Removes a version, named or not, as the writer asks, which can leave
    // room for history to go on. False when it is missing or could not be removed.
    bool remove(const QString &documentId, const QString &versionId);
    // Works out whether history is paused, and says so if that changed.
    void checkPaused();
    // The bytes every document's versions take, and the named ones among them.
    struct Usage {
        qint64 bytes = 0;
        qint64 named = 0;
    };
    Usage usage() const;
    // How many times a document's index has been read, or every document's
    // when no id is given; for tests and measures.
    int indexReads(const QString &documentId = {}) const;
    // For tests: called as captureUnlessNewest() begins, on the thread that
    // makes the capture, before anything is locked or written.
    static void setBeforeCapture(std::function<void()> hook);

signals:
    // History has paused, gone on again, or paused for another reason.
    // Emitted on whichever thread made the change, while History holds its
    // lock, so a receiver takes it queued and never calls back into History
    // from it directly.
    void pausedChanged(History::Pause pause);

private:
    QString folderOf(const QString &documentId) const;
    QList<Version> load(const QString &documentId) const;
    bool store(const QString &documentId, const QList<Version> &versions) const;
    QStringList documentIds() const;
    // Records the bytes for a document whose versions, as its index lists
    // them, are given.
    QString captureLocked(const QString &documentId, QList<Version> versions, const QByteArray &bytes,
                          const QString &name, CaptureStatus &status, Reason reason);
    // The document's own unnamed versions beyond the count or the age go,
    // and when all documents together exceed the byte budget, every
    // document's are pruned with pruneAll().
    void prune(const QString &documentId, const QList<Version> &versions);
    void pruneAll();
    void removeFiles(const QString &documentId, const Version &version) const;
    bool writeName(const QString &documentId, const QString &versionId, const QString &name) const;
    Pause pauseLocked() const;
    // The running totals, with the lock held: once worked out, at once.
    Usage totalsLocked() const;
    Usage sumLocked() const;
    // What the document's versions take now, as they were just read or
    // written, which the totals follow.
    void noteDocument(const QString &documentId, const QList<Version> &versions) const;
    // Emits pausedChanged() when pause() now answers otherwise than it last
    // said. Called with the lock held.
    void announceLocked();

    AppPaths m_paths;
    HistoryLimits m_limits;
    std::function<QDateTime()> m_now;
    // The last capture did not fit beside the named versions. Visible as a
    // pause until a later capture is kept.
    bool m_full = false;
    // What pausedChanged() said last.
    Pause m_announced = Pause::None;
    mutable QMutex m_mutex;
    // The totals, once the scan begun on construction has worked them out,
    // and each document's share, as its index was last read or written.
    mutable std::optional<Usage> m_totals;
    mutable QHash<QString, Usage> m_documentUsage;
    mutable QWaitCondition m_scanned;
    mutable QHash<QString, int> m_indexReads;
    std::thread m_scan;
};
