#pragma once

#include "apppaths.h"

#include <QAbstractItemModel>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QModelIndexList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

class Document;
class DocumentStore;
class QThread;
class RecentFiles;
class Settings;

// The watch a LibraryWatch keeps on a thread of its own: it adds folders as
// asked there, and says which it could add.
class LibraryFolderWatcher : public QFileSystemWatcher {
    Q_OBJECT
public:
    using QFileSystemWatcher::QFileSystemWatcher;
    void add(const QString &path);
    // For tests: called on the watch's thread as a folder is added.
    static std::function<void(const QString &path)> beforeWatching;

signals:
    void added(const QString &path);
};

// What the libraries of every window share, one for each document store:
// one watch of the folders they have open and of the drafts folder, and one
// look at the drafts on disk for all of them. The watch lives on a thread of
// its own, since adding a folder to it asks the file system, which can be
// slow to answer; what it sees is said on the thread the libraries live on.
class LibraryWatch : public QObject {
    Q_OBJECT
public:
    struct DiskDraft {
        QString id;
        QString title;
        QDateTime modified;
    };
    // The one the libraries of this store and pool share, made for the first
    // and gone with the last. The pool must outlive every library given it.
    static LibraryWatch *take(const AppPaths &paths, DocumentStore *store, QThreadPool *pool);
    static void give(LibraryWatch *watch);
    // For tests: called on the watch's own thread as a folder is added to it.
    static void setBeforeWatching(std::function<void(const QString &path)> hook);

    // Watches a folder for as many libraries as ask, until as many let it go.
    void watch(const QString &folder);
    void unwatch(const QString &folder);
    // The folders watched for any library.
    QStringList folders() const;
    // The drafts on disk, as the last look at them found them.
    const QHash<QString, DiskDraft> &drafts() const { return m_drafts; }
    // Looks at the drafts on disk again, once the look still running ends.
    void scanDrafts();
    bool scanning() const { return m_scanning; }

signals:
    // Something in a watched folder changed.
    void folderChanged(const QString &path);
    // A folder asked for is watched now: a change made before then is
    // caught by one more listing.
    void folderWatched(const QString &path);
    // A look at the drafts on disk ended.
    void draftsScanned();

private:
    LibraryWatch(const AppPaths &paths, DocumentStore *store, QThreadPool *pool);
    ~LibraryWatch() override;
    static QList<DiskDraft> scan(const AppPaths &paths, bool *folderThere);

    AppPaths m_paths;
    QThreadPool *m_pool;
    int m_users = 0;
    QThread *m_thread;
    LibraryFolderWatcher *m_watcher;
    QHash<QString, int> m_counts;
    QHash<QString, DiskDraft> m_drafts;
    bool m_scanning = false;
    bool m_scanAgain = false;
    bool m_draftsWatched = false;
};

// What one window's library shows, as a tree for a QML TreeView. At the top
// are two sections, Drafts and Recent, and then each folder the settings
// keep for the library, in the order they were added.
//
// Drafts holds every unnamed draft, those in AppPaths::drafts() and those
// open, each titled as its document is, the newest first. Recent holds the
// recent files, the newest first, each marked when nothing is at its path
// any more.
//
// A folder is listed when it is first expanded, one folder at a time, off
// the UI thread: its subfolders and then its .md, .markdown and .txt files,
// each sorted ignoring case, with hidden entries left out. A folder that is
// not there, or cannot be read, holds one error row that says so. While
// expanded, a folder is watched and listed again, once a burst of changes
// has settled, so that the tree stays true to the disk; a listing that ends
// after its folder was collapsed or removed is dropped.
//
// Everything that asks the file system runs on the pool it is given, never
// on the pool that saves documents, so that a folder that never answers can
// hold up the library but never a save. Each folder has at most one job on
// the pool, and the drafts and the recent files one each, so that nothing
// piles up behind a job that does not end.
//
// The filter keeps the rows whose names hold it, ignoring case, with their
// ancestors and what lies beneath them, and the two sections always. While
// there is a filter, the folders beneath the library's folders that are not
// listed yet are listed for it in the background, on the same pool and one
// job per folder, at most SearchJobs at a time, so that what they hold can
// match too: at most SearchDepth folders below a library folder and at most
// SearchFolders folders for one filter, leaving out hidden entries and links
// that lead back into a folder listed already. Folders listed only for the
// filter are not watched, and once the field is emptied they are not listed
// any more, unless the view has them open. A listing that lands while the
// field has text is used, whichever filter asked for it. A new filter drops what the last
// one left to do; while the field keeps text, it reuses what was listed.
class LibraryModel : public QAbstractItemModel {
    Q_OBJECT
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    // How many rows the filter matches by their own names; 0 without a filter.
    Q_PROPERTY(int matchCount READ matchCount NOTIFY matchCountChanged)
    // Folders the filter will use are still being listed, its own or an
    // earlier filter's, for at most SearchingMs after the filter was set.
    Q_PROPERTY(bool searching READ searching NOTIFY searchingChanged)
    // How many folders the filter has listed, or has still to list.
    Q_PROPERTY(int searchListings READ searchListings NOTIFY searchingChanged)

public:
    // Beside the name, path, kind and missing of every row: a draft's
    // document id, whether a folder is being listed, and which section a
    // section is, "drafts" or "recent".
    enum Role { NameRole = Qt::UserRole + 1, PathRole, KindRole, MissingRole, DocumentIdRole, LoadingRole, SectionRole };
    Q_ENUM(Role)

    // How long a folder's changes must pause before it is listed again.
    static constexpr int SettleMs = 100;
    // How far a filter lists folders, and how many it lists at once.
    static constexpr int SearchDepth = 8;
    static constexpr int SearchFolders = 2000;
    static constexpr int SearchJobs = 2;
    // How long a filter says it is searching at most, so that a folder that
    // never answers does not keep it saying so; what lands later still shows.
    static constexpr int SearchingMs = 5000;
    // At most this many rows of a listing are put in a folder in one turn of
    // the event loop; the rest follow as many at a time in the turns after,
    // so that a folder of many thousand entries never holds up a frame.
    static constexpr int ListingChunk = 1000;

    // The pool must outlive the model.
    LibraryModel(const AppPaths &paths, DocumentStore *store, RecentFiles *recent, Settings *settings,
                 QThreadPool *pool, QObject *parent = nullptr);
    ~LibraryModel() override;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    // A folder may hold something until a listing finds it empty, and again
    // once it is no longer watched.
    bool hasChildren(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
    // A folder not listed yet is listed when asked, as a view does when it is expanded.
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    QString filter() const;
    void setFilter(const QString &filter);
    int matchCount() const;
    bool searching() const;
    int searchListings() const;
    // How many folders wait to be listed for the filter.
    int searchQueued() const;

    // Adds a folder to the library, for every window, and keeps it in the settings.
    Q_INVOKABLE void addFolder(const QString &path);
    Q_INVOKABLE void removeFolder(const QString &path);
    // A view expanded or collapsed a row. An expanded folder is listed again
    // and watched; collapsing one stops watching it and everything beneath.
    Q_INVOKABLE void setExpanded(const QModelIndex &index, bool expanded);
    // The rows the filter matches by their own names, so that a view can
    // expand what holds them.
    Q_INVOKABLE QModelIndexList matches() const;
    // The same, nearest the top first, and at most `limit` of them.
    Q_INVOKABLE QModelIndexList nearestMatches(int limit) const;
    // The folders watched now for this library.
    QStringList watchedFolders() const;
    // The jobs this model has on its pool, the look at the drafts it shares included.
    int jobsInFlight() const;
    // What it shares with the other libraries of its store.
    LibraryWatch *shared() const { return m_shared; }

signals:
    void filterChanged();
    void matchCountChanged();
    void searchingChanged();
    // A listing for the filter found `count` rows that match among what
    // the folder holds, which a view may open to show.
    void matchesListed(const QModelIndex &folder, int count);

private:
    enum class Kind { Section, Folder, Document, Draft, Error };
    enum class Listing { None, Pending, Done };
    struct Node;
    struct Entry {
        QString name;
        bool folder = false;
    };
    struct FolderListing {
        bool ok = false;
        // Only whether the folder is there was asked, and it is.
        bool probed = false;
        // The folder itself is not there, or is not a folder.
        bool missing = false;
        QString error;
        QList<Entry> entries;
        // The folder's device and inode, whatever link led to it.
        QString identity;
    };
    // A folder the filter is to list, and how far below its library folder it is.
    struct Search {
        QString path;
        int depth = 0;
    };
    using DiskDraft = LibraryWatch::DiskDraft;
    // What a row shows, as plain values, before it is given a node.
    struct Row {
        Kind kind = Kind::Section;
        QString name;
        QString path = {};
        QString documentId = {};
        bool missing = false;
        QDateTime order = {};
    };
    // Within one parent, what makes a row the same row when the parent is built again.
    using Key = std::pair<int, QString>;
    // A listing put in its folder a chunk at a time: its rows, and how many are in.
    struct Staged {
        Node *folder = nullptr;
        QList<Row> rows;
        qsizetype ready = 0;
        bool ok = false;
        QString identity;
    };

    static FolderListing listFolder(const QString &path);
    static FolderListing examine(const QString &path, bool probeOnly);
    static std::unique_ptr<Node> makeNode(const Row &row);
    static QString kindName(Kind kind);
    static Key keyOf(Kind kind, const QString &name, const QString &path, const QString &documentId);
    static Key keyOf(const Node &node);
    static Key keyOf(const Row &row);
    static bool sameEntries(const Node *folder, const QList<Entry> &entries);

    Node *nodeAt(const QModelIndex &index) const;
    QModelIndex indexOf(const Node *node) const;
    bool inView(const Node *node) const;
    void changedData(Node *node);

    // Replaces a node's children, keeping the nodes that are still there with
    // their rows, listings and expansion, and shows what the filter wants.
    void replaceChildren(Node *parent, const QList<Row> &rows);
    // Works out which rows the filter keeps and shows exactly those.
    void applyFilter();
    bool computeWanted(Node *node, bool ancestorMatches, int *matches);
    // Brings a node's shown rows in line with what is wanted, announcing each change.
    void sync(Node *node);
    // The same for a node not in view, which announces nothing.
    void fill(Node *node);
    void renumber(Node *node, int from);
    // A node left the view: neither it nor anything beneath is expanded,
    // watched or waiting for a listing any more.
    void forget(Node *node);

    void list(Node *folder);
    void probe(Node *folder);
    void request(Node *folder, bool probeOnly);
    void startRequest(Node *folder, quint64 request);
    void startJob(const QString &path);
    void applyListing(quint64 request, const FolderListing &listing);
    // Puts the next chunk of a staged listing in its folder.
    void applyChunk(quint64 request);
    // What follows once the whole of a listing is in its folder.
    void listingApplied(Node *folder, bool ok, const QString &identity);
    // Has the shared watch watch a folder this library shows open.
    void startWatching(const QString &path);
    void watch(Node *folder);
    void unwatch(Node *folder);
    void listChangedFolders();
    void findExpanded(Node *node, const QString &path, QList<Node *> *found) const;

    // Starts listing for the filter what is not listed yet, from the
    // library's folders down, dropping what an earlier filter left to do.
    void startSearch();
    void searchBeneath(Node *node, int depth);
    void queueSearch(Node *folder, int depth);
    void pumpSearch();
    void applySearchListing(const Search &search, const FolderListing &listing);
    // The listed folders the view does not have open, and so does not
    // watch, go back to not listed: what they held may have changed since,
    // and the next search looks in them afresh.
    void unlistUnwatched(Node *node);
    void sayWhetherSearching();
    // The folder at a path beneath one of the library's folders, if it has a node.
    Node *folderAt(const QString &path) const;
    int depthOf(const Node *node) const;
    void collectIdentities(const Node *node);

    void syncFolders();
    void rebuildDrafts();
    void updateDraft(Document *draft);
    void rebuildRecent();
    void checkRecent();
    void follow(Document *document);

    AppPaths m_paths;
    DocumentStore *m_store;
    RecentFiles *m_recent;
    Settings *m_settings;
    QThreadPool *m_pool;
    std::unique_ptr<Node> m_root;
    Node *m_draftsSection = nullptr;
    Node *m_recentSection = nullptr;
    QString m_filter;
    int m_matchCount = 0;
    QStringList m_folders;

    quint64 m_lastRequest = 0;
    QHash<quint64, Node *> m_requests;
    // The requests that only ask whether a folder is there.
    QSet<quint64> m_probes;
    // The folders with a job on the pool, and how many jobs it has of this model's.
    QSet<QString> m_running;
    int m_jobs = 0;
    // The listings being put in their folders a chunk at a time, by request.
    QHash<quint64, Staged> m_staged;
    LibraryWatch *m_shared;
    QHash<QString, int> m_watchCount;
    // The folders this library has the shared watch watch, in the order asked.
    QStringList m_watching;
    QSet<QString> m_changed;
    QTimer m_settle;

    // When each open draft with no file yet was first seen, for its order.
    QHash<QString, QDateTime> m_firstSeen;

    QHash<QString, bool> m_missing;
    bool m_checking = false;
    bool m_checkAgain = false;

    // The listing done for the filter: what waits, how many jobs run, which
    // filter they are for, every folder taken on for it, and the folders
    // listed already, by device and inode.
    QList<Search> m_searchQueue;
    // The current filter's jobs that run, and the folders any filter's run for.
    int m_searchRunning = 0;
    QSet<QString> m_searchJobs;
    quint64 m_searchGeneration = 0;
    // Runs from when the filter was set, for SearchingMs.
    QTimer m_searchingFor;
    QSet<QString> m_searchPaths;
    QSet<QString> m_searchSeen;
    bool m_searchingSaid = false;
};
