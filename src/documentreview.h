#pragma once

#include "history.h"
#include "sourcetypes.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

class Document;
class DocumentStore;
class QFutureWatcherBase;

// Looking back at a document, as its dialogs do: its versions in history,
// and its file while it is in conflict. The reads run off the UI thread and
// are numbered; each answer carries its read's number, so that a dialog can
// tell a late answer to an older read from the one it waits for. What the
// writer does there acts on the document the dialog shows, whichever is in
// front: name a version, restore one, remove one, or choose what happens to
// a conflict. Problems are said through problem().
class DocumentReview : public QObject {
    Q_OBJECT
public:
    // The choices resolveConflict() takes.
    enum Resolution { Reload, KeepBoth, Overwrite, Follow, PutHere };
    Q_ENUM(Resolution)

    explicit DocumentReview(DocumentStore *store, QObject *parent = nullptr);
    // Waits for the reads and records still running, which use history.
    ~DocumentReview() override;

    // As DocumentStore::resolveConflict() does; a refusal is a problem said,
    // unless the document's conflict already shows its reason.
    Q_INVOKABLE void resolveConflict(Document *document, int choice);
    // Records the text as it is now as a new version with a name, off the UI thread.
    Q_INVOKABLE void checkpoint(Document *document, const QString &name);
    // As DocumentStore::restoreVersion() does; a refusal is a problem said,
    // every time. A version that is not UTF-8 text is restored as its bytes,
    // as DocumentStore::restoreBytes() does, off the UI thread.
    Q_INVOKABLE bool restoreVersion(Document *document, const QString &versionId);
    Q_INVOKABLE void nameVersion(Document *document, const QString &versionId, const QString &name);
    Q_INVOKABLE void removeVersion(Document *document, const QString &versionId);

    // Each returns its read's number, which its answer carries.
    Q_INVOKABLE int readVersions(Document *document);
    Q_INVOKABLE int readVersion(Document *document, const QString &versionId);
    // The document's file as it is on disk now, a draft's own file for a
    // draft, and what differs between it and `compareWith`, as difference()
    // finds it, worked out off the UI thread with the read.
    Q_INVOKABLE int readDiskText(Document *document, const QString &compareWith = {});
    // What differs between two texts, by whole lines: the common start and
    // end are trimmed, and what is left of each is the differing middle.
    // "differs" says whether there is one. Each side's middle is given as
    // source offsets ("localFrom", "localTo", "diskFrom", "diskTo") and as
    // positions in a text view, which shows CRLF as one separator
    // ("localStart", "localEnd", "diskStart", "diskEnd"). Within the middles
    // a line diff finds each run of lines that differ, listed in "runs" with
    // the same fields; lines the two share between runs are in none. A side
    // whose part of a run is empty, where the other has lines the first
    // lacks, starts and ends where those lines go. When either middle has
    // more than DiffLines lines, or the diff needs more than DiffEdits
    // edits, the whole middle is the one run, and "bounded" says so.
    // Nothing is given for texts that are the same.
    static constexpr int DiffLines = 20000;
    static constexpr int DiffEdits = 2000;
    static QVariantMap difference(const QString &local, const QString &disk);

    // Records the text as it is now, off the UI thread, for the reason
    // given, and hands `done` the outcome and the revision that was recorded.
    void record(Document *document, std::function<void(History::CaptureStatus, Revision)> done,
                History::Reason reason = History::Reason::Closed);
    // Works out, off the UI thread, whether history is paused.
    void checkPaused();
    // When a version was taken, as a person says it, in the locale's time:
    // today, yesterday, a day this year, or a day with its year.
    static QString when(const QDateTime &taken, const QDateTime &now = QDateTime::currentDateTime(),
                        bool seconds = false);
    // Why history kept a version by itself, in a few lowercase words, as
    // the version list says it after the time; nothing for a save.
    static QString reasonWords(History::Reason reason);
    // The same as when() for every version of a list, newest first: one that
    // shares its minute with a neighbour in the list shows its seconds too.
    // Two versions taken in the same second still read the same; one that
    // history kept by itself says why, as reasonWords() puts it.
    static QStringList whenAll(const QList<QDateTime> &taken, const QDateTime &now = QDateTime::currentDateTime());

signals:
    // An action is starting, before what it does.
    void acting();
    // A document's versions, newest first, each a map of its id, the time
    // it was taken, that time in words, its name, why history kept it, and
    // the encoding of one that is not UTF-8 text, as shown.
    void versionsRead(int request, const QString &documentId, const QVariantList &versions);
    // A version's text, or why it cannot be shown. A version that is not
    // UTF-8 text is read in the encoding named, as it is shown.
    void versionRead(int request, const QString &documentId, const QString &versionId, const QString &text,
                     const QString &problem, const QString &encoding = {});
    // The text of a document's file, or why it cannot be shown.
    void diskTextRead(int request, const QString &documentId, const QString &text, const QString &problem,
                      const QVariantMap &difference = {});
    // A version of the document was recorded, named or removed.
    void versionsChanged(const QString &documentId);
    // Something touched the document's file; a view comparing with it reads it again.
    void diskChanged(const QString &documentId);
    void problem(const QString &problem, const QString &detail);

private:
    template <typename Work, typename Done>
    void runJob(Work work, Done done);

    DocumentStore *m_store;
    int m_lastRequest = 0;
    QList<QFutureWatcherBase *> m_jobs;
};
