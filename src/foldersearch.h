#pragma once

#include "apppaths.h"
#include "textsearch.h"

#include <QAbstractListModel>
#include <QList>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVariantMap>
#include <memory>

class DocumentStore;
class Settings;

// Search across the library, for one window: the text of every document in
// the library's folders, and of every draft, for what was typed. Documents
// open in parch are searched as their text is now, unsaved edits and all,
// rather than as their files hold it.
//
// A walk lists the documents and hands them, a batch at a time, to a small
// pool of workers, which read and search them; nothing asks the file system
// or matches text on the UI thread. What the workers find is taken here a
// little at a time, in the order the walk found the documents, so that the
// results stream in grouped by document, the library's order kept, and no
// frame waits on many of them at once. A new search, or cancel(), stops the
// one that runs.
//
// Each row is one match: its document, its line, and the line around it with
// the match marked. A document that cannot be read, or is not UTF-8 text, is
// passed over and counted. What a document's file held is kept for a repeat
// search in a small cache, by path, modification time and size, shared by
// every window. It lets the least recently used go first, but never what
// the search running has used, so that a repeat search of a library larger
// than the cache reads again only what the cache could not hold.
class FolderSearch : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString pattern READ pattern WRITE setPattern NOTIFY queryChanged)
    Q_PROPERTY(bool caseSensitive READ caseSensitive WRITE setCaseSensitive NOTIFY queryChanged)
    Q_PROPERTY(bool wholeWord READ wholeWord WRITE setWholeWord NOTIFY queryChanged)
    Q_PROPERTY(bool regex READ regex WRITE setRegex NOTIFY queryChanged)
    // Why the pattern cannot be searched for; empty when it can.
    Q_PROPERTY(QString error READ error NOTIFY queryChanged)
    Q_PROPERTY(bool searching READ searching NOTIFY progressChanged)
    // The search ran to its end, rather than being cancelled or not yet run.
    Q_PROPERTY(bool finished READ finished NOTIFY progressChanged)
    Q_PROPERTY(int filesSearched READ filesSearched NOTIFY progressChanged)
    Q_PROPERTY(int filesMatched READ filesMatched NOTIFY progressChanged)
    Q_PROPERTY(int matchCount READ matchCount NOTIFY progressChanged)
    // Documents and folders passed over because they could not be read.
    Q_PROPERTY(int unreadable READ unreadable NOTIFY progressChanged)
    // There were more matches, or documents, than are shown.
    Q_PROPERTY(bool truncated READ truncated NOTIFY progressChanged)
    // The folders searched, for the window to name.
    Q_PROPERTY(int folderCount READ folderCount NOTIFY progressChanged)

public:
    enum Role {
        SectionRole = Qt::UserRole + 1,
        PathRole,
        DocumentIdRole,
        TitleRole,
        FolderRole,
        LineRole,
        ColumnRole,
        StartRole,
        LengthRole,
        BeforeRole,
        MatchRole,
        AfterRole,
        LineTextRole,
    };
    Q_ENUM(Role)

    // How many documents a worker takes at a time, how many workers there
    // are, how many rows are shown at most, and how many for one document.
    static constexpr int BatchFiles = 32;
    static constexpr int Workers = 3;
    static constexpr int MaxRows = 20000;
    static constexpr int MaxRowsPerFile = 100;
    // A document larger than this is not searched.
    static constexpr qint64 MaxFileBytes = 16 * 1024 * 1024;
    // How often what the workers found is taken, and how many rows at most
    // each time.
    static constexpr int TakeEveryMs = 16;
    static constexpr int RowsPerTake = 200;
    // How much text the cache keeps, in UTF-16 units.
    static constexpr qint64 CacheUnits = 16 * 1024 * 1024;

    FolderSearch(const AppPaths &paths, DocumentStore *store, Settings *settings, QObject *parent = nullptr);
    ~FolderSearch() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString pattern() const;
    void setPattern(const QString &pattern);
    bool caseSensitive() const;
    void setCaseSensitive(bool on);
    bool wholeWord() const;
    void setWholeWord(bool on);
    bool regex() const;
    void setRegex(bool on);
    QString error() const;
    bool searching() const;
    bool finished() const;
    int filesSearched() const;
    int filesMatched() const;
    int matchCount() const;
    int unreadable() const;
    bool truncated() const;
    int folderCount() const;

    // Searches for the query as it is now, from the start, dropping what
    // the last search found.
    Q_INVOKABLE void start();
    // Stops the search that runs, keeping what it found so far.
    Q_INVOKABLE void cancel();
    // What a section of the results is: a document's title, its folder, its
    // path or draft id, and how many matches it has, as
    // {title, folder, path, documentId, matches}.
    Q_INVOKABLE QVariantMap section(const QString &section) const;
    // A row's values by role name, for opening it.
    Q_INVOKABLE QVariantMap row(int row) const;

    // A document's text from the cache, or else from its file, which is
    // then kept in the cache; false for one that cannot be read, is larger
    // than MaxFileBytes or is not UTF-8 text. `size` and `modified`, in
    // nanoseconds, are the file's as a walk found it. `search`, a search's
    // own number, keeps what that search has used from making room for the
    // rest it reads; 0 for a read that is no search. Safe on any thread.
    static bool readText(const QString &path, qint64 size, qint64 modified, QString *text, quint64 search = 0);
    // How many times a document's file has been read, rather than found in
    // the cache, since the process began; for tests.
    static quint64 fileReads();
    // Empties the cache, for tests.
    static void clearCache();

signals:
    void queryChanged();
    void progressChanged();

private:
    struct Run;
    struct Item;
    struct Hit;
    struct FileResult;
    struct Batch;
    struct Section {
        QString title;
        QString folder;
        QString path;
        QString documentId;
        int matches = 0;
    };
    struct Row {
        QString section;
        int line = 0;
        int column = 0;
        int start = 0;
        int length = 0;
        QString before;
        QString match;
        QString after;
        QString lineText;
    };

    static void walkJob(const std::shared_ptr<Run> &run, QThreadPool *pool);
    static void searchJob(const std::shared_ptr<Run> &run, int sequence, const QList<Item> &items);
    static FileResult searchItem(const Run &run, const Item &item, bool *read);
    void take();
    void stop();
    void queryEdited();

    AppPaths m_paths;
    DocumentStore *m_store;
    Settings *m_settings;
    TextQuery m_query;
    QString m_error;
    std::shared_ptr<Run> m_run;
    QTimer m_taker;
    QList<Row> m_rows;
    QHash<QString, Section> m_sections;
    // The next batch to take, in the walk's order.
    int m_nextBatch = 0;
    bool m_searching = false;
    bool m_finished = false;
    int m_filesSearched = 0;
    int m_filesMatched = 0;
    int m_matchCount = 0;
    int m_unreadable = 0;
    bool m_truncated = false;
    int m_folderCount = 0;
    QThreadPool m_pool;
};
