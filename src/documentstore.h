#pragma once

#include "apppaths.h"
#include "diskfile.h"
#include "documentlinks.h"
#include "filewatch.h"
#include "history.h"
#include "recovery.h"
#include "sourcetypes.h"

#include <QFuture>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class Document;
class SourceBuffer;

// Follow takes the file a document's path now leads to, when that is not its
// own file, as the document's file from then on.
enum class ConflictChoice { Reload, KeepBoth, Overwrite, Follow, PutHere };

// Owns every open document and keeps exactly one per canonical file, so that
// two views, a second path or a symlink never give a file two writers. Saves
// run off the UI thread, and the saves of one document never overlap.
class DocumentStore : public QObject {
    Q_OBJECT
public:
    explicit DocumentStore(const AppPaths &paths, QObject *parent = nullptr);
    // Smaller limits than the defaults, so that tests can reach them.
    DocumentStore(const AppPaths &paths, HistoryLimits limits, QObject *parent = nullptr);
    // How long a restore, Reload or Overwrite waits, at most, for history to
    // keep the text it replaces; history writes off the UI thread.
    static constexpr int HistoryWaitMs = 1000;
    ~DocumentStore() override;

    Document *newDraft();
    // A document of parch's own, such as its guide, with the title and text
    // given, read-only when asked: see Document::isScratch().
    Document *newScratch(const QString &title, const QString &text, bool readOnly);
    // The same, finding its pictures, includes and bibliography in `folder`
    // (Document::scratchFolder()). Save as names it as it names a draft, and
    // it is an ordinary document from then on.
    Document *newScratch(const QString &title, const QString &text, bool readOnly, const QString &folder);
    // The unnamed draft with this id, read from <drafts>/<id>/draft.md unless
    // it is open already; null when there is no such draft.
    // Opens a draft by its id. One that cannot be opened is null, with why
    // in `error`, worded whole, when it is asked for.
    Document *openDraft(const QString &documentId, QString *error = nullptr);
    // Opens the file at the path, however the path is spelled: the file is
    // the one the kernel finds there. A path recorded as a document's file
    // that leads to another file now, as through a link put in its place,
    // opens that document in conflict, with no text of its own: the file it
    // leads to is never read into it. A path named afresh through a link
    // opens the file it leads to.
    Document *open(const QString &path, QString *error = nullptr);
    // The same for a path recorded as the file of the document with this
    // id, as a session's tab records it.
    Document *reopen(const QString &path, const QString &documentId, QString *error = nullptr);
    // One retain per view showing the document, and one release when it closes.
    void retain(Document *document);
    void release(Document *document);
    // Lets go of a document that no view shows, whatever its text: what is
    // not in its file is given up, as when it is closed without saving. At
    // once, or, while a save of it runs, as soon as that save ends, whatever
    // it ends in, with nothing more saved or asked; a view that takes the
    // document meanwhile keeps it. False, and nothing changes, while a view
    // shows it.
    bool drop(Document *document);
    // Both return at once and report through the document's state.
    void save(Document *document);
    // Naming a draft keeps its id, text and undo. Naming a document its own
    // file saves it there, and, in conflict, overwrites the file, as a save
    // dialog that asked to replace it has agreed to.
    void saveAs(Document *document, const QString &path);
    // The same, but only while the file at the path is still at `expected`.
    // With an expected version that does not exist, the file is only ever
    // created: one that appeared since, even while the save runs, is left as
    // it is and the save fails.
    void saveAs(Document *document, const QString &path, const DiskVersion &expected);
    // What a restore did with the entry's text: the file held it already;
    // the document shows it, unsaved or in conflict; or parch's own later
    // save left newer text in the file, which the document shows, and the
    // entry's text became a version in history.
    enum class Recovered { AlreadySaved, Shown, InHistory };
    // A document with the text of a recovery entry, under the entry's id and
    // path, or null when the entry cannot be taken: its id or file is already
    // open. The entry's edits are safe to save over the file only while the
    // file is what they were made on, or what parch's newest save wrote: then
    // the newer of the two texts is shown, and the older kept in history. A
    // named file that something else changed, removed or made unreadable
    // waits in conflict for a choice, as does a text history cannot keep. A
    // draft's own file is parch's alone, so a draft never waits in conflict
    // over it unless history cannot keep the older text, or the bytes of a
    // file that is not valid text.
    //
    // An entry whose file, or the file its path leads to, another document
    // has is declined and held: it stays in recovery exactly as it is, and
    // history is given its text as well, on a worker. While it is held,
    // opening gives its id to no document, so nothing writes over it or
    // removes it. Its path, asked for while it leads to another file, is
    // not opened. Once the path leads to its own file again, and no
    // document has that file, opening it, whichever way, restores the
    // entry as here and lets the hold go.
    Document *restore(const Recovery::Entry &entry, Recovered *recovered = nullptr);
    // Whether recovery holds text for this id that restore() declined.
    bool holdsRecovered(const QString &documentId) const;
    // Chooses what happens to a document in conflict. Reload takes the file's
    // text, Keep both saves the local text beside the file and takes the
    // file's text, and Overwrite replaces the file with the local text. When
    // the document's path leads to another file than its own, that file is
    // never written or taken: Overwrite and Reload are refused, Keep both
    // leaves the document waiting on its own text, Put here writes the text
    // at the document's own name in place of the link, and only Follow takes
    // that file, as the document's file from then on. A
    // draft's file is its own draft.md, which a copy has no place beside, so
    // Keep both is refused for a draft. The file is read afresh, so the choice
    // also fits a conflict that came from a failed save or a restart. A choice
    // that cannot be carried out returns false with the reason in refusal;
    // for a document in conflict the reason is also its lastError().
    bool resolveConflict(Document *document, ConflictChoice choice, QString *refusal = nullptr);
    // Reads again, off the UI thread, the file of a document that waits in
    // conflict because its file could not be read. Once it can be, what it
    // holds is taken as any change of the file is: the conflict ends when
    // it is what the text was made on, and the text is then saved over it.
    void readAgain(Document *document);
    // The local versions of every document. A save records its text here
    // after it succeeds, and Reload, Overwrite and restoreVersion record what
    // they are about to replace.
    History *history() const;
    // Replaces the text with a version from history, as one edit that a single
    // undo reverts. The document becomes modified and is saved like any edit.
    // The current text is recorded first, off the UI thread. If history
    // cannot write it because of an error, or does not answer within
    // HistoryWaitMs, nothing is replaced, so that the text is never lost; if
    // history is only paused by its size limit, the restore goes on, since
    // ordinary work continues while capture is paused. A version that is
    // missing or is not valid UTF-8 changes nothing.
    // Says whether the text was replaced, was already the version's, or was
    // left as it is, with the reason for that in refusal.
    enum class Restore { Restored, Unchanged, Refused };
    Restore restoreVersion(Document *document, const QString &versionId, QString *refusal = nullptr);
    // The same with the version's bytes read already, off the UI thread;
    // none when history no longer has it.
    Restore restoreRead(Document *document, const std::optional<QByteArray> &bytes, QString *refusal = nullptr);
    // Returns once no save is running, with each save's outcome applied.
    void waitForSaves();
    // The same, but for at most timeoutMs: false when a save is still running then.
    bool waitForSaves(int timeoutMs);
    // Whether a save of a document is still running: one of its text, a
    // naming, or a restore of bytes. Other work, such as a version kept in
    // history, is not a save.
    bool savesRunning() const;
    // Waits, for at most timeoutMs, until the record of which file is which
    // document is written; false when it is still being written then.
    bool waitForRegistry(int timeoutMs);
    QList<Document *> documents() const;
    // Whether the id is an unnamed draft's: one of the ids parch gives
    // drafts, and not a document's that has been named since. A draft
    // folder that a named document left behind is no draft.
    bool isUnnamedDraft(const QString &documentId) const;
    // The file a draft's text is kept in, once something is written in it.
    static QString draftFile(const AppPaths &paths, const QString &documentId);
    // The file the document's text is saved in: its own, or its draft file.
    QString fileOf(const Document *document) const;
    // How messages name the folder of a path, as the writer sees folders
    // named elsewhere; without it, by the folder's own name.
    void setFolderNames(std::function<QString(const QString &path)> names);
    // The folders the libraries watch, where a document's file renamed by
    // another program is looked for too. Asked on the UI thread.
    void setWatchedFolders(std::function<QStringList()> folders);
    // How long a file gone from its name, and found under another, is given
    // to come back, as when a program sets the old file aside and writes a
    // new one in its place, before the document follows it.
    static constexpr int RenameSettleMs = 100;
    // At most this many folders and entries in each are looked through.
    static constexpr int RenameFolders = 256;
    static constexpr int RenameEntries = 20000;
    // For tests: called with each of a named draft's assets, at its new
    // path, just before it is written there.
    static void setBeforePlacing(std::function<void(const QString &path)> hook);

    // The open document whose file is at this canonical path, if any.
    Document *documentOf(const QString &canonicalPath) const;
    // Whether the document's file may be moved now: it is a named file, no
    // save or choice of it runs, and it waits on no choice about its file.
    // False, with why in a few words, otherwise.
    bool canRelocate(const Document *document, QString *reason = nullptr) const;
    // The document's file has been moved to this canonical path, as the
    // library renames and moves files: the document follows it, keeping its
    // identity, text, undo, history and recovery, and is watched there. The
    // file is the same file unless `version` says what it is now, as for a
    // copy made on another file system.
    void relocate(Document *document, const QString &canonicalPath, const std::optional<DiskVersion> &version = {});
    // The id history keeps a file's versions under, as opening it would
    // give it, given now if it has none yet.
    QString documentIdFor(const QString &canonicalPath);

    // A document open read-only because its file is not UTF-8 takes the
    // text the writer chose to read the file as, in the named encoding,
    // with the convention given: it is editable from then on, undo leads no
    // further back, and nothing saves it until confirmEncoding(). `read` is
    // the version of the file the text was read from. False, with why in
    // refusal and nothing changed, for a document that is not read-only so,
    // or whose file is no longer that version.
    bool reopenAs(Document *document, const QString &text, const TextConvention &convention, const QString &encoding,
                  const DiskVersion &read, QString *refusal = nullptr);
    // The writer confirms that saving writes UTF-8 over the file: its
    // original bytes go to history first, off the UI thread, and then the
    // document saves as any does. `done` is called here with why not, when
    // history could not keep them or the file changed meanwhile.
    void confirmEncoding(Document *document, std::function<void(const QString &error)> done);
    // Restores a version from history that is not UTF-8 text, such as a
    // file's original bytes kept as it was first written as UTF-8: those
    // bytes go back into the file as they are, once history has what the
    // file and the text hold now, and the document then reads its file as
    // any not UTF-8 is read, read-only, for Reopen as… to read again. Off
    // the UI thread; `done` is called here with why not, should that be so:
    // a draft keeps UTF-8 text only, and a file changed meanwhile, or one
    // history could not keep the text of, is left as it is. A save of the
    // document that runs is waited for first. It runs as a save does, and
    // ends as one, with saveFinished(); text typed meanwhile is kept in
    // history before the file's bytes replace it.
    void restoreBytes(Document *document, const QString &versionId, std::function<void(const QString &error)> done);

    // A draft deleted from the library is set aside, in a hidden folder
    // beside the drafts that nothing lists, until it is put back or forgotten.
    static QString asideFolder(const AppPaths &paths, const QString &documentId);
    // Sets aside an unnamed draft that no document has open, once history
    // has its text: the bytes given, such as an open draft's text as it was
    // let go, else its file's. Off the UI thread; `done` is called here with
    // the draft's title, and why nothing was set aside, should that be so:
    // history did not keep the text, or the folder could not be moved. A
    // draft never written has nothing on disk to set aside.
    void setDraftAside(const QString &documentId, const std::optional<QByteArray> &bytes,
                       std::function<void(const QString &title, const QString &error)> done);
    // Puts a draft set aside back among the drafts; should its folder be
    // gone, its newest version in history becomes its text. Off the UI
    // thread; `done` is called here with why not, should that be so.
    void restoreDraft(const QString &documentId, std::function<void(const QString &error)> done);
    // Removes a draft set aside, off the UI thread.
    void forgetDraft(const QString &documentId);
    // Removes every draft set aside, as those a crash left, off the UI thread.
    void forgetDraftsSetAside();

signals:
    // A document is open: a new draft, a draft or file read, or one restored.
    void documentOpened(Document *document);
    // A save, or the naming of a document, reached its file.
    void documentSaved(Document *document);
    // A save of the document ended, with its outcome. The name is the one a
    // naming was asked to take, as given to saveAs, and empty for a plain
    // save; a naming of the document's own file says its name too. A naming
    // refused before it starts, or that finds nothing to write, ends before
    // saveAs returns. Said once the document shows the outcome, and after
    // any save that waited for this one has started. A naming that a newer
    // one replaced before it started never ends.
    void saveFinished(Document *document, const QString &name, SaveStatus status);
    void documentClosed(const QString &id);
    // Keep both has saved the local text to a file beside the original and
    // opened it as a document, so that a view can show it. The original
    // still holds that text as this is said, and takes its file's text
    // right after, so that a view can open the copy where it was in it.
    void conflictCopyOpened(Document *copy, Document *original);
    // Keep both made its copy, at this path, with the text as it was then,
    // but the document had moved on meanwhile: saved, named, no longer in
    // that conflict, or closed. The copy stays on disk and is not opened,
    // and the document, titled as it was when Keep both was chosen, is left
    // as it is.
    void conflictCopyLeft(const QString &title, const QString &copy);
    // A document that no view shows any more, kept until its text is in its
    // file, could not be saved there, or its file changed meanwhile: it needs
    // a view again so that the writer can see why and choose.
    void needsView(Document *document);
    // A draft was named, but its own file held other text that history
    // could not keep, so that text stays as a draft of its own, under the
    // id given, with the title given; the id is empty when its folder could
    // not be moved there, and it stays where it was.
    void draftKept(Document *named, const QString &draftId, const QString &title);
    // Something touched the file of an open document, as its watch saw it,
    // whatever the document makes of it; a view comparing with the file
    // reads it again.
    void diskChanged(Document *document);
    // A draft was named, but "assets" beside its file, or a folder in it, is
    // no folder of its own, such as a link: nothing went there, and the
    // draft's assets stay in the folder given.
    void assetsKept(Document *named, const QString &folder);
    // Text recovery held since the launch came back when its path, opened
    // again, led to its own file: the document is as restore() makes it.
    void heldRestored(Document *document, DocumentStore::Recovered how);
    // A document of parch's own was saved as a file, and is an ordinary
    // document now, kept as any other is.
    void scratchNamed(Document *document);
    // Another program renamed or moved an open document's file, within its
    // folder or into another watched one, and the document followed it,
    // keeping its identity, text, undo, history and recovery. A file moved
    // where nothing is watched is missing, as one deleted is.
    void renamedElsewhere(Document *document, const QString &from, const QString &to);

private:
    // Everything a save needs, as plain values, since it runs on another thread.
    struct SaveJob {
        QString target;
        QByteArray bytes;
        DiskVersion expected;
        Revision revision = 0;
        // The target is a draft's own file, which only parch writes: when it
        // is not there, as before the first save or after its folder went,
        // its folder is made first and the expected version taken after,
        // because a path in a missing folder has no version yet.
        bool savesDraft = false;
        // The document takes the target as its new name when the save succeeds.
        bool renames = false;
        // A naming replaces whatever is at the target when the save starts,
        // rather than an expected version.
        bool replacesWhatIsThere = false;
        // The canonical path a naming claims while it runs.
        QString claim;
        // The name a naming was asked to take, said again when the save ends.
        QString name;
        // The draft being named, whose assets go beside the target; its
        // text, the dialect it is read in and how it is written, so that
        // links to an asset placed under a new name can be rewritten.
        QString draftFolder;
        QString text;
        md::Options options;
        TextConvention convention;
        // The save is noted in recovery before it writes, with the content
        // it replaces, so that a restart knows the file it leaves as parch's.
        bool notesSave = false;
        QByteArray from;
        // The first save of a file since it was opened or reloaded: what it
        // replaces is kept in history first, unless history's newest version
        // holds it, and what it writes is kept too.
        bool keepsReplaced = false;
        // The text is put at the target's very name, in place of a link
        // there, which is never followed.
        bool putsHere = false;
        // The permission bits of the document's file as last seen, which a
        // file made again in its place is given; 0 when not known.
        quint32 mode = 0;
        // The document's own path, for a naming: a name that is that very
        // path, reached through a link to its folder, puts the text there
        // rather than writing through a link at the name.
        QString ownPath;
    };

    struct SaveOutcome {
        SaveResult result;
        // The draft's own file held text other than what was named, and
        // history could not keep it, so its folder stays, as a draft whose
        // title this is.
        bool keepDraft = false;
        QString keptTitle;
        // The draft's assets, left in its folder because a folder they
        // would go in beside the named file is not a folder of its own.
        QString assetsLeft;
        // The draft's assets that are beside the named file now, by their
        // names within its assets folder.
        QStringList assetsPlaced;
        // The draft's assets that took a free name beside the named file,
        // since another file had theirs, as moves from the name they would
        // have had there; the links to them that the named file has
        // rewritten, in the text that was asked to be saved; and the bytes
        // that were written.
        QList<DocumentLinks::Move> assetsRenamed;
        QList<DocumentLinks::Change> linksRewritten;
        QByteArray written;
        // A file made again found its name taken by something else.
        bool nameTaken = false;
        // The text was put at the document's own name, as Put this text
        // does, whichever way the save was asked for.
        bool put = false;
    };

    // What a comparison of the file with the document's last read found, as
    // plain values since it is made on another thread.
    struct DiskCheck {
        DiskVersion now;
        // Something is at the name itself, a link that leads nowhere included.
        bool entry = false;
        // Read only when the file is not what the document last read.
        LoadedFile loaded;
        // The file is gone from its name, and the very file, by device and
        // inode, is at this canonical path now, in its folder or another
        // that is watched: another program renamed or moved it.
        QString movedTo;
    };

    static SaveOutcome writeJob(const SaveJob &job, History *history, const Recovery &recovery,
                                const QString &documentId);
    // Looks at the file; one gone from its name is looked for in the
    // folders given, its own first.
    static DiskCheck checkJob(const QString &path, const DiskVersion &known, const QStringList &folders = {});
    // The canonical path of the file with the known device and inode
    // among the entries of the folders, or empty.
    static QString findMoved(const DiskVersion &known, const QStringList &folders);
    // The folders a renamed file is looked for in: its own, those of every
    // file the store watches, and those the libraries watch.
    QStringList renameFolders(const QString &path) const;
    // Another program renamed or moved the document's file: it follows.
    void followRename(Document *document, const QString &movedTo);
    // What Keep both found and made, off the UI thread: the file as it is
    // now, and the copy of the text beside it, made at a name nothing has,
    // and still that very file once made.
    struct KeptCopy {
        LoadedFile disk;
        SaveResult copy;
    };
    static KeptCopy keepBothJob(const QString &path, const QByteArray &local, const QString &stamp,
                                const QSet<QString> &open, quint32 ownMode);
    void keepBoth(Document *document);
    // What Follow found off the UI thread: the file the path leads to now, read.
    struct Followed {
        LoadedFile disk;
        // The file is one of parch's own, in one of its folders, which no
        // document ever takes.
        bool own = false;
    };
    static Followed followJob(const QString &path, const AppPaths &paths);
    // Runs work off the UI thread, as the store's own, waited for as it
    // goes, and hands its result to `then` here.
    template <typename Result>
    void runFiled(std::function<Result()> work, std::function<void(const Result &)> then);
    void follow(Document *document);
    // The document as its writer saw it when choosing to follow the link.
    struct Chosen {
        QString path;
        DiskVersion version;
        QString leadsTo;
        bool missing = false;
        Revision revision = 0;
    };
    // Whether the link can be followed to the file read: the document is
    // still as it was chosen for, and the file is the one shown, no file of
    // parch's own and no other document's. A refusal says why on the
    // document; one that has left the conflict is left as it is.
    bool mayFollow(Document *document, const Chosen &chosen, const Followed &followed);
    Document *adopt(Document *document);
    // An entry restore() declined, as recovery has it.
    struct Held {
        Recovery::Entry entry;
        // The path it is recorded at.
        QString path;
        // Its text going into history, on a worker.
        QFuture<void> kept;
    };
    void hold(const Recovery::Entry &entry, const QString &path);
    // Keeps the bytes in the document's history on a worker, unless its
    // newest version holds them already. `then` is called here once that
    // has ended, whether or not history took them.
    QFuture<void> keepInHistory(const QString &documentId, const QByteArray &bytes, History::Reason reason,
                                std::function<void()> then = {});
    // The same for a choice the writer waits on: the bytes are kept on a
    // worker, unless history's newest version holds them, and the answer is
    // waited for at most HistoryWaitMs. Failed when history did not answer
    // by then, as on a disk that stopped answering; the capture still lands
    // when it can.
    History::CaptureStatus keepInHistoryNow(const QString &documentId, const QByteArray &bytes, History::Reason reason);
    // The held entry recorded at this path.
    const Held *heldAt(const QString &path) const;
    // The path an entry is recorded at: its own, or for a draft named
    // after its last snapshot, the one the registry gives its id.
    QString recordedPath(const Recovery::Entry &entry) const;
    // restore(), for an entry recorded at `path`; `read` is its file, when
    // an opening has read it already.
    Document *restoreAt(const Recovery::Entry &entry, const QString &path, const LoadedFile *read,
                        Recovered *recovered);
    // Restores a held entry over its own file, read as an opening found it.
    Document *restoreHeld(const Held &held, const LoadedFile &read);
    // The document whose recorded path leads to another file now, in
    // conflict and with no text of its own; the one open already, if it is
    // open at that path.
    Document *openElsewhere(const QString &path, const QString &documentId, const QString &target, QString *error);
    // The path as parch records a document's file: as given, tidied as
    // text, or with its folder resolved, whichever the registry knows
    // first, else spelled clean. `canonical` is the path's canonical name.
    // The folder is resolved only when the first two miss, and once.
    QString recordedSpelling(const QString &path, const QString &canonical) const;
    // open(), with the path's canonical name and its recorded spelling
    // already asked for.
    Document *openAt(const QString &path, const QString &canonical, const QString &spelled, QString *error);
    // A document for a file parch has just made with this text: taken as
    // the file that was made, never opened again by its name. Null, with
    // the reason, when a document is already at that name.
    Document *adoptCreated(const DiskVersion &made, const QString &text, const TextConvention &convention,
                           QString *error);
    Document *handOut(Document *document);
    Document *documentAt(const QString &canonicalPath) const;
    Document *documentWithId(const QString &id) const;
    QString draftFolder(const QString &id) const;
    QString canonicalDrafts() const;
    QString draftIdAt(const QString &canonicalPath) const;
    void nameAs(Document *document, const QString &path, const std::optional<DiskVersion> &expected);
    // Puts the text at the document's own name, in place of the link that
    // leads elsewhere from there; the name is the one a naming asked for.
    void putHere(Document *document, const QString &name);
    void run(Document *document, const SaveJob &job);
    void finish(Document *document, const SaveJob &job, SaveOutcome outcome);
    // A restore of bytes ended, as a save ends: the file is taken when it
    // was restored, saveFinished() said, and a document given up meanwhile closed.
    void restoredBytes(Document *document, const LoadedFile &loaded, bool restored, const QString &error,
                       const std::function<void(const QString &error)> &done);
    void refuse(Document *document, const QString &error);
    void closeIfUnused(Document *document);
    // Asks for a view for a document that has none and needs the writer.
    void askForView(Document *document);
    void close(Document *document);
    void checkDisk(Document *document);
    void applyCheck(Document *document, const DiskCheck &check);
    // Replaces the buffer's text with the file's by one edit of the changed
    // middle, and makes the file's version the document's.
    void takeFile(Document *document, const LoadedFile &file);
    // The text becomes `after` by one edit of the middle that differs,
    // grown to whole graphemes.
    static void replaceChanged(SourceBuffer *buffer, const QString &after);
    void enterConflict(Document *document, const QString &error, bool missing = false, bool unreadable = false);
    // The document's path leads to another file than its own now: it waits
    // in conflict, saying so.
    void enterElsewhere(Document *document, const QString &target);
    void showElsewhere(Document *document, const QString &target);
    // Forgets parch's earlier saves of a document whose file holds, or will
    // hold, content parch did not write: they tell nothing about it, and
    // content one of them wrote can come back from another program.
    void forgetSaves(const QString &documentId) const;
    // The reason stays visible while the document waits for another choice.
    bool refuseChoice(Document *document, const QString &error, QString *refusal);
    void watchPath(const QString &path);
    QString shownFolder(const QString &path) const;
    // Why a document with no text of its own cannot be saved or kept.
    static QString textlessReason(const Document *document);
    // Why a file that is there cannot be read, as the writer is told it: by
    // its name and its folder's, or its folder's when that is what stops it.
    QString cannotRead(const QString &path, const LoadedFile &loaded) const;
    // Why a file is not opened, worded whole: gone, no regular file, or not readable.
    QString cannotOpen(const QString &path, const LoadedFile &loaded) const;
    void loadRegistry();
    // Lets go, off the UI thread, of the identities nothing is left to tie
    // to: their file is gone, and neither history nor recovery holds
    // anything under their id, nor does an open document or held text.
    void pruneRegistry();
    // Writes the registry as it is now, on a thread of its own, after any
    // write asked for before.
    void writeRegistry();
    void writeRegistryFile(const QJsonObject &entries);

    AppPaths m_paths;
    std::unique_ptr<History> m_history;
    QList<Document *> m_documents;
    QHash<Document *, int> m_views;
    // Documents whose last view closed while they had unsaved text. They stay
    // until they are clean and then close.
    QSet<Document *> m_unviewed;
    // Documents given up while a save of them ran, which close once it ends.
    QSet<Document *> m_dropping;
    // Canonical path to document id, as kept in AppPaths::registryFile().
    QHash<QString, QString> m_registry;
    // The file there could not be read, nor moved aside to keep it. Only
    // the registry's writer reads it, once the store is made.
    std::atomic<bool> m_registryDamaged{false};
    // One thread, so that the registry's writes land in the order asked.
    QThreadPool m_registryWriter;
    // The text a Reload, an Overwrite or a restore is about to replace goes
    // to history on a thread of its own, so that the bounded wait for it is
    // a wait for history alone, never for the saves and looks at files the
    // shared pool runs.
    QThreadPool m_historyKeeper;
    // Targets of names being saved, by canonical path. A document owns its new
    // name from the moment the save starts, so that no other document can
    // open or take the same file before the save finishes.
    QHash<QString, Document *> m_claims;
    // Every save running, every Keep both, whose copy is a save too, every
    // Follow, and every text going into history on a worker.
    QList<QFutureWatcherBase *> m_running;
    QList<QFutureWatcher<DiskCheck> *> m_checks;
    FileWatch m_watch;
    std::function<QString(const QString &path)> m_folderNames;
    std::function<QStringList()> m_watchedFolders;
    // The entries recovery holds, by document id.
    QHash<QString, Held> m_held;
};
