#pragma once

#include "documentlinks.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>

class Document;
class DocumentStore;
class History;
class Settings;

// Makes, renames, moves and trashes the files and folders of the library,
// as its menu, keys and drags ask, and keeps what parch knows of them in
// step. Only what lies inside one of the library's folders is organized;
// the folders themselves are added and removed, not moved.
//
// A move, a rename among them, never replaces anything: a name taken is
// said, and nothing changes. An open document whose file moves follows it,
// with its identity, text, undo, history and recovery, as the store moves
// it; tabs not read yet, recent files and reading positions follow through
// relocated(). Before a document or a folder moves, the library's Markdown
// documents are read off the UI thread for relative links, images and
// includes that the move would break, the moved documents' own among them,
// and when there are any the move waits while the writer reviews them: it
// can update them, leave them, or not move at all. Open documents are
// updated through their SourceBuffer, as one undo step each; others through
// the safe save, after history has kept what they held.
//
// Trash moves to the desktop's trash, from where it can be put back, once
// the writer has confirmed it. What goes wrong, and what was done, is said
// through problem() and news().
class FileOrganizer : public QObject {
    Q_OBJECT
    // Something is being looked into off the UI thread.
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    // A move waiting for its references to be reviewed, as {title, detail,
    // files, changes, rows}, each row {file, folder, line, before, after};
    // empty while none waits.
    Q_PROPERTY(QVariantMap review READ review NOTIFY reviewChanged)
    // A trash waiting to be confirmed, as {path, name, folder, documents,
    // open}; empty while none waits.
    Q_PROPERTY(QVariantMap trashing READ trashing NOTIFY trashingChanged)
    // The folders a move can go to, as {path, label, depth}, once listed.
    Q_PROPERTY(QVariantList folders READ folders NOTIFY foldersChanged)

public:
    // How many folders the move dialog lists at most.
    static constexpr int MaxFolders = 5000;

    FileOrganizer(DocumentStore *store, Settings *settings, QObject *parent = nullptr);
    ~FileOrganizer() override;

    bool busy() const;
    QVariantMap review() const;
    QVariantMap trashing() const;
    QVariantList folders() const;

    // Why a name cannot be given to a file or folder, in a few words; empty
    // when it can.
    Q_INVOKABLE QString nameProblem(const QString &name) const;

    // The dialect a Markdown file is read in when its links are found, by
    // its path; safe on any thread.
    using Dialects = std::function<md::Options(const QString &path)>;
    // Asked once at the start of each move, for the documents' settings as
    // they are then; without it, every file is read in the defaults.
    void setDialects(std::function<Dialects()> dialects) { m_dialects = std::move(dialects); }
    // Whether the path lies inside one of the library's folders, so that it
    // can be renamed, moved or trashed.
    Q_INVOKABLE bool organizes(const QString &path) const;
    // The library folder that holds the path, the outermost when several
    // do; empty for none.
    Q_INVOKABLE QString libraryOf(const QString &path) const;

    // Makes an empty document in the folder, and then created() says where.
    // A name with no .md, .markdown or .txt at its end gets .md.
    Q_INVOKABLE void newFile(const QString &folder, const QString &name);
    Q_INVOKABLE void newFolder(const QString &folder, const QString &name);
    // A document renamed keeps its kind: a name without a text document's
    // ending takes the one it had.
    Q_INVOKABLE void rename(const QString &path, const QString &name);
    Q_INVOKABLE void move(const QString &path, const QString &folder);
    // The answer to a review: move and update the references, or move and
    // leave them as they are.
    Q_INVOKABLE void confirmMove(bool updateReferences);
    Q_INVOKABLE void cancelMove();
    // Counts what a folder holds and asks, through trashing, to confirm.
    Q_INVOKABLE void askToTrash(const QString &path);
    Q_INVOKABLE void confirmTrash();
    Q_INVOKABLE void cancelTrash();
    // Lists, off the UI thread, the folders a move can go to.
    Q_INVOKABLE void listFolders();

    // Returns once nothing runs off the UI thread; for tests and the end.
    void waitForWork();

signals:
    void busyChanged();
    void reviewChanged();
    void trashingChanged();
    void foldersChanged();
    void problem(const QString &title, const QString &detail);
    void news(const QString &title, const QString &detail);
    // A new document is at this path, for the window that asked to open.
    void created(const QString &path);
    // What was at `from` is at `to` now: a file, or a folder with all in it.
    void relocated(const QString &from, const QString &to);
    // An open document followed its file.
    void documentMoved(Document *document);
    void trashed(const QString &path);

private:
    struct FilePlan {
        QString path;
        QList<DocumentLinks::Change> changes;
    };
    struct Plan {
        QString from;
        QString to;
        QString library;
        bool folder = false;
        // The documents the move takes along.
        int documents = 0;
        QList<FilePlan> files;
        // What each file is read in, as the settings were when the move began.
        Dialects dialects;
    };
    struct Rewrite {
        QString path;
        QString oldPath;
        QString documentId;
    };
    struct Rewritten {
        int files = 0;
        int changes = 0;
        QStringList failed;
    };

    void startMove(const QString &from, const QString &to);
    static Plan planJob(Plan plan, QHash<std::pair<quint64, quint64>, QString> open, std::shared_ptr<std::atomic<bool>> cancel);
    // Moves on disk and lets open documents follow; false, having said why,
    // when it could not.
    bool perform(const Plan &plan);
    void updateReferences(const Plan &plan);
    static Rewritten rewriteJob(QList<Rewrite> files, QString library, QList<DocumentLinks::Move> moves, History *history,
                                Dialects dialects);
    // Says what a move did: what it took along, and what became of the
    // references to what moved.
    void finishMove(const Plan &plan, bool updated, int changes, int files, const QStringList &failed = {});
    void setBusy(int delta);
    // The open documents a move of this path takes along.
    QList<Document *> documentsUnder(const QString &canonical) const;
    QString shownName(const QString &path) const;

    DocumentStore *m_store;
    Settings *m_settings;
    std::function<Dialects()> m_dialects;
    int m_busy = 0;
    std::optional<Plan> m_review;
    QVariantMap m_trashing;
    QVariantList m_folders;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    QThreadPool m_pool;
};
