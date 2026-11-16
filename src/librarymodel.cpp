#include "librarymodel.h"

#include "document.h"
#include "documentstore.h"
#include "pathtext.h"
#include "recentfiles.h"
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QtConcurrent>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>

struct LibraryModel::Node {
    Kind kind = Kind::Section;
    QString name;
    // A folder's or a file's full path; empty for a section, a draft and an error.
    QString path;
    QString documentId;
    bool missing = false;
    Node *parent = nullptr;
    // Every child, in order, and those the filter keeps, in the same order.
    std::vector<std::unique_ptr<Node>> children;
    QList<Node *> shown;
    // Where the node is among its parent's shown children, while it is shown.
    int row = -1;
    bool wanted = true;
    // Drafts are ordered by when they were last written.
    QDateTime order;
    // A folder's.
    Listing listing = Listing::None;
    // The last listing could not be made.
    bool failed = false;
    quint64 request = 0;
    bool expanded = false;
    // A listed folder's device and inode.
    QString identity;
};

namespace {
// How much of a draft is read for its title.
constexpr qint64 titleBytes = 4096;

bool isTextDocument(const QString &name)
{
    for (const char *ending : {".md", ".markdown", ".txt"})
        if (name.endsWith(QLatin1String(ending), Qt::CaseInsensitive) && name.size() > qsizetype(std::strlen(ending)))
            return true;
    return false;
}

// Orders names as people read them: runs of digits by their value, so
// "chapter 2" comes before "chapter 10", and the text between them without
// regard to case, in the locale's order. Qt's collator does the first only
// where ICU is available, which a plain C locale is not.
int naturalCompare(const QString &a, const QString &b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a.at(i).isDigit() && b.at(j).isDigit()) {
            qsizetype endA = i, endB = j;
            while (endA < a.size() && a.at(endA).isDigit()) ++endA;
            while (endB < b.size() && b.at(endB).isDigit()) ++endB;
            // Leading zeros do not change a number's value.
            qsizetype startA = i, startB = j;
            while (startA + 1 < endA && a.at(startA).digitValue() == 0) ++startA;
            while (startB + 1 < endB && b.at(startB).digitValue() == 0) ++startB;
            if (endA - startA != endB - startB) return endA - startA < endB - startB ? -1 : 1;
            for (qsizetype k = 0; k < endA - startA; ++k) {
                const int da = a.at(startA + k).digitValue(), db = b.at(startB + k).digitValue();
                if (da != db) return da < db ? -1 : 1;
            }
            i = endA;
            j = endB;
            continue;
        }
        qsizetype endA = i, endB = j;
        while (endA < a.size() && !a.at(endA).isDigit()) ++endA;
        while (endB < b.size() && !b.at(endB).isDigit()) ++endB;
        const QString textA = a.mid(i, endA - i).toCaseFolded(), textB = b.mid(j, endB - j).toCaseFolded();
        if (const int order = QString::localeAwareCompare(textA, textB); order != 0) return order;
        // Equal text of different lengths cannot happen without the runs
        // ending at different places; compare them as text then.
        if (textA != textB) return textA < textB ? -1 : 1;
        i = endA;
        j = endB;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}
} // namespace

TestHook<const QString &> LibraryFolderWatcher::beforeWatching;

void LibraryFolderWatcher::add(const QString &path)
{
    beforeWatching(path);
    if (addPath(path)) emit added(path);
}

namespace {
// The shared watches, by the store and pool their libraries use.
using SharedKey = std::pair<DocumentStore *, QThreadPool *>;
QHash<SharedKey, LibraryWatch *> &sharedWatches()
{
    static QHash<SharedKey, LibraryWatch *> watches;
    return watches;
}
} // namespace

LibraryWatch *LibraryWatch::take(const AppPaths &paths, DocumentStore *store, QThreadPool *pool)
{
    LibraryWatch *&shared = sharedWatches()[SharedKey{store, pool}];
    if (!shared) shared = new LibraryWatch(paths, store, pool);
    ++shared->m_users;
    return shared;
}

void LibraryWatch::give(LibraryWatch *shared)
{
    if (!shared || --shared->m_users > 0) return;
    sharedWatches().removeIf([shared](const auto &entry) { return entry.value() == shared; });
    delete shared;
}

void LibraryWatch::setBeforeWatching(std::function<void(const QString &path)> hook)
{
    LibraryFolderWatcher::beforeWatching.set(std::move(hook));
}

LibraryWatch::LibraryWatch(const AppPaths &paths, DocumentStore *store, QThreadPool *pool)
    : m_paths(paths), m_pool(pool), m_thread(new QThread), m_watcher(new LibraryFolderWatcher)
{
    // One look at the drafts on disk for every library, as one starts, as a
    // document closes, and when a draft's first save makes its folder.
    connect(store, &DocumentStore::documentClosed, this, &LibraryWatch::scanDrafts);
    connect(store, &DocumentStore::documentSaved, this, [this](Document *document) {
        if (document->isDraft() && !m_drafts.contains(document->id())) scanDrafts();
    });
    scanDrafts();
    m_watcher->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_watcher, &QObject::deleteLater);
    // What the watch sees is taken here, on the libraries' thread.
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) {
        if (path == m_paths.drafts()) scanDrafts();
        else if (m_counts.contains(path)) emit folderChanged(path);
    });
    connect(m_watcher, &LibraryFolderWatcher::added, this, [this](const QString &path) {
        if (m_counts.contains(path)) emit folderWatched(path);
    });
    m_thread->start();
}

LibraryWatch::~LibraryWatch()
{
    // A watch that never answers, as on a file system that stopped, is left
    // to the end of the process rather than waited for.
    disconnect(m_watcher, nullptr, this, nullptr);
    m_thread->quit();
    if (m_thread->wait(1000)) delete m_thread;
}

void LibraryWatch::watch(const QString &folder)
{
    if (++m_counts[folder] > 1) return;
    QMetaObject::invokeMethod(m_watcher, [watcher = m_watcher, folder] { watcher->add(folder); });
}

void LibraryWatch::unwatch(const QString &folder)
{
    const auto count = m_counts.find(folder);
    if (count == m_counts.end() || --*count > 0) return;
    m_counts.erase(count);
    QMetaObject::invokeMethod(m_watcher, [watcher = m_watcher, folder] { watcher->removePath(folder); });
}

QStringList LibraryWatch::folders() const
{
    return m_counts.keys();
}

void LibraryWatch::scanDrafts()
{
    if (m_scanning) {
        m_scanAgain = true;
        return;
    }
    m_scanning = true;
    QtConcurrent::run(m_pool, [paths = m_paths] {
        bool there = false;
        QList<DiskDraft> drafts = scan(paths, &there);
        return std::pair(drafts, there);
    }).then(this, [this](const std::pair<QList<DiskDraft>, bool> &found) {
        m_scanning = false;
        m_drafts.clear();
        for (const DiskDraft &draft : found.first) m_drafts.insert(draft.id, draft);
        // The folder exists once a draft has been written; drafts that go
        // from it, named or removed, then go from the list.
        if (found.second && !m_draftsWatched) {
            m_draftsWatched = true;
            QMetaObject::invokeMethod(m_watcher, [watcher = m_watcher, folder = m_paths.drafts()] {
                watcher->add(folder);
            });
        }
        emit draftsScanned();
        if (std::exchange(m_scanAgain, false)) scanDrafts();
    });
}

QList<LibraryWatch::DiskDraft> LibraryWatch::scan(const AppPaths &paths, bool *folderThere)
{
    QList<DiskDraft> drafts;
    *folderThere = QFileInfo(paths.drafts()).isDir();
    const QStringList ids = QDir(paths.drafts()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &id : ids) {
        QFile file(DocumentStore::draftFile(paths, id));
        if (!file.open(QIODevice::ReadOnly)) continue;
        QString text = QString::fromUtf8(file.read(titleBytes));
        if (text.startsWith(QChar(0xfeff))) text.remove(0, 1);
        drafts.append({id, Document::draftTitle(text), QFileInfo(file).lastModified()});
    }
    return drafts;
}

std::unique_ptr<LibraryModel::Node> LibraryModel::makeNode(const Row &row)
{
    auto node = std::make_unique<Node>();
    node->kind = row.kind;
    node->name = row.name;
    node->path = row.path;
    node->documentId = row.documentId;
    node->missing = row.missing;
    node->order = row.order;
    return node;
}

LibraryModel::LibraryModel(const AppPaths &paths, DocumentStore *store, RecentFiles *recent, Settings *settings,
                           QThreadPool *pool, QObject *parent)
    : QAbstractItemModel(parent), m_paths(paths), m_store(store), m_recent(recent), m_settings(settings), m_pool(pool),
      m_root(std::make_unique<Node>()), m_shared(LibraryWatch::take(paths, store, pool))
{
    // The sections are there before any view, so they need no announcing.
    for (const QString &name : {tr("Drafts"), tr("Recent")}) {
        auto section = makeNode({Kind::Section, name});
        section->parent = m_root.get();
        m_root->children.push_back(std::move(section));
    }
    m_draftsSection = m_root->children.at(0).get();
    m_recentSection = m_root->children.at(1).get();
    fill(m_root.get());

    m_settle.setSingleShot(true);
    m_settle.setInterval(SettleMs);
    connect(&m_settle, &QTimer::timeout, this, &LibraryModel::listChangedFolders);
    m_searchingFor.setSingleShot(true);
    m_searchingFor.setInterval(SearchingMs);
    // Not early, as a coarse timer may be: the time is the filter's to the end.
    m_searchingFor.setTimerType(Qt::PreciseTimer);
    connect(&m_searchingFor, &QTimer::timeout, this, &LibraryModel::sayWhetherSearching);
    // A folder this library watches changed, or is watched now, which
    // catches what changed before then.
    const auto changed = [this](const QString &path) {
        if (!m_watching.contains(path)) return;
        m_changed.insert(path);
        m_settle.start();
    };
    connect(m_shared, &LibraryWatch::folderChanged, this, changed);
    connect(m_shared, &LibraryWatch::folderWatched, this, changed);
    connect(m_shared, &LibraryWatch::draftsScanned, this, &LibraryModel::rebuildDrafts);

    connect(m_store, &DocumentStore::documentOpened, this, [this](Document *document) {
        follow(document);
        rebuildDrafts();
    });
    // What the drafts on disk hold now, the shared watch looks at again.
    connect(m_store, &DocumentStore::documentClosed, this, &LibraryModel::rebuildDrafts);
    for (Document *document : m_store->documents()) follow(document);
    connect(m_recent, &RecentFiles::changed, this, [this] {
        rebuildRecent();
        checkRecent();
    });
    connect(m_settings, &Settings::changed, this, &LibraryModel::syncFolders);

    rebuildDrafts();
    rebuildRecent();
    syncFolders();
    checkRecent();
}

LibraryModel::~LibraryModel()
{
    for (const QString &folder : std::as_const(m_watching)) m_shared->unwatch(folder);
    LibraryWatch::give(m_shared);
}

QModelIndex LibraryModel::index(int row, int column, const QModelIndex &parent) const
{
    const Node *node = nodeAt(parent);
    if (!node || column != 0 || row < 0 || row >= node->shown.size()) return {};
    return createIndex(row, 0, node->shown.at(row));
}

QModelIndex LibraryModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) return {};
    const Node *node = nodeAt(child);
    return node ? indexOf(node->parent) : QModelIndex();
}

int LibraryModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid() && parent.column() != 0) return 0;
    const Node *node = nodeAt(parent);
    return node ? int(node->shown.size()) : 0;
}

int LibraryModel::columnCount(const QModelIndex &) const { return 1; }

bool LibraryModel::hasChildren(const QModelIndex &parent) const
{
    const Node *node = nodeAt(parent);
    if (!node) return false;
    if (node->kind == Kind::Folder) return node->listing != Listing::Done || !node->children.empty();
    return !node->shown.isEmpty();
}

QVariant LibraryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid()) return {};
    const Node *node = nodeAt(index);
    if (!node) return {};
    switch (role) {
    case Qt::DisplayRole:
    case NameRole:
        return node->name;
    case PathRole:
        return node->path;
    case KindRole:
        return kindName(node->kind);
    case MissingRole:
        return node->missing;
    case DocumentIdRole:
        return node->documentId;
    case LoadingRole:
        return node->request != 0;
    case SectionRole:
        return node == m_draftsSection ? QStringLiteral("drafts") : node == m_recentSection ? QStringLiteral("recent") : QString();
    default:
        return {};
    }
}

QHash<int, QByteArray> LibraryModel::roleNames() const
{
    return {{NameRole, "name"}, {PathRole, "path"}, {KindRole, "kind"}, {MissingRole, "missing"},
            {DocumentIdRole, "documentId"}, {LoadingRole, "loading"}, {SectionRole, "section"}};
}

bool LibraryModel::canFetchMore(const QModelIndex &parent) const
{
    const Node *node = nodeAt(parent);
    return node && node->kind == Kind::Folder && node->listing == Listing::None;
}

void LibraryModel::fetchMore(const QModelIndex &parent)
{
    Node *node = nodeAt(parent);
    if (node && node->kind == Kind::Folder && node->listing == Listing::None) list(node);
}

QString LibraryModel::filter() const { return m_filter; }
int LibraryModel::matchCount() const { return m_matchCount; }
bool LibraryModel::searching() const
{
    // Every listing out for a search lands in it, an earlier filter's too,
    // so each counts until it lands, or the filter's time is up.
    return !m_filter.isEmpty() && m_searchingFor.isActive() && (!m_searchQueue.isEmpty() || !m_searchJobs.isEmpty());
}
int LibraryModel::searchListings() const { return int(m_searchPaths.size()); }
int LibraryModel::searchQueued() const { return int(m_searchQueue.size()); }

void LibraryModel::setFilter(const QString &filter)
{
    if (filter == m_filter) return;
    const bool wasEmpty = m_filter.isEmpty();
    // A search that starts afresh lists afresh what is not watched.
    if (wasEmpty && !filter.isEmpty()) unlistUnwatched(m_root.get());
    m_filter = filter;
    if (filter.isEmpty()) m_searchingFor.stop();
    else m_searchingFor.start();
    applyFilter();
    startSearch();
    // Emptied, the field leaves open what the view keeps open, which it
    // settles once it has seen the change.
    if (filter.isEmpty())
        QTimer::singleShot(0, this, [this] {
            if (m_filter.isEmpty()) unlistUnwatched(m_root.get());
        });
    emit filterChanged();
}

void LibraryModel::addFolder(const QString &path)
{
    if (!PathText::isFullPath(path)) return;
    // The kernel's name for it, when it is there, so that one folder is listed once.
    const QString canonical = QFileInfo(path).canonicalFilePath();
    m_settings->setLibraryFolders(m_settings->libraryFolders() << (canonical.isEmpty() ? path : canonical));
}

void LibraryModel::removeFolder(const QString &path)
{
    QStringList folders = m_settings->libraryFolders();
    folders.removeAll(path);
    m_settings->setLibraryFolders(folders);
}

void LibraryModel::setExpanded(const QModelIndex &index, bool expanded)
{
    // An index of another model, as QML could pass, names nothing here.
    Node *node = index.isValid() ? nodeAt(index) : nullptr;
    if (!node) return;
    if (!expanded) {
        forget(node);
        return;
    }
    if (node == m_recentSection) checkRecent();
    if (node->kind != Kind::Folder) {
        node->expanded = true;
        return;
    }
    watch(node);
    // What was listed before may be out of date, since it was not watched.
    if (node->listing != Listing::Pending) list(node);
}

QModelIndexList LibraryModel::matches() const
{
    QModelIndexList found;
    if (m_filter.isEmpty()) return found;
    const auto visit = [&](const auto &self, const Node *node) -> void {
        for (const Node *child : node->shown) {
            if (child->kind != Kind::Section && child->name.contains(m_filter, Qt::CaseInsensitive))
                found.append(indexOf(child));
            self(self, child);
        }
    };
    visit(visit, m_root.get());
    return found;
}

QModelIndexList LibraryModel::nearestMatches(int limit) const
{
    QModelIndexList found;
    if (m_filter.isEmpty() || limit <= 0) return found;
    // Level by level, so that the nearest are found first.
    QList<const Node *> level{m_root.get()};
    while (!level.isEmpty() && found.size() < limit) {
        QList<const Node *> next;
        for (const Node *node : std::as_const(level)) {
            for (const Node *child : node->shown) {
                if (child->kind != Kind::Section && child->name.contains(m_filter, Qt::CaseInsensitive)) {
                    found.append(indexOf(child));
                    if (found.size() >= limit) return found;
                }
                next.append(child);
            }
        }
        level = std::move(next);
    }
    return found;
}

QStringList LibraryModel::watchedFolders() const { return m_watching; }

int LibraryModel::jobsInFlight() const { return m_jobs + (m_shared->scanning() ? 1 : 0); }

// What runs off the UI thread works on plain values only.

LibraryModel::FolderListing LibraryModel::listFolder(const QString &path)
{
    FolderListing listing;
    const QByteArray encoded = QFile::encodeName(path);
    struct stat state {};
    if (::stat(encoded.constData(), &state) != 0) {
        const int error = errno;
        listing.missing = error == ENOENT || error == ENOTDIR;
        listing.error = listing.missing ? tr("Folder not found")
                                        : tr("Cannot be read: %1").arg(QString::fromLocal8Bit(std::strerror(error)));
        return listing;
    }
    if (!S_ISDIR(state.st_mode)) {
        listing.missing = true;
        listing.error = tr("Not a folder");
        return listing;
    }
    listing.identity = QString::number(state.st_dev) + u':' + QString::number(state.st_ino);
    DIR *folder = ::opendir(encoded.constData());
    if (!folder) {
        listing.error = tr("Cannot be read: %1").arg(QString::fromLocal8Bit(std::strerror(errno)));
        return listing;
    }
    errno = 0;
    while (const dirent *entry = ::readdir(folder)) {
        const QByteArray raw(entry->d_name);
        const QString name = QFile::decodeName(raw);
        // Hidden entries, and a name that would not lead back to its file, are left out.
        if (name.startsWith(u'.') || QFile::encodeName(name) != raw) continue;
        bool isFolder = entry->d_type == DT_DIR;
        if (entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN) {
            struct stat target {};
            if (::stat((encoded + '/' + raw).constData(), &target) != 0) continue;
            if (!S_ISDIR(target.st_mode) && !S_ISREG(target.st_mode)) continue;
            isFolder = S_ISDIR(target.st_mode);
        } else if (entry->d_type != DT_DIR && entry->d_type != DT_REG) {
            continue;
        }
        if (isFolder || isTextDocument(name)) listing.entries.append({name, isFolder});
        errno = 0;
    }
    const int error = errno;
    ::closedir(folder);
    if (error != 0) {
        listing.entries.clear();
        listing.error = tr("Cannot be read: %1").arg(QString::fromLocal8Bit(std::strerror(error)));
        return listing;
    }
    std::sort(listing.entries.begin(), listing.entries.end(), [](const Entry &a, const Entry &b) {
        if (a.folder != b.folder) return a.folder;
        const int order = naturalCompare(a.name, b.name);
        return order != 0 ? order < 0 : a.name < b.name;
    });
    listing.ok = true;
    return listing;
}

LibraryModel::FolderListing LibraryModel::examine(const QString &path, bool probeOnly)
{
    if (probeOnly && QFileInfo(path).isDir()) {
        FolderListing there;
        there.probed = true;
        return there;
    }
    return listFolder(path);
}

QString LibraryModel::kindName(Kind kind)
{
    switch (kind) {
    case Kind::Section:
        return QStringLiteral("section");
    case Kind::Folder:
        return QStringLiteral("folder");
    case Kind::Document:
        return QStringLiteral("document");
    case Kind::Draft:
        return QStringLiteral("draft");
    case Kind::Error:
        return QStringLiteral("error");
    }
    return {};
}

LibraryModel::Key LibraryModel::keyOf(Kind kind, const QString &name, const QString &path, const QString &documentId)
{
    switch (kind) {
    case Kind::Section:
    case Kind::Error:
        return {int(kind), name};
    case Kind::Draft:
        return {int(kind), documentId};
    case Kind::Folder:
    case Kind::Document:
        return {int(kind), path};
    }
    return {};
}

LibraryModel::Key LibraryModel::keyOf(const Node &node)
{
    return keyOf(node.kind, node.name, node.path, node.documentId);
}

LibraryModel::Key LibraryModel::keyOf(const Row &row)
{
    return keyOf(row.kind, row.name, row.path, row.documentId);
}

// A folder's listing names what its rows already show, in the same order.
bool LibraryModel::sameEntries(const Node *folder, const QList<Entry> &entries)
{
    if (folder->children.size() != size_t(entries.size())) return false;
    for (qsizetype i = 0; i < entries.size(); ++i) {
        const Node *child = folder->children.at(i).get();
        const Kind kind = entries.at(i).folder ? Kind::Folder : Kind::Document;
        if (child->kind != kind || child->name != entries.at(i).name) return false;
    }
    return true;
}

// The node an index of this model names, the root for no index, and none
// for an index of another model.
LibraryModel::Node *LibraryModel::nodeAt(const QModelIndex &index) const
{
    if (!index.isValid()) return m_root.get();
    if (index.model() != this) return nullptr;
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex LibraryModel::indexOf(const Node *node) const
{
    if (!node || node == m_root.get()) return {};
    return createIndex(node->row, 0, const_cast<Node *>(node));
}

bool LibraryModel::inView(const Node *node) const
{
    for (; node != m_root.get(); node = node->parent)
        if (!node->parent || node->parent->shown.value(node->row) != node) return false;
    return true;
}

void LibraryModel::changedData(Node *node)
{
    if (!inView(node)) return;
    const QModelIndex at = indexOf(node);
    emit dataChanged(at, at);
}

void LibraryModel::replaceChildren(Node *parent, const QList<Row> &rows)
{
    std::vector<std::unique_ptr<Node>> &children = parent->children;
    QHash<Key, qsizetype> existing;
    existing.reserve(qsizetype(children.size()));
    for (qsizetype i = 0; i < qsizetype(children.size()); ++i) existing.insert(keyOf(*children[i]), i);
    // The present child each new row keeps, if any, so that a row that stays
    // keeps its listing and expansion; and whether they keep their order.
    QList<qsizetype> from(rows.size(), -1);
    std::vector<bool> keeping(children.size(), false);
    bool reordered = false;
    qsizetype last = -1;
    for (qsizetype j = 0; j < rows.size(); ++j) {
        const auto found = existing.constFind(keyOf(rows.at(j)));
        if (found == existing.cend() || keeping[*found]) continue;
        keeping[*found] = true;
        from[j] = *found;
        reordered = reordered || *found < last;
        last = *found;
    }
    // Rows that go are taken out of view first. A new order takes every row
    // out and puts them back, which only sections do, whose rows hold nothing.
    for (qsizetype i = 0; i < qsizetype(children.size()); ++i)
        if (reordered || !keeping[i]) children[i]->wanted = false;
    if (inView(parent)) {
        sync(parent);
    } else {
        parent->shown.removeIf([](const Node *node) { return !node->wanted; });
        renumber(parent, 0);
    }

    std::vector<std::unique_ptr<Node>> next;
    next.reserve(rows.size());
    for (qsizetype j = 0; j < rows.size(); ++j) {
        const Row &row = rows.at(j);
        if (from[j] < 0) {
            next.push_back(makeNode(row));
            next.back()->parent = parent;
            continue;
        }
        Node *same = children[from[j]].get();
        const bool changed = same->name != row.name || same->path != row.path || same->missing != row.missing;
        same->name = row.name;
        same->path = row.path;
        same->missing = row.missing;
        same->order = row.order;
        next.push_back(std::move(children[from[j]]));
        if (changed) changedData(same);
    }
    // What is left goes, with whatever it listed, watched or waited for.
    for (auto &child : children)
        if (child) forget(child.get());
    children = std::move(next);
    if (!m_filter.isEmpty()) {
        // What matches may have come or gone, and with it what holds it.
        applyFilter();
        return;
    }
    // Without a filter every row is shown, and only these rows changed.
    for (const auto &child : children) child->wanted = true;
    if (inView(parent)) sync(parent);
    else fill(parent);
}

void LibraryModel::applyFilter()
{
    int matches = 0;
    computeWanted(m_root.get(), false, &matches);
    sync(m_root.get());
    if (std::exchange(m_matchCount, matches) != matches) emit matchCountChanged();
}

// Marks what the filter keeps beneath a node, and returns whether anything
// beneath it matches.
bool LibraryModel::computeWanted(Node *node, bool ancestorMatches, int *matches)
{
    bool matched = false;
    for (const auto &owned : node->children) {
        Node *child = owned.get();
        const bool self = !m_filter.isEmpty() && child->kind != Kind::Section
                          && child->name.contains(m_filter, Qt::CaseInsensitive);
        *matches += self ? 1 : 0;
        const bool beneath = computeWanted(child, ancestorMatches || self, matches);
        child->wanted = m_filter.isEmpty() || child->kind == Kind::Section || self || ancestorMatches || beneath;
        matched = matched || self || beneath;
    }
    return matched;
}

void LibraryModel::sync(Node *node)
{
    const QModelIndex parentIndex = indexOf(node);
    QList<Node *> &shown = node->shown;
    // shown[at] is the next row to compare; a run of rows to remove starts
    // there, or a run of nodes waits to be put there. Runs are announced
    // whole, so that a folder of thousands of entries is one insertion.
    int at = 0;
    int removing = 0;
    QList<Node *> inserting;
    const auto flushInsert = [&] {
        if (inserting.isEmpty()) return;
        for (Node *child : std::as_const(inserting)) fill(child);
        beginInsertRows(parentIndex, at, at + int(inserting.size()) - 1);
        for (qsizetype i = 0; i < inserting.size(); ++i) shown.insert(at + i, inserting.at(i));
        renumber(node, at);
        endInsertRows();
        at += int(inserting.size());
        inserting.clear();
    };
    const auto flushRemove = [&] {
        if (removing == 0) return;
        beginRemoveRows(parentIndex, at, at + removing - 1);
        const QList<Node *> gone = shown.mid(at, removing);
        shown.remove(at, removing);
        renumber(node, at);
        endRemoveRows();
        for (Node *child : gone) forget(child);
        removing = 0;
    };
    for (const auto &owned : node->children) {
        Node *child = owned.get();
        const bool isShown = at + removing < shown.size() && shown.at(at + removing) == child;
        if (child->wanted && isShown) {
            flushRemove();
            flushInsert();
            sync(child);
            ++at;
        } else if (child->wanted) {
            flushRemove();
            inserting.append(child);
        } else if (isShown) {
            flushInsert();
            ++removing;
        }
    }
    flushRemove();
    flushInsert();
}

void LibraryModel::fill(Node *node)
{
    node->shown.clear();
    for (const auto &child : node->children) {
        if (!child->wanted) continue;
        node->shown.append(child.get());
        fill(child.get());
    }
    renumber(node, 0);
}

void LibraryModel::renumber(Node *node, int from)
{
    for (qsizetype i = from; i < node->shown.size(); ++i) node->shown.at(i)->row = int(i);
}

void LibraryModel::forget(Node *node)
{
    if (node->kind == Kind::Folder) unwatch(node);
    node->expanded = false;
    if (node->request) {
        m_probes.remove(node->request);
        m_staged.remove(node->request);
        m_requests.remove(std::exchange(node->request, 0));
        if (node->listing == Listing::Pending) node->listing = Listing::None;
        changedData(node);
    }
    // A folder found empty may not be any more, now that it is not watched;
    // it is looked in again when next expanded.
    if (node->kind == Kind::Folder && node->listing == Listing::Done && node->children.empty()) {
        node->listing = Listing::None;
        changedData(node);
    }
    for (const auto &child : node->children) forget(child.get());
}

void LibraryModel::list(Node *folder)
{
    request(folder, false);
}

// Only whether a folder is there, for a folder at the top that is not listed
// yet, so that one that has gone says so before it is expanded.
void LibraryModel::probe(Node *folder)
{
    request(folder, true);
}

void LibraryModel::request(Node *folder, bool probeOnly)
{
    if (!probeOnly && folder->listing == Listing::None) folder->listing = Listing::Pending;
    if (folder->request) m_probes.remove(folder->request);
    const quint64 token = ++m_lastRequest;
    startRequest(folder, token);
    if (probeOnly) m_probes.insert(token);
    startJob(folder->path);
}

// One job per folder at a time, for the newest request waiting for it, so
// that a folder that never answers holds one worker and nothing piles up
// behind it. A request made while the job runs is taken up when it ends.
void LibraryModel::startJob(const QString &path)
{
    if (m_running.contains(path)) return;
    quint64 token = 0;
    for (auto waiting = m_requests.cbegin(); waiting != m_requests.cend(); ++waiting)
        if (waiting.value()->path == path) token = std::max(token, waiting.key());
    if (token == 0) return;
    m_running.insert(path);
    ++m_jobs;
    QtConcurrent::run(m_pool, &LibraryModel::examine, path, m_probes.contains(token))
        .then(this, [this, path, token](const FolderListing &listing) {
            --m_jobs;
            m_running.remove(path);
            applyListing(token, listing);
            startJob(path);
            // A folder the filter waited for while this job ran can be listed now.
            pumpSearch();
        });
}

void LibraryModel::startRequest(Node *folder, quint64 request)
{
    if (folder->request) {
        m_requests.remove(folder->request);
        m_staged.remove(folder->request);
    }
    const bool wasLoading = folder->request != 0;
    folder->request = request;
    m_requests.insert(request, folder);
    // Announced once the view's expansion, which may have asked for this
    // listing, is over.
    if (!wasLoading && inView(folder)) {
        const QPersistentModelIndex at = indexOf(folder);
        QTimer::singleShot(0, this, [this, at] {
            if (at.isValid()) emit dataChanged(at, at);
        });
    }
}

void LibraryModel::applyListing(quint64 request, const FolderListing &listing)
{
    // A folder collapsed or removed since asked for nothing more.
    Node *folder = m_requests.take(request);
    m_probes.remove(request);
    if (!folder) return;
    folder->request = 0;
    if (listing.probed) {
        changedData(folder);
        return;
    }
    folder->listing = Listing::Done;
    folder->missing = listing.missing;
    if (listing.ok) folder->identity = listing.identity;
    const bool wasFailed = std::exchange(folder->failed, !listing.ok);
    // Most listings find what the last one did, as when a document in the
    // folder is saved, and change nothing.
    if (listing.ok && !wasFailed && sameEntries(folder, listing.entries)) {
        listingApplied(folder, true, listing.identity);
        return;
    }
    Staged staged{folder, {}, 0, listing.ok, listing.identity};
    if (!listing.ok) {
        staged.rows.append({Kind::Error, listing.error});
    } else {
        staged.rows.reserve(listing.entries.size());
        for (const Entry &entry : listing.entries)
            staged.rows.append({entry.folder ? Kind::Folder : Kind::Document, entry.name, folder->path + u'/' + entry.name});
    }
    // Still loading until the last chunk is in, and dropped, as a listing
    // is, should the folder be collapsed, removed or listed again first.
    folder->request = request;
    m_staged.insert(request, staged);
    applyChunk(request);
}

void LibraryModel::applyChunk(quint64 request)
{
    const auto found = m_staged.find(request);
    if (found == m_staged.end()) return;
    Staged &staged = *found;
    Node *folder = staged.folder;
    staged.ready = std::min(staged.rows.size(), staged.ready + ListingChunk);
    // The rows of the listing up to here, and those it keeps further on that
    // the folder shows already, where they are, so that no row is taken out
    // and put back. Both are in the listing's order.
    QList<Row> rows = staged.rows.mid(0, staged.ready);
    if (staged.ready < staged.rows.size()) {
        QSet<Key> later;
        for (qsizetype i = staged.ready; i < staged.rows.size(); ++i) later.insert(keyOf(staged.rows.at(i)));
        for (const auto &child : folder->children) {
            if (!later.contains(keyOf(*child))) continue;
            rows.append({child->kind, child->name, child->path, child->documentId, child->missing, child->order});
        }
    }
    const bool whole = staged.ready == staged.rows.size();
    const bool ok = staged.ok;
    const QString identity = staged.identity;
    if (whole) m_staged.erase(found);
    replaceChildren(folder, rows);
    if (!whole) {
        QTimer::singleShot(0, this, [this, request] { applyChunk(request); });
        return;
    }
    folder->request = 0;
    listingApplied(folder, ok, identity);
}

void LibraryModel::listingApplied(Node *folder, bool ok, const QString &identity)
{
    changedData(folder);
    // What it holds that is not listed yet, the filter lists too.
    if (!m_filter.isEmpty() && ok) {
        m_searchSeen.insert(identity);
        searchBeneath(folder, depthOf(folder));
        pumpSearch();
    }
    // An expanded folder is watched once a listing has read it, so that
    // watching never waits on the file system here. What changed between
    // that listing and the watch is caught by one more, once it is in place.
    if (folder->expanded && ok) startWatching(folder->path);
}

void LibraryModel::watch(Node *folder)
{
    if (folder->expanded) return;
    folder->expanded = true;
    // A folder the last listing read is watched at once; any other once a
    // listing has read it, since a folder that cannot be read cannot be
    // watched, and asking would wait on the file system here.
    const bool read = folder->listing == Listing::Done && !folder->failed;
    if (++m_watchCount[folder->path] == 1 && read) startWatching(folder->path);
}

void LibraryModel::unwatch(Node *folder)
{
    if (!folder->expanded) return;
    folder->expanded = false;
    const auto count = m_watchCount.find(folder->path);
    if (count == m_watchCount.end() || --*count > 0) return;
    m_watchCount.erase(count);
    if (m_watching.removeOne(folder->path)) m_shared->unwatch(folder->path);
}

void LibraryModel::startWatching(const QString &path)
{
    if (m_watching.contains(path)) return;
    m_watching.append(path);
    m_shared->watch(path);
}

void LibraryModel::listChangedFolders()
{
    const QSet<QString> changed = std::exchange(m_changed, {});
    for (const QString &path : changed) {
        QList<Node *> folders;
        findExpanded(m_root.get(), path, &folders);
        for (Node *folder : std::as_const(folders))
            if (folder->listing != Listing::Pending) list(folder);
    }
}

void LibraryModel::findExpanded(Node *node, const QString &path, QList<Node *> *found) const
{
    for (const auto &child : node->children) {
        if (child->kind != Kind::Folder && child->kind != Kind::Section) continue;
        if (child->kind == Kind::Folder && !child->expanded) continue;
        if (child->kind == Kind::Folder && child->path == path) found->append(child.get());
        findExpanded(child.get(), path, found);
    }
}

void LibraryModel::startSearch()
{
    // What an earlier filter left to do goes; its jobs that still run are
    // dropped as they end.
    ++m_searchGeneration;
    m_searchRunning = 0;
    m_searchQueue.clear();
    m_searchPaths.clear();
    m_searchSeen.clear();
    if (!m_filter.isEmpty()) {
        collectIdentities(m_root.get());
        for (const auto &top : m_root->children)
            if (top->kind == Kind::Folder) searchBeneath(top.get(), 0);
    }
    pumpSearch();
}

// Queues, nearest first, the folders not listed yet at or beneath a node
// that lies `depth` folders below its library folder: the node itself, or,
// once it is listed, what it holds.
void LibraryModel::searchBeneath(Node *node, int depth)
{
    QList<std::pair<Node *, int>> level{{node, depth}};
    while (!level.isEmpty()) {
        QList<std::pair<Node *, int>> next;
        for (const auto &[at, atDepth] : std::as_const(level)) {
            if (at->listing != Listing::Done) {
                queueSearch(at, atDepth);
                continue;
            }
            for (const auto &child : at->children)
                if (child->kind == Kind::Folder) next.append({child.get(), atDepth + 1});
        }
        level = std::move(next);
    }
}

void LibraryModel::queueSearch(Node *folder, int depth)
{
    if (depth > SearchDepth || folder->kind != Kind::Folder || folder->listing == Listing::Done) return;
    // A listing asked for by a view is on its way and does the same.
    if (folder->request && !m_probes.contains(folder->request)) return;
    if (m_searchPaths.contains(folder->path) || m_searchPaths.size() >= SearchFolders) return;
    m_searchPaths.insert(folder->path);
    m_searchQueue.append({folder->path, depth});
}

void LibraryModel::pumpSearch()
{
    for (qsizetype i = 0; i < m_searchQueue.size() && m_searchRunning < SearchJobs;) {
        const Search search = m_searchQueue.at(i);
        // One job per folder. A folder an older filter's listing is still
        // out for is passed over, since that listing may never answer; one
        // a view's listing, or a look at whether it is there, is out for is
        // waited for.
        if (m_searchJobs.contains(search.path)) {
            m_searchQueue.removeAt(i);
            continue;
        }
        if (m_running.contains(search.path)) {
            ++i;
            continue;
        }
        m_searchQueue.removeAt(i);
        m_running.insert(search.path);
        m_searchJobs.insert(search.path);
        ++m_jobs;
        ++m_searchRunning;
        const quint64 generation = m_searchGeneration;
        QtConcurrent::run(m_pool, &LibraryModel::listFolder, search.path)
            .then(this, [this, search, generation](const FolderListing &listing) {
                --m_jobs;
                m_running.remove(search.path);
                m_searchJobs.remove(search.path);
                // Only the filter's own jobs count against its jobs. What
                // an earlier filter lists is used all the same while the
                // field has text, even one from before it was emptied: the
                // folder is listed as it is now.
                if (generation == m_searchGeneration) --m_searchRunning;
                if (!m_filter.isEmpty()) applySearchListing(search, listing);
                startJob(search.path);
                pumpSearch();
            });
    }
    sayWhetherSearching();
}

void LibraryModel::applySearchListing(const Search &search, const FolderListing &listing)
{
    Node *folder = folderAt(search.path);
    // Gone, listed meanwhile, or about to be by a view's request.
    if (!folder || folder->listing == Listing::Done || (folder->request && !m_probes.contains(folder->request))) return;
    if (listing.ok) {
        // A link back into a folder listed already would list it again, and
        // again beneath that.
        if (m_searchSeen.contains(listing.identity)) return;
        m_searchSeen.insert(listing.identity);
        folder->identity = listing.identity;
    }
    folder->listing = Listing::Done;
    folder->missing = listing.missing;
    folder->failed = !listing.ok;
    QList<Row> rows;
    if (!listing.ok) {
        rows.append({Kind::Error, listing.error});
    } else {
        rows.reserve(listing.entries.size());
        for (const Entry &entry : listing.entries)
            rows.append({entry.folder ? Kind::Folder : Kind::Document, entry.name, folder->path + u'/' + entry.name});
    }
    replaceChildren(folder, rows);
    changedData(folder);
    if (!listing.ok) return;
    int matched = 0;
    for (const auto &child : folder->children) {
        if (child->kind == Kind::Folder) queueSearch(child.get(), search.depth + 1);
        if (child->name.contains(m_filter, Qt::CaseInsensitive)) ++matched;
    }
    // Only what this listing added, for a view to open to.
    if (matched > 0 && inView(folder)) emit matchesListed(indexOf(folder), matched);
}

void LibraryModel::unlistUnwatched(Node *node)
{
    for (const auto &owned : node->children) {
        Node *folder = owned.get();
        if (folder->kind != Kind::Folder || folder->listing != Listing::Done) continue;
        // An open folder is watched, and so is true to the disk; what lies
        // beneath it may not be.
        if (folder->expanded) {
            unlistUnwatched(folder);
            continue;
        }
        if (folder->request && !m_probes.contains(folder->request)) continue;
        replaceChildren(folder, {});
        folder->listing = Listing::None;
        folder->failed = false;
        folder->identity.clear();
        changedData(folder);
    }
}

void LibraryModel::sayWhetherSearching()
{
    const bool now = searching();
    if (now == m_searchingSaid) return;
    m_searchingSaid = now;
    emit searchingChanged();
}

LibraryModel::Node *LibraryModel::folderAt(const QString &path) const
{
    for (const auto &top : m_root->children) {
        if (top->kind != Kind::Folder) continue;
        if (top->path == path) return top.get();
        if (!path.startsWith(top->path + u'/')) continue;
        Node *node = top.get();
        for (const QString &part : path.mid(top->path.size() + 1).split(u'/', Qt::SkipEmptyParts)) {
            Node *next = nullptr;
            for (const auto &child : node->children)
                if (child->kind == Kind::Folder && child->name == part) next = child.get();
            node = next;
            if (!node) break;
        }
        if (node && node->path == path) return node;
    }
    return nullptr;
}

int LibraryModel::depthOf(const Node *node) const
{
    int depth = 0;
    for (const Node *at = node; at->parent && at->parent != m_root.get(); at = at->parent) ++depth;
    return depth;
}

void LibraryModel::collectIdentities(const Node *node)
{
    for (const auto &child : node->children) {
        if (child->kind == Kind::Folder && child->listing == Listing::Done && !child->identity.isEmpty())
            m_searchSeen.insert(child->identity);
        collectIdentities(child.get());
    }
}

void LibraryModel::syncFolders()
{
    const QStringList folders = m_settings->libraryFolders();
    if (folders == m_folders && m_root->children.size() == size_t(2 + folders.size())) return;
    const QStringList before = std::exchange(m_folders, folders);
    QList<Row> rows{{Kind::Section, m_draftsSection->name}, {Kind::Section, m_recentSection->name}};
    for (const QString &folder : folders) {
        const QString name = PathText::fileName(folder);
        rows.append({Kind::Folder, name.isEmpty() ? folder : name, folder});
    }
    replaceChildren(m_root.get(), rows);
    for (const auto &child : m_root->children)
        if (child->kind == Kind::Folder && child->listing == Listing::None && !child->request) probe(child.get());
    if (m_filter.isEmpty()) return;
    // A search follows the library: what waited beneath a folder taken out
    // goes, and a folder added is searched too.
    for (const QString &gone : before) {
        if (folders.contains(gone)) continue;
        m_searchQueue.removeIf([&gone](const Search &search) {
            return search.path == gone || search.path.startsWith(gone + u'/');
        });
    }
    for (const auto &child : m_root->children)
        if (child->kind == Kind::Folder && !before.contains(child->path)) searchBeneath(child.get(), 0);
    pumpSearch();
}

void LibraryModel::rebuildDrafts()
{
    QList<Row> rows;
    QSet<QString> open;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (Document *document : m_store->documents()) {
        if (!document->isDraft() || document->isScratch()) continue;
        open.insert(document->id());
        Row row{Kind::Draft, document->title(), {}, document->id()};
        const auto disk = m_shared->drafts().constFind(document->id());
        if (disk != m_shared->drafts().cend()) {
            row.order = disk->modified;
        } else {
            if (!m_firstSeen.contains(document->id())) m_firstSeen.insert(document->id(), now);
            row.order = m_firstSeen.value(document->id());
        }
        rows.append(row);
    }
    // A draft no longer open needs its first sighting kept no longer.
    m_firstSeen.removeIf([&open](const auto &seen) { return !open.contains(seen.key()); });
    for (const DiskDraft &draft : m_shared->drafts()) {
        if (open.contains(draft.id) || !m_store->isUnnamedDraft(draft.id)) continue;
        rows.append({Kind::Draft, draft.title, {}, draft.id, false, draft.modified});
    }
    // The newest first, and otherwise always in the same order.
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        return a.order != b.order ? a.order > b.order : a.documentId < b.documentId;
    });
    replaceChildren(m_draftsSection, rows);
}

void LibraryModel::updateDraft(Document *draft)
{
    for (const auto &child : m_draftsSection->children) {
        if (child->documentId != draft->id()) continue;
        const QString title = draft->title();
        if (child->name == title) return;
        child->name = title;
        changedData(child.get());
        if (!m_filter.isEmpty()) applyFilter();
        return;
    }
}

void LibraryModel::rebuildRecent()
{
    QList<Row> rows;
    for (const QString &path : m_recent->paths())
        rows.append({Kind::Document, PathText::fileName(path), path, {}, m_missing.value(path, false)});
    replaceChildren(m_recentSection, rows);
}

void LibraryModel::checkRecent()
{
    // One check at a time; a change meanwhile asks for one more after it.
    if (m_checking) {
        m_checkAgain = true;
        return;
    }
    m_checking = true;
    ++m_jobs;
    QtConcurrent::run(m_pool, [paths = m_recent->paths()] {
        QHash<QString, bool> missing;
        for (const QString &path : paths) missing.insert(path, RecentFiles::isMissing(path));
        return missing;
    }).then(this, [this](const QHash<QString, bool> &missing) {
        --m_jobs;
        m_checking = false;
        m_missing = missing;
        for (const auto &child : m_recentSection->children) {
            const bool now = m_missing.value(child->path, false);
            if (child->missing == now) continue;
            child->missing = now;
            changedData(child.get());
        }
        if (std::exchange(m_checkAgain, false)) checkRecent();
    });
}

void LibraryModel::follow(Document *document)
{
    if (!document->isDraft() || document->isScratch()) return;
    connect(document, &Document::changed, this, [this, document] {
        if (document->isDraft()) {
            updateDraft(document);
            return;
        }
        // A named draft is a draft no more.
        disconnect(document, &Document::changed, this, nullptr);
        rebuildDrafts();
    });
}
