#include "autosave.h"

#include "document.h"
#include "documentstore.h"
#include "sourcebuffer.h"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

// A QObject so that the connections made for it end with it.
struct Autosave::Watch : QObject {
    QPointer<Document> document;
    QString id;
    QTimer *recoveryIdle = nullptr, *recoveryMax = nullptr, *saveIdle = nullptr, *saveMax = nullptr;
    // The revision last handed to the writer, so an unchanged text is not written again.
    std::optional<Revision> queued;
    // The version of the file that snapshot was made on.
    std::optional<DiskVersion> base;
};

Autosave::Autosave(DocumentStore *store, Recovery *recovery, QObject *parent)
    : QObject(parent), m_store(store), m_recovery(recovery)
{
    // One thread keeps the writes and discards of a document in the order they were asked for.
    m_pool.setMaxThreadCount(1);
    connect(store, &DocumentStore::documentClosed, this, &Autosave::unwatch);
}

Autosave::~Autosave()
{
    // A write in flight completes, so that a crash-like teardown still leaves its entry.
    m_pool.waitForDone();
    qDeleteAll(m_watches);
}

bool Autosave::needsRecovery(const Document *document)
{
    // A document with no text of its own has nothing to keep.
    if (document->isTextless()) return false;
    switch (document->state()) {
    case Document::State::Modified:
    case Document::State::Saving:
        return true;
    // These keep an edit the file does not have, but only while the text differs from the file.
    case Document::State::SaveFailed:
        return document->buffer()->modified();
    // With the file gone, the text in memory is the only copy, modified or not.
    case Document::State::Conflict:
        return document->buffer()->modified() || document->fileMissing();
    case Document::State::Clean:
    case Document::State::ReadOnly:
        return false;
    }
    return false;
}

void Autosave::watch(Document *document)
{
    if (!document || m_watches.contains(document->id())) return;
    auto *watch = new Watch;
    watch->document = document;
    watch->id = document->id();
    m_watches.insert(watch->id, watch);

    const auto makeTimer = [this, watch](int interval, void (Autosave::*action)(Watch *)) {
        auto *timer = new QTimer(watch);
        timer->setSingleShot(true);
        timer->setInterval(interval);
        connect(timer, &QTimer::timeout, watch, [this, watch, action] { (this->*action)(watch); });
        return timer;
    };
    watch->recoveryIdle = makeTimer(RecoveryIdleMs, &Autosave::snapshotNow);
    watch->recoveryMax = makeTimer(RecoveryMaxMs, &Autosave::snapshotNow);
    watch->saveIdle = makeTimer(SaveIdleMs, &Autosave::saveNow);
    watch->saveMax = makeTimer(SaveMaxMs, &Autosave::saveNow);

    connect(document->buffer(), &SourceBuffer::sourceChanged, watch, [this, watch] { edited(watch); });
    connect(document, &Document::changed, watch, [this, watch] { changed(watch); });
    connect(document, &QObject::destroyed, this, [this, id = watch->id] { unwatch(id); });
    // A restored document is already unsaved, and must not wait for another edit.
    if (needsRecovery(document)) edited(watch);
}

void Autosave::unwatch(const QString &documentId)
{
    Watch *watch = m_watches.take(documentId);
    if (!watch) return;
    // Deleting the watch also deletes its timers and ends its connections.
    delete watch;
}

void Autosave::stopTimers(Watch *watch)
{
    for (QTimer *timer : {watch->recoveryIdle, watch->recoveryMax, watch->saveIdle, watch->saveMax}) timer->stop();
}

void Autosave::edited(Watch *watch)
{
    // The idle timers wait out a pause; the maximum timers start with the
    // first edit of a burst and are not moved by later ones.
    watch->recoveryIdle->start();
    if (!watch->recoveryMax->isActive()) watch->recoveryMax->start();
    watch->saveIdle->start();
    if (!watch->saveMax->isActive()) watch->saveMax->start();
}

void Autosave::changed(Watch *watch)
{
    Document *document = watch->document;
    if (!document) return;
    if (needsRecovery(document)) {
        // The file moved on under the text, as when a save lands while typing
        // goes on. The entry would hold an older text on an older version of
        // the file, which after a crash would read as a change made by
        // someone else, so the newest text and version are written at once.
        if (watch->base && *watch->base != document->diskVersion()) {
            watch->queued.reset();
            snapshotNow(watch);
            return;
        }
        // A conflict over a vanished file needs its snapshot even though no edit made it.
        if (document->fileMissing() && watch->queued != document->buffer()->revision()) edited(watch);
        return;
    }
    if (document->state() != Document::State::Clean) return;
    // The file holds this text now, so no snapshot is wanted. The discard is
    // queued behind any write already waiting for the same document.
    stopTimers(watch);
    watch->queued.reset();
    watch->base.reset();
    m_pool.start([recovery = *m_recovery, id = watch->id] { recovery.discard(id); });
}

void Autosave::snapshotNow(Watch *watch)
{
    watch->recoveryIdle->stop();
    watch->recoveryMax->stop();
    Document *document = watch->document;
    if (!document || !needsRecovery(document)) return;
    const auto snapshot = document->buffer()->snapshot();
    if (watch->queued == snapshot->revision) return;
    watch->queued = snapshot->revision;
    watch->base = document->diskVersion();
    const quint64 write = ++m_writes;
    m_pool.start([this, recovery = *m_recovery, id = watch->id, path = document->path(), base = document->diskVersion(),
                  convention = document->convention(), snapshot, write] {
        QString error;
        if (recovery.write(id, path, *snapshot, base, convention, &error)) {
            QMetaObject::invokeMethod(this, [this, id, write] { emit recoveryWritten(id, write); }, Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, id, error] {
                // The next tick must try again, since nothing was written.
                if (Watch *watch = m_watches.value(id)) watch->queued.reset();
                emit recoveryFailed(id, error);
            },
            Qt::QueuedConnection);
    });
}

void Autosave::saveNow(Watch *watch)
{
    watch->saveIdle->stop();
    watch->saveMax->stop();
    Document *document = watch->document;
    if (!document) return;
    // Only text the file lacks is saved. A failure or a conflict waits for
    // someone's decision, and saving an unmodified document would clear the
    // error that is being shown.
    const auto state = document->state();
    if (state == Document::State::Modified || state == Document::State::Saving) m_store->save(document);
}

bool Autosave::keep(Document *document, QString *error)
{
    // A document with no text of its own has nothing to keep, and its
    // entry, if one is there, is not its text to replace.
    if (document->isTextless()) return true;
    // Writes already queued finish first, so the entry written here is the newest.
    m_pool.waitForDone();
    const auto snapshot = document->buffer()->snapshot();
    if (!m_recovery->write(document->id(), document->path(), *snapshot, document->diskVersion(), document->convention(),
                           error))
        return false;
    if (Watch *watch = m_watches.value(document->id())) {
        watch->queued = snapshot->revision;
        watch->base = document->diskVersion();
    }
    return true;
}

bool Autosave::writeNow(Document *document)
{
    Watch *watch = document ? m_watches.value(document->id()) : nullptr;
    if (!watch || !needsRecovery(document)) return false;
    // The same text is written again, since the last write of it failed.
    watch->queued.reset();
    snapshotNow(watch);
    return true;
}

void Autosave::discard(const QString &documentId)
{
    m_pool.start([recovery = *m_recovery, documentId] { recovery.discard(documentId); });
}

bool Autosave::secure(int waitMs)
{
    for (Watch *watch : std::as_const(m_watches)) stopTimers(watch);
    // Writes already queued finish first, so the entries written below are the newest.
    m_pool.waitForDone();
    // The text is in recovery before any save is waited for, so that it is
    // safe whatever the saves do and however soon the process ends.
    QList<QPointer<Document>> documents;
    for (Document *document : m_store->documents()) {
        documents.append(document);
        if (!needsRecovery(document)) continue;
        m_recovery->write(document->id(), document->path(), *document->buffer()->snapshot(), document->diskVersion(),
                          document->convention());
    }
    for (const auto &document : std::as_const(documents))
        if (document && document->state() == Document::State::Modified) m_store->save(document);
    const bool settled = m_store->waitForSaves(waitMs);
    // A text its file now holds needs no recovery, nor its saves telling apart.
    for (Document *document : m_store->documents()) {
        if (document->state() != Document::State::Clean) continue;
        m_recovery->discard(document->id());
        m_recovery->forgetSaves(document->id());
    }
    return settled;
}

bool Autosave::flush(QString *error, QStringList *unkept)
{
    for (Watch *watch : std::as_const(m_watches)) stopTimers(watch);
    // Writes already queued finish first, so the entries written below are the newest.
    m_pool.waitForDone();
    m_store->waitForSaves();
    // Waiting applies save outcomes, and a document that is closed and clean
    // is deleted meanwhile, so none is kept across a wait except as a guard.
    QList<QPointer<Document>> documents;
    for (Document *document : m_store->documents()) documents.append(document);
    for (const auto &document : std::as_const(documents)) {
        // A save can leave newer text behind, so it repeats while it makes progress.
        for (int attempt = 0; document && attempt < 3 && document->state() == Document::State::Modified; ++attempt) {
            m_store->save(document);
            m_store->waitForSaves();
        }
    }
    QStringList problems, notKept;
    // Read again, since the last wait may have closed documents.
    for (Document *document : m_store->documents()) {
        const auto state = document->state();
        if (state == Document::State::Clean) {
            m_recovery->discard(document->id());
            m_recovery->forgetSaves(document->id());
            continue;
        }
        if (state == Document::State::ReadOnly || document->isTextless()) continue;
        // Text that is still not in its file is reported, and kept in recovery where that works.
        QString reason;
        const auto snapshot = document->buffer()->snapshot();
        const bool written = m_recovery->write(document->id(), document->path(), *snapshot, document->diskVersion(),
                                               document->convention(), &reason);
        if (!written) notKept << document->id();
        const QString why = document->lastError().isEmpty() ? tr("not saved") : document->lastError();
        problems.append(written ? tr("%1: %2 (kept in recovery)").arg(document->title(), why)
                                : tr("%1: %2 (not kept in recovery: %3)").arg(document->title(), why, reason));
    }
    m_pool.waitForDone();
    if (unkept) *unkept = notKept;
    if (problems.isEmpty()) return true;
    if (error) *error = problems.join(u'\n');
    return false;
}
