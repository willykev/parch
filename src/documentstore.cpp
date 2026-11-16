#include "documentstore.h"

#include "appservice.h"
#include "document.h"
#include "pathtext.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QSemaphore>
#include <QTimer>
#include <QUuid>
#include <QtConcurrent>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace {
const QString draftName = QStringLiteral("draft.md");

QString errorText(int number)
{
    return QString::fromLocal8Bit(std::strerror(number));
}

QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// What a version holds, as recovery notes a save: its hash, or nothing for a file that is not there.
QByteArray contentOf(const DiskVersion &version)
{
    return version.exists ? version.sha256 : QByteArray();
}

// What a document waits on when its path leads to another file than its own.
QString leadsElsewhere(const QString &path, const QString &target)
{
    return DocumentStore::tr("“%1” now leads to “%2”").arg(PathText::fileName(path), target);
}

// For tests: called with each asset's path just before it is written.
std::function<void(const QString &path)> beforePlacing;

bool isRegularFile(const QString &path)
{
    struct stat state {};
    return ::stat(QFile::encodeName(path).constData(), &state) == 0 && S_ISREG(state.st_mode);
}

// Whether the file at the path and the one at the name in the folder hold
// the same bytes; the name is never followed.
bool sameBytes(const QString &first, int folder, const QByteArray &second)
{
    QFile a(first), b;
    const int descriptor = ::openat(folder, second.constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return false;
    if (!b.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return false;
    }
    if (a.size() != b.size() || !a.open(QIODevice::ReadOnly)) return false;
    for (;;) {
        const QByteArray left = a.read(1 << 16), right = b.read(1 << 16);
        if (left != right) return false;
        if (left.isEmpty()) return a.error() == QFileDevice::NoError && b.error() == QFileDevice::NoError;
    }
}

// A folder held open, so that what is made in it, and undone in it, is made
// there and never through a link put on its path since.
struct Folder {
    int descriptor = -1;
    Folder() = default;
    Folder(const Folder &) = delete;
    Folder &operator=(const Folder &) = delete;
    ~Folder()
    {
        if (descriptor >= 0) ::close(descriptor);
    }
};
using FolderRef = std::shared_ptr<Folder>;

// The files and folders one naming created, each by the folder it was made
// in and its name there. A failed naming removes exactly these and never
// anything that was there before it.
struct Placement {
    struct Made {
        FolderRef folder;
        QByteArray name;
        bool isFolder = false;
    };
    QList<Made> made;

    void undo()
    {
        // A folder that something else has written into since is not empty and stays.
        for (auto entry = made.crbegin(); entry != made.crend(); ++entry)
            ::unlinkat(entry->folder->descriptor, entry->name.constData(), entry->isFolder ? AT_REMOVEDIR : 0);
        made.clear();
    }
};

// How placing a draft's assets went: placed, left in the draft because a
// folder they would go in is not a folder of its own, or failed.
enum class Placed { All, Left, Failed };

// Opens, or makes and opens, each folder of a relative path beneath root,
// and records the ones it made; the last is `leaf`. A folder already there
// is used only if it is one of its own, opened without following a link:
// anything else leaves the assets where they are. Folders opened already
// are taken from `opened`.
Placed makeFolders(const FolderRef &root, const QString &shownRoot, const QString &relative, Placement &placement,
                   QHash<QString, FolderRef> &opened, FolderRef &leaf, QString &error)
{
    FolderRef current = root;
    QString walked;
    for (const QString &part : relative.split(u'/', Qt::SkipEmptyParts)) {
        walked += (walked.isEmpty() ? QString() : QStringLiteral("/")) + part;
        if (const FolderRef found = opened.value(walked)) {
            current = found;
            continue;
        }
        const QByteArray name = QFile::encodeName(part);
        const bool made = ::mkdirat(current->descriptor, name.constData(), 0777) == 0;
        if (!made && errno != EEXIST) {
            error = DocumentStore::tr("Cannot create the folder %1: %2").arg(shownRoot + u'/' + walked, errorText(errno));
            return Placed::Failed;
        }
        if (made) placement.made.append({current, name, true});
        auto next = std::make_shared<Folder>();
        next->descriptor = ::openat(current->descriptor, name.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next->descriptor < 0) {
            const int failure = errno;
            if (failure == ELOOP || failure == ENOTDIR) return Placed::Left;
            error = DocumentStore::tr("Cannot create the folder %1: %2").arg(shownRoot + u'/' + walked, errorText(failure));
            return Placed::Failed;
        }
        opened.insert(walked, next);
        current = next;
    }
    leaf = current;
    return Placed::All;
}

// Puts one draft asset at its name in the folder opened for it. A file
// already there with the same bytes is used as it is; anything else there is
// refused, so no existing file is ever replaced.
bool placeAsset(const QString &source, const FolderRef &folder, const QString &target, const QString &name,
                Placement &placement, QString &error)
{
    const QString shownName = QStringLiteral("assets/") + name;
    if (!isRegularFile(source)) {
        error = DocumentStore::tr("Cannot read the draft's %1").arg(shownName);
        return false;
    }
    const QByteArray leaf = QFile::encodeName(name.mid(name.lastIndexOf(u'/') + 1));
    struct stat existing {};
    if (::fstatat(folder->descriptor, leaf.constData(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISREG(existing.st_mode) && sameBytes(source, folder->descriptor, leaf)) return true;
        error = DocumentStore::tr("A different file is already at %1").arg(shownName);
        return false;
    }
    if (beforePlacing) beforePlacing(target);
    // O_EXCL creates the file only while nothing, not even a link, has the
    // name, so a file that appeared after the check is not overwritten either.
    const int descriptor =
        ::openat(folder->descriptor, leaf.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (descriptor < 0) {
        const int failure = errno;
        error = failure == EEXIST ? DocumentStore::tr("A different file is already at %1").arg(shownName)
                                  : DocumentStore::tr("Cannot copy %1: %2").arg(shownName, errorText(failure));
        return false;
    }
    placement.made.append({folder, leaf, false});
    QFile output;
    if (!output.open(descriptor, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        error = DocumentStore::tr("Cannot copy %1: %2").arg(shownName, output.errorString());
        return false;
    }
    QFile input(source);
    bool copied = input.open(QIODevice::ReadOnly);
    while (copied) {
        const QByteArray chunk = input.read(1 << 16);
        if (chunk.isEmpty()) break;
        copied = output.write(chunk) == chunk.size();
    }
    // The copy reaches the disk before the named file refers to it.
    copied = copied && input.error() == QFileDevice::NoError && output.flush() && ::fdatasync(output.handle()) == 0;
    if (!copied) error = DocumentStore::tr("Cannot copy %1").arg(shownName);
    return copied;
}

// Copies a draft's assets into an assets folder beside the named file, in
// name order so that a failure is the same each time it is tried. When a
// folder they would go in is not a folder of its own, as a link put there,
// nothing is placed, and the assets stay in the draft. Every folder is
// opened once and written through, never by its path.
Placed placeAssets(const QString &source, const QString &targetFolder, Placement &placement, QString &error,
                   QStringList &placed)
{
    QStringList names;
    const QDir sourceDir(source);
    QDirIterator entries(source, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (entries.hasNext()) names.append(sourceDir.relativeFilePath(entries.next()));
    if (names.isEmpty()) return Placed::All;
    names.sort();
    // Only folders inside the target's folder are ever made, never the target's folder itself.
    auto root = std::make_shared<Folder>();
    root->descriptor = ::open(QFile::encodeName(targetFolder).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root->descriptor < 0) {
        error = DocumentStore::tr("The folder %1 does not exist").arg(targetFolder);
        return Placed::Failed;
    }
    QHash<QString, FolderRef> opened;
    for (const QString &name : std::as_const(names)) {
        const qsizetype slash = name.lastIndexOf(u'/');
        const QString folder = slash < 0 ? QStringLiteral("assets") : QStringLiteral("assets/") + name.left(slash);
        FolderRef leaf;
        const Placed made = makeFolders(root, targetFolder, folder, placement, opened, leaf, error);
        if (made != Placed::All) {
            placement.undo();
            return made;
        }
        if (!placeAsset(source + u'/' + name, leaf, targetFolder + QStringLiteral("/assets/") + name, name, placement,
                        error)) {
            placement.undo();
            return Placed::Failed;
        }
    }
    placed = names;
    return Placed::All;
}

// Takes from a named draft's folder what went with the draft: its file and
// the assets now beside the named file, and then the folders that leaves
// empty. Anything else there, as an image pasted while the naming ran,
// stays where it is, and the folders that hold it with it.
void removeNamedDraft(const QString &folder, const QStringList &assets)
{
    QFile::remove(folder + u'/' + draftName);
    QStringList folders;
    for (const QString &name : assets) {
        QFile::remove(folder + QStringLiteral("/assets/") + name);
        for (qsizetype slash = name.lastIndexOf(u'/'); slash > 0; slash = name.lastIndexOf(u'/', slash - 1))
            if (!folders.contains(name.left(slash))) folders.append(name.left(slash));
    }
    // The deepest first, and each only while it is empty.
    std::sort(folders.begin(), folders.end(), [](const QString &a, const QString &b) { return a.count(u'/') > b.count(u'/'); });
    for (const QString &inner : std::as_const(folders)) QDir().rmdir(folder + QStringLiteral("/assets/") + inner);
    QDir().rmdir(folder + QStringLiteral("/assets"));
    QDir().rmdir(folder);
}
} // namespace

DocumentStore::DocumentStore(const AppPaths &paths, QObject *parent) : DocumentStore(paths, HistoryLimits{}, parent)
{
}

DocumentStore::DocumentStore(const AppPaths &paths, HistoryLimits limits, QObject *parent)
    : QObject(parent), m_paths(paths), m_history(std::make_unique<History>(paths, limits))
{
    m_registryWriter.setMaxThreadCount(1);
    loadRegistry();
    pruneRegistry();
    const auto touched = [this](const QString &path) {
        Document *document = documentAt(path);
        checkDisk(document);
        if (document) emit diskChanged(document);
    };
    connect(&m_watch, &FileWatch::changedOnDisk, this, touched);
    connect(&m_watch, &FileWatch::removedFromDisk, this, touched);
}

DocumentStore::~DocumentStore()
{
    // Saves in flight finish before the store goes, so that none is cut short
    // or lands after the files around it have been cleaned up.
    for (auto *watcher : std::as_const(m_running)) watcher->waitForFinished();
    for (auto *watcher : std::as_const(m_checks)) watcher->waitForFinished();
    m_registryWriter.waitForDone();
}

Document *DocumentStore::newDraft()
{
    return adopt(new Document(newId(), {}, {}, {}, {}, false, this));
}

Document *DocumentStore::openDraft(const QString &documentId, QString *error)
{
    // A damaged session cannot point outside the drafts, since only a real id names a folder.
    if (!AppPaths::isDocumentId(documentId)) return nullptr;
    // A named draft belongs to its file now; a draft folder left behind is not a second document.
    const auto namedSince = [this, error](const QString &path) -> Document * {
        if (error)
            *error = tr("This is the old copy of a draft since named “%1” in “%2”, so it is not opened")
                         .arg(PathText::fileName(path), shownFolder(path));
        return nullptr;
    };
    if (Document *live = documentWithId(documentId)) return live->isDraft() ? handOut(live) : namedSince(live->path());
    for (auto named = m_registry.cbegin(); named != m_registry.cend(); ++named)
        if (named.value() == documentId) return namedSince(named.key());
    // Nor is an id whose text recovery holds, at the path it is recorded at.
    if (const auto held = m_held.constFind(documentId); held != m_held.cend()) return namedSince(held->path);
    const QString file = draftFolder(documentId) + u'/' + draftName;
    const LoadedFile loaded = DiskFile::load(file);
    if (!loaded.ok) {
        if (error) *error = cannotOpen(file, loaded);
        return nullptr;
    }
    return adopt(new Document(documentId, {}, loaded.text, loaded.convention, loaded.version, loaded.readOnly, this));
}

QString DocumentStore::recordedSpelling(const QString &path, const QString &canonical) const
{
    if (m_registry.contains(path)) return path;
    // Tidied as text, before any folder is resolved: a folder on the
    // recorded path that a link replaced since would be resolved away, and
    // the recorded path missed. Text alone takes ".." back past a link to
    // a place the kernel would not go, so a recorded path found this way
    // counts only while it leads where the path given does.
    bool same = false;
    const QString tidy = DiskFile::tidied(path, &same);
    if (tidy != path && m_registry.contains(tidy)
        && (same || tidy == canonical || DiskFile::canonicalPath(tidy) == canonical))
        return tidy;
    // Only now is the folder resolved, and once: a path through a folder
    // that was a link when the document was recorded is found so. A path
    // the registry does not know either way is spelled clean by what has
    // been asked already.
    const QString literal = DiskFile::literalPath(path);
    return m_registry.contains(literal) || tidy != path ? literal : path;
}

Document *DocumentStore::reopen(const QString &path, const QString &documentId, QString *error)
{
    const QString canonical = DiskFile::canonicalPath(path);
    const QString recorded = recordedSpelling(path, canonical);
    if (canonical != recorded) {
        const QString registered = m_registry.value(recorded);
        const QString id =
            !registered.isEmpty() ? registered : AppPaths::isDocumentId(documentId) ? documentId : QString();
        if (!id.isEmpty()) return openElsewhere(recorded, id, canonical, error);
    }
    // The names already asked for are used again, not asked twice.
    return openAt(path, canonical, recorded, error);
}

bool DocumentStore::holdsRecovered(const QString &documentId) const
{
    return m_held.contains(documentId);
}

History::CaptureStatus DocumentStore::keepInHistoryNow(const QString &documentId, const QByteArray &bytes,
                                                       History::Reason reason)
{
    // Shared with the capture, which may end after the wait has.
    struct Kept {
        QSemaphore done;
        History::CaptureStatus status = History::CaptureStatus::Failed;
    };
    const auto kept = std::make_shared<Kept>();
    auto *watcher = new QFutureWatcher<void>(this);
    m_running.append(watcher);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        m_running.removeOne(watcher);
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([history = m_history.get(), documentId, bytes, reason, kept] {
        History::CaptureStatus status = History::CaptureStatus::Failed;
        history->captureUnlessNewest(documentId, bytes, reason, &status);
        kept->status = status;
        kept->done.release();
    }));
    return kept->done.tryAcquire(1, HistoryWaitMs) ? kept->status : History::CaptureStatus::Failed;
}

QFuture<void> DocumentStore::keepInHistory(const QString &documentId, const QByteArray &bytes, History::Reason reason,
                                           std::function<void()> then)
{
    auto *watcher = new QFutureWatcher<void>(this);
    m_running.append(watcher);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, then = std::move(then)] {
        m_running.removeOne(watcher);
        watcher->deleteLater();
        if (then) then();
    });
    watcher->setFuture(QtConcurrent::run([history = m_history.get(), documentId, bytes, reason] {
        history->captureUnlessNewest(documentId, bytes, reason);
    }));
    return watcher->future();
}

void DocumentStore::hold(const Recovery::Entry &entry, const QString &path)
{
    if (m_held.contains(entry.documentId)) return;
    // The entry stays where its text is until it is restored. History is
    // given a second copy, begun now, before anything can be opened.
    const QByteArray bytes = DiskFile::encode(entry.text, entry.hasConvention ? entry.convention : TextConvention{});
    m_held.insert(entry.documentId,
                  Held{entry, path, keepInHistory(entry.documentId, bytes, History::Reason::Recovery)});
}

const DocumentStore::Held *DocumentStore::heldAt(const QString &path) const
{
    for (const Held &held : m_held)
        if (held.path == path) return &held;
    return nullptr;
}

Document *DocumentStore::restoreHeld(const Held &held, const LoadedFile &read)
{
    // Copies, since restoring the entry lets the hold go.
    const Recovery::Entry entry = held.entry;
    const QString path = held.path;
    Recovered how = Recovered::AlreadySaved;
    Document *document = restoreAt(entry, path, &read, &how);
    if (document) emit heldRestored(document, how);
    return document;
}

Document *DocumentStore::openElsewhere(const QString &path, const QString &documentId, const QString &target,
                                       QString *error)
{
    if (Document *live = documentWithId(documentId); live && live->m_path == path) return handOut(live);
    if (Document *live = documentAt(path)) return handOut(live);
    // Text recovery holds for it, by its id or at its path, is never
    // covered by a document with no text of its own.
    if (m_held.contains(documentId) || m_held.contains(m_registry.value(path)) || heldAt(path)) {
        if (error)
            *error = tr("The text of “%1” is kept in recovery, and its path now leads to “%2”, so it is not opened")
                         .arg(PathText::fileName(path), target);
        return nullptr;
    }
    // A document of that id open elsewhere keeps it; this one is new.
    const bool taken = documentWithId(documentId) != nullptr;
    const QString id = taken ? newId() : documentId;
    if (!taken && m_registry.value(path) != id) {
        m_registry.insert(path, id);
        writeRegistry();
    }
    auto *document = new Document(id, path, {}, {}, {}, false, this);
    document->m_textless = true;
    document->m_problem = Document::Problem::Conflict;
    document->m_leadsTo = target;
    document->m_lastError = leadsElsewhere(path, target);
    document->refresh();
    return adopt(document);
}

Document *DocumentStore::open(const QString &path, QString *error)
{
    // Identity is the kernel's answer for the path, so a symlink, a relative
    // path or ".." reach the document that is already open. It is asked
    // without reading the file, which is read once, below.
    const QString canonical = DiskFile::canonicalPath(path);
    return openAt(path, canonical, recordedSpelling(path, canonical), error);
}

Document *DocumentStore::openAt(const QString &path, const QString &canonical, const QString &spelled, QString *error)
{
    // A path recorded as a document's file, in the registry or by text
    // recovery holds for it, however it is spelled, which leads to another
    // file now.
    if (const QString recorded = m_registry.value(spelled);
        (!recorded.isEmpty() || heldAt(spelled)) && canonical != spelled)
        return openElsewhere(spelled, recorded, canonical, error);
    if (Document *live = documentAt(canonical)) return handOut(live);
    // A draft's own file opens as that draft, or the draft would have two writers.
    const QString draft = draftIdAt(canonical);
    if (!draft.isEmpty()) return openDraft(draft, error);
    const LoadedFile loaded = DiskFile::load(path);
    if (!loaded.ok) {
        // Named as it is spelled clean, however it was asked for.
        if (error) *error = cannotOpen(spelled, loaded);
        return nullptr;
    }
    // The file can have been replaced between the two reads; what was loaded counts.
    const QString loadedPath = loaded.version.canonicalPath;
    if (Document *live = documentAt(loadedPath)) return handOut(live);
    // Text recovery holds for this file comes back over it, as a restart
    // would bring it, rather than the file opening without it.
    if (const Held *held = heldAt(loadedPath))
        if (Document *restored = restoreHeld(*held, loaded)) return restored;
    // An id recovery holds text under, recorded for another path, is that
    // entry's alone, as one a document is open under is that document's.
    QString id = m_registry.value(loadedPath);
    if (id.isEmpty() || documentWithId(id) || m_held.contains(id)) {
        id = newId();
        m_registry.insert(loadedPath, id);
        writeRegistry();
    }
    // What the file holds now was read, not written, by this document.
    forgetSaves(id);
    return adopt(new Document(id, loadedPath, loaded.text, loaded.convention, loaded.version, loaded.readOnly, this));
}

Document *DocumentStore::adoptCreated(const DiskVersion &made, const QString &text, const TextConvention &convention,
                                      QString *error)
{
    if (documentAt(made.canonicalPath)) {
        *error = tr("another document is already at %1").arg(made.canonicalPath);
        return nullptr;
    }
    const QString id = newId();
    m_registry.insert(made.canonicalPath, id);
    writeRegistry();
    return adopt(new Document(id, made.canonicalPath, text, convention, made, false, this));
}

QString DocumentStore::recordedPath(const Recovery::Entry &entry) const
{
    // A draft that was named after its last snapshot belongs to its file now.
    QString path = entry.path;
    if (path.isEmpty()) {
        for (auto named = m_registry.cbegin(); named != m_registry.cend(); ++named)
            if (named.value() == entry.documentId) path = named.key();
    }
    return path;
}

Document *DocumentStore::restore(const Recovery::Entry &entry, Recovered *recovered)
{
    return restoreAt(entry, recordedPath(entry), nullptr, recovered);
}

Document *DocumentStore::restoreAt(const Recovery::Entry &entry, const QString &path, const LoadedFile *read,
                                   Recovered *recovered)
{
    // An entry whose id is open is that document's, which writes it.
    if (!AppPaths::isDocumentId(entry.documentId) || documentWithId(entry.documentId)) return nullptr;
    const bool draft = path.isEmpty();
    const QString file = draft ? draftFolder(entry.documentId) + u'/' + draftName : path;
    // The file is read once. One that could not be read gets an identity
    // that matches nothing read, as fingerprint() gives it.
    const QString canonical = read ? read->version.canonicalPath : DiskFile::canonicalPath(file);
    // Another document has the file: the entry stays in recovery as it is,
    // held until it can be restored.
    if (!draft && documentAt(canonical)) {
        hold(entry, path);
        return nullptr;
    }
    // The text of an entry held until now is in history, or history has
    // refused it, before its id is a document's again.
    if (const auto held = m_held.constFind(entry.documentId); held != m_held.cend())
        QFuture<void>(held->kept).waitForFinished();
    const LoadedFile loaded = read ? *read : DiskFile::load(file);
    // The path leads to another file than the document's own now, as when a
    // link was put in its place: that file is neither taken nor written.
    const QString leadsTo = !draft && loaded.ok && loaded.version.canonicalPath != path
                                ? loaded.version.canonicalPath
                                : QString();
    const DiskVersion now = loaded.ok || !loaded.version.canonicalPath.isEmpty()
                                ? loaded.version
                                : DiskVersion{canonical, 0, 0, {}, !loaded.missing, {}};
    const bool readable = loaded.ok && !loaded.readOnly;
    const QString documentPath = draft ? QString() : (loaded.ok && leadsTo.isEmpty() ? loaded.version.canonicalPath : path);
    const Recovery recovery(m_paths);
    const std::optional<Recovery::Save> lastSave = recovery.lastSave(entry.documentId);
    // The restored text counts its revisions on from those recorded before,
    // so that a later restart still compares them in the order of the edits.
    const auto counted = [&](Document *document) {
        document->m_buffer->continueAfter(std::max(entry.revision, lastSave ? lastSave->revision : 0));
        return document;
    };
    const auto adopted = [&](Document *document, Recovered how) {
        if (recovered) *recovered = how;
        // The entry is this document's from here on, to write and to remove.
        m_held.remove(entry.documentId);
        if (!draft) {
            m_registry.insert(documentPath, entry.documentId);
            writeRegistry();
        }
        // parch's saves tell about the file only while it holds what the
        // newest of them wrote. Anything else came from elsewhere, even
        // content an earlier save wrote, or the entry's own base.
        if (!readable || !recovery.savedLast(entry.documentId, loaded.version.sha256))
            forgetSaves(entry.documentId);
        return adopt(counted(document));
    };

    // What a save writes the text with, when the file cannot say: as the
    // entry recorded it.
    const TextConvention recorded = entry.hasConvention ? entry.convention : TextConvention{};
    if (!leadsTo.isEmpty()) {
        Document *document = counted(new Document(entry.documentId, path, {}, recorded, entry.base, false, this));
        document->m_buffer->replace({0, 0}, entry.text);
        document->m_problem = Document::Problem::Conflict;
        document->m_leadsTo = leadsTo;
        document->m_lastError = leadsElsewhere(path, leadsTo);
        document->refresh();
        return adopted(document, Recovered::Shown);
    }
    // Text the file already holds needs no recovery.
    if (readable && loaded.text == entry.text) {
        recovery.discard(entry.documentId);
        return adopted(new Document(entry.documentId, documentPath, loaded.text, loaded.convention, loaded.version,
                                    false, this),
                       Recovered::AlreadySaved);
    }
    // The recovered edits are safe to save over the file the text was based
    // on. A path that leads nowhere, such as one beneath a file or in a
    // folder that is gone, counts as absent like a missing file; a file that
    // is there but cannot be read is not absent, and says why it cannot be read.
    const bool absent = !now.exists || (!loaded.ok && loaded.missing);
    const bool unreadable = !loaded.ok && !loaded.unreadable.isEmpty();
    // Device and inode numbers need not survive a restart, so only the file's
    // place and content are compared. An entry that recorded no base cannot
    // show that a named file is unchanged, or that its absence is expected.
    const bool sameBase = (!entry.base.exists && absent) || sameContentIdentity(entry.base, now);
    const bool unknownBase = !entry.hasBase && !entry.path.isEmpty();
    // A file that differs from the base can still be parch's own: a save
    // that landed after the entry was last written, before parch ended. A
    // draft's own file is only ever parch's; a named file is when it is
    // what parch's newest save wrote, and parch's saves lead to it from the
    // entry's base.
    const bool own =
        readable && !sameBase
        && (draft
            || (!unknownBase && recovery.savedSince(entry.documentId, contentOf(entry.base), loaded.version.sha256)));
    if (own) {
        // The newer text is shown and the older one kept in history, so that
        // neither is lost. Newer is later in the order of the edits: the
        // revision parch's newest save wrote, when the file holds what that
        // save wrote, against the entry's. Only without such a save, as for
        // a draft whose saves were forgotten, does the file's time stand in.
        const std::optional<Recovery::Save> held = recovery.saveHeld(entry.documentId, loaded.version.sha256);
        const bool fileNewer = held ? held->revision >= entry.revision
                                    : !entry.written.isValid() || QFileInfo(file).lastModified() >= entry.written;
        History::CaptureStatus status = History::CaptureStatus::Failed;
        m_history->captureUnlessNewest(entry.documentId,
                                       fileNewer ? DiskFile::encode(entry.text, loaded.convention)
                                                 : loaded.originalBytes,
                                       History::Reason::Recovery, &status);
        if (status == History::CaptureStatus::Recorded || status == History::CaptureStatus::Held) {
            auto *document = new Document(entry.documentId, documentPath, loaded.text, loaded.convention,
                                          loaded.version, false, this);
            if (fileNewer) {
                recovery.discard(entry.documentId);
                return adopted(document, Recovered::InHistory);
            }
            counted(document)->m_buffer->replace({0, document->m_buffer->length()}, entry.text);
            return adopted(document, Recovered::Shown);
        }
        // History cannot keep the older text, so both stay in view, the
        // file's on disk and the entry's here, until the writer chooses.
    }
    // A named file that is different, gone, or unreadable now may hold
    // someone else's work, so the document waits in conflict for a choice. A
    // draft takes its recovered text over a file that is missing or cannot be
    // read as text, once history keeps what can be read of it; while nothing
    // keeps those bytes, the draft waits in conflict too, so that no save
    // replaces them.
    bool unkept = false;
    if (draft && !own && !sameBase && loaded.ok && loaded.readOnly) {
        History::CaptureStatus status = History::CaptureStatus::Failed;
        m_history->captureUnlessNewest(entry.documentId, loaded.originalBytes, History::Reason::Recovery, &status);
        unkept = status != History::CaptureStatus::Recorded && status != History::CaptureStatus::Held;
    }
    const bool conflict = own || unkept || (!draft && (!sameBase || unknownBase || (loaded.ok && loaded.readOnly)));
    Document *document = counted(readable ? new Document(entry.documentId, documentPath, loaded.text,
                                                         loaded.convention, loaded.version, false, this)
                                          : new Document(entry.documentId, documentPath, {}, recorded,
                                                         draft && absent ? DiskVersion{} : now, false, this));
    document->m_buffer->replace({0, document->m_buffer->length()}, entry.text);
    if (conflict) {
        // The base stays the version the edits were made on, so that a snapshot
        // written from here still tells the next restart about the conflict.
        document->m_diskVersion = entry.base;
        document->m_problem = Document::Problem::Conflict;
        document->m_fileMissing = absent;
        document->m_fileUnreadable = unreadable;
        document->m_lastError =
            unreadable ? cannotRead(path, loaded) : tr("The file has changed since this text was last saved");
        document->refresh();
    }
    return adopted(document, Recovered::Shown);
}

DocumentStore::KeptCopy DocumentStore::keepBothJob(const QString &path, const QByteArray &local, const QString &stamp,
                                                   const QSet<QString> &open, quint32 ownMode)
{
    KeptCopy kept;
    kept.disk = DiskFile::load(path);
    if (!kept.disk.ok) return kept;
    // The copy has the mode of the file it is kept beside: the document's
    // own, as it is now, or as it last was when a link leads elsewhere.
    const quint32 mode = kept.disk.version.canonicalPath == path ? kept.disk.version.mode : ownMode;
    const QFileInfo info(path);
    // A second conflict in the same second must not replace the first copy.
    for (int attempt = 1; attempt < 100; ++attempt) {
        const QString suffix = attempt == 1 ? QString() : QStringLiteral(" %1").arg(attempt);
        const QString name = info.absolutePath() + u'/' + info.completeBaseName()
                             + QStringLiteral(" (conflict %1%2).md").arg(stamp, suffix);
        // A name parch makes up is made at that very path: anything already
        // there, a link included, or a document open at it such as the copy
        // of an earlier choice, is never replaced or followed, and the next
        // name is tried, as for a file that takes the name while the copy is
        // written.
        if (open.contains(DiskFile::literalPath(name))) continue;
        kept.copy = DiskFile::create(name, local, mode);
        if (kept.copy.status != SaveStatus::Conflict) break;
    }
    if (kept.copy.status != SaveStatus::Saved) {
        if (kept.copy.error.isEmpty()) kept.copy.error = tr("No free name for it");
        kept.copy.status = SaveStatus::Failed;
        return kept;
    }
    // The copy is the file that was made, taken as that file and never
    // opened again by its name: its name is looked at without following
    // it, and must still hold that very file, never a link or a file put
    // in its place since.
    struct stat state {};
    const DiskVersion &made = kept.copy.version;
    if (::lstat(QFile::encodeName(made.canonicalPath).constData(), &state) != 0 || !S_ISREG(state.st_mode)
        || quint64(state.st_dev) != made.device || quint64(state.st_ino) != made.inode) {
        kept.copy.status = SaveStatus::Conflict;
        kept.copy.error = tr("something else took its place at %1").arg(made.canonicalPath);
    }
    return kept;
}

void DocumentStore::keepBoth(Document *document)
{
    const QString path = fileOf(document);
    const auto snapshot = document->m_buffer->snapshot();
    const QByteArray local = DiskFile::encode(snapshot->text, document->m_convention);
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HHmmss"));
    QSet<QString> open;
    for (const Document *other : std::as_const(m_documents))
        if (!other->m_path.isEmpty()) open.insert(other->m_path);
    for (auto claim = m_claims.cbegin(); claim != m_claims.cend(); ++claim) open.insert(claim.key());
    document->m_resolving = true;
    // The document as the choice found it.
    const DiskVersion version = document->m_diskVersion;
    const QString leadsTo = document->m_leadsTo, own = document->m_path, title = document->title();
    const bool missing = document->m_fileMissing;
    auto *watcher = new QFutureWatcher<KeptCopy>(this);
    m_running.append(watcher);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, path, snapshot, version, leadsTo, own, missing, title, target = QPointer<Document>(document)] {
        m_running.removeOne(watcher);
        watcher->deleteLater();
        const KeptCopy kept = watcher->result();
        // A document closed meanwhile is gone, and its copy stays, said.
        if (!target || !m_documents.contains(target)) {
            if (kept.copy.status == SaveStatus::Saved) emit conflictCopyLeft(title, kept.copy.version.canonicalPath);
            return;
        }
        Document *document = target;
        document->m_resolving = false;
        // The choice acts only on the document as it found it: one saved,
        // named, or out of that conflict since is left as it is, and the
        // copy, which holds its text as it was then, stays on disk.
        const bool moved = document->m_problem != Document::Problem::Conflict || document->m_saving
                           || document->m_path != own || document->m_diskVersion != version
                           || document->m_leadsTo != leadsTo || document->m_fileMissing != missing;
        if (moved) {
            if (kept.copy.status == SaveStatus::Saved) emit conflictCopyLeft(title, kept.copy.version.canonicalPath);
            return;
        }
        const LoadedFile &disk = kept.disk;
        if (!disk.ok) {
            const bool gone = disk.missing;
            if (gone) document->m_fileMissing = true;
            refuseChoice(document, gone ? tr("The file no longer exists") : disk.error, nullptr);
            return;
        }
        const QString elsewhere = disk.version.canonicalPath != path ? disk.version.canonicalPath : QString();
        if (!elsewhere.isEmpty()) showElsewhere(document, elsewhere);
        if (kept.copy.status != SaveStatus::Saved) {
            refuseChoice(document,
                         kept.copy.status == SaveStatus::Conflict
                             ? tr("Cannot open the conflict copy: %1").arg(kept.copy.error)
                             : tr("Cannot save the conflict copy: %1").arg(kept.copy.error),
                         nullptr);
            return;
        }
        QString error;
        Document *copy = adoptCreated(kept.copy.version, snapshot->text, document->m_convention, &error);
        if (!copy) {
            refuseChoice(document, tr("Cannot open the conflict copy: %1").arg(error), nullptr);
            return;
        }
        emit conflictCopyOpened(copy, document);
        // A document whose path leads to another file keeps its own text,
        // and waits on; so does one written to while the copy was made,
        // whose newer text the copy lacks.
        if (elsewhere.isEmpty() && document->m_buffer->revision() == snapshot->revision) takeFile(document, disk);
    });
    watcher->setFuture(
        QtConcurrent::run(&DocumentStore::keepBothJob, path, local, stamp, open, document->m_diskVersion.mode));
}

DocumentStore::Followed DocumentStore::followJob(const QString &path, const AppPaths &paths)
{
    Followed followed;
    followed.disk = DiskFile::load(path);
    const QString &leadsTo = followed.disk.version.canonicalPath;
    if (!followed.disk.ok || leadsTo == path) return followed;
    // Drafts, history and the registry, recovery and the session, and
    // settings are parch's own, in folders of its own; in the session's
    // runtime folder, which others share, only its socket and the lock
    // beside it are.
    for (const QString &folder : {paths.data, paths.state, paths.config}) {
        const QString own = QFileInfo(folder).canonicalFilePath();
        if (!own.isEmpty() && leadsTo.startsWith(own + u'/')) followed.own = true;
    }
    const QString runtime = QFileInfo(paths.runtime).canonicalFilePath();
    const QString socket = PathText::fileName(AppService::socketPath(paths));
    if (!runtime.isEmpty() && PathText::folderOf(leadsTo) == runtime && PathText::fileName(leadsTo).startsWith(socket))
        followed.own = true;
    return followed;
}

bool DocumentStore::mayFollow(Document *document, const Chosen &chosen, const Followed &followed)
{
    // Out of the conflict since, as when its own file came back and was
    // taken: there is nothing left to choose.
    if (document->m_problem != Document::Problem::Conflict) return false;
    // Text typed meanwhile, a save, or another change of the document's
    // own: nothing is taken, and the text stays as it is.
    if (document->m_saving || document->m_path != chosen.path || document->m_diskVersion != chosen.version
        || document->m_leadsTo != chosen.leadsTo || document->m_fileMissing != chosen.missing
        || document->m_buffer->revision() != chosen.revision)
        return refuseChoice(document,
                            tr("“%1” changed while the link was being followed, so it was not followed: review it again")
                                .arg(document->title()),
                            nullptr);
    const LoadedFile &disk = followed.disk;
    if (!disk.ok) {
        if (disk.missing) document->m_fileMissing = true;
        return refuseChoice(document, disk.missing ? tr("The file no longer exists") : disk.error, nullptr);
    }
    const QString leadsTo = disk.version.canonicalPath != chosen.path ? disk.version.canonicalPath : QString();
    if (leadsTo.isEmpty())
        return refuseChoice(document,
                            tr("“%1” leads to its own file, so there is no link to follow").arg(document->title()), nullptr);
    // Only the file the writer was shown is followed.
    if (leadsTo != chosen.leadsTo) {
        showElsewhere(document, leadsTo);
        return refuseChoice(document,
                            tr("“%1” now leads to “%2”, not to the file shown when Follow the link was chosen, so it was "
                               "not followed: review it again")
                                .arg(document->title(), leadsTo),
                            nullptr);
    }
    if (followed.own)
        return refuseChoice(document,
                            tr("“%1” is one of parch's own files; parch does not follow a link there.").arg(leadsTo),
                            nullptr);
    if (Document *other = documentAt(leadsTo); other && other != document)
        return refuseChoice(document, tr("Another document is already at %1").arg(leadsTo), nullptr);
    return true;
}

void DocumentStore::follow(Document *document)
{
    document->m_resolving = true;
    // What the writer was shown when choosing: the link is followed only
    // while all of it still holds.
    const Chosen chosen{document->m_path, document->m_diskVersion, document->m_leadsTo, document->m_fileMissing,
                        document->m_buffer->revision()};
    // The writer chose to take that file: it is the document's file from
    // here on, and its text the document's.
    const auto take = [this](Document *document, const LoadedFile &disk) {
        m_registry.remove(document->m_path);
        m_watch.unwatch(document->m_path);
        document->m_path = disk.version.canonicalPath;
        watchPath(document->m_path);
        m_registry.insert(document->m_path, document->m_id);
        writeRegistry();
        takeFile(document, disk);
    };
    auto *watcher = new QFutureWatcher<Followed>(this);
    m_running.append(watcher);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, chosen, take, target = QPointer<Document>(document)] {
        m_running.removeOne(watcher);
        watcher->deleteLater();
        if (!target || !m_documents.contains(target)) return;
        Document *document = target;
        const Followed followed = watcher->result();
        const bool follows = mayFollow(document, chosen, followed);
        // A document with no text of its own has none to keep first.
        if (!follows || document->m_textless) {
            document->m_resolving = false;
            if (follows) take(document, followed.disk);
            return;
        }
        // Nothing stands in the way, so the text here is kept in history
        // first, on a worker too; the file is taken once that has ended,
        // if nothing stands in the way then either.
        keepInHistory(document->m_id, DiskFile::encode(document->m_buffer->text(), document->m_convention),
                      History::Reason::Reload, [this, chosen, followed, take, target] {
            if (!target || !m_documents.contains(target)) return;
            target->m_resolving = false;
            if (mayFollow(target, chosen, followed)) take(target, followed.disk);
        });
    });
    watcher->setFuture(QtConcurrent::run(&DocumentStore::followJob, chosen.path, m_paths));
}

bool DocumentStore::resolveConflict(Document *document, ConflictChoice choice, QString *refusal)
{
    const auto refused = [refusal](const QString &reason) {
        if (refusal) *refusal = reason;
        return false;
    };
    if (!m_documents.contains(document)) return refused(tr("The document is no longer open"));
    if (document->m_problem != Document::Problem::Conflict)
        return refused(tr("%1 is not in conflict with its file, so there is nothing to choose").arg(document->title()));
    if (document->m_readOnly) return refused(tr("This document is read-only"));
    if (document->m_saving || document->m_resolving)
        return refused(tr("A save of this document is still running; choose again once it ends"));
    // A document with no text of its own can only take a file: the one the
    // link leads to, by following it, or its own, by reloading it once no
    // link is left.
    if (document->m_textless && choice != ConflictChoice::Follow
        && (choice != ConflictChoice::Reload || !document->m_leadsTo.isEmpty() || document->m_fileMissing))
        return refuseChoice(document, textlessReason(document), refusal);
    // A copy beside a draft's own file would be inside parch's drafts folder.
    if (document->isDraft() && choice == ConflictChoice::KeepBoth)
        return refuseChoice(
            document, tr("A draft has no folder to keep a copy in: reload, overwrite or save it elsewhere"), refusal);
    // The file is read, and the copy written, on a worker; what they found
    // is taken when they end.
    if (choice == ConflictChoice::KeepBoth) {
        keepBoth(document);
        return true;
    }
    // The file the link leads to is read on a worker, and then, if nothing
    // refuses it, this text is kept in history on a worker; the document
    // takes that file once both have ended.
    if (choice == ConflictChoice::Follow) {
        if (document->isDraft())
            return refuseChoice(
                document, tr("“%1” leads to its own file, so there is no link to follow").arg(document->title()), refusal);
        follow(document);
        return true;
    }
    // The text goes at the document's own name, in place of the link that
    // leads elsewhere from there, on the save's worker, which replaces
    // nothing but a link.
    if (choice == ConflictChoice::PutHere) {
        if (document->isDraft() || document->m_leadsTo.isEmpty())
            return refuseChoice(document,
                                tr("“%1” leads to no other file, so there is no link to put this text in place of")
                                    .arg(document->title()),
                                refusal);
        putHere(document, {});
        return true;
    }
    // The file as it is now, never the version the conflict was found at, since
    // it can have changed again, and a conflict from a restart or a failed
    // save has no watcher's version at all.
    const QString path = fileOf(document);
    const LoadedFile disk = DiskFile::load(path);
    // The file the path leads to now, when it is not the document's own.
    const QString leadsTo =
        !document->isDraft() && disk.ok && disk.version.canonicalPath != path ? disk.version.canonicalPath : QString();
    if (!leadsTo.isEmpty()) showElsewhere(document, leadsTo);
    // Nothing is written to that file, or taken from it, but by following the link.
    if (!leadsTo.isEmpty() && (choice == ConflictChoice::Overwrite || choice == ConflictChoice::Reload))
        return refuseChoice(document, leadsElsewhere(path, leadsTo), refusal);
    // Nothing is there, or a folder on its path is gone too.
    const bool gone = !disk.ok && disk.missing;
    if (gone) document->m_fileMissing = true;
    if (gone && choice != ConflictChoice::Overwrite)
        return refuseChoice(document, tr("The file no longer exists"), refusal);
    if (!gone && !disk.ok) return refuseChoice(document, disk.error, refusal);
    const QByteArray local = DiskFile::encode(document->m_buffer->text(), document->m_convention);

    if (choice == ConflictChoice::Overwrite) {
        // What is about to be replaced goes to history first. A failed capture
        // never blocks the choice: the writer has chosen, and history is a
        // convenience beside the file.
        if (!gone) keepInHistoryNow(document->m_id, disk.originalBytes, History::Reason::Overwrite);
        document->m_problem = Document::Problem::None;
        document->m_fileMissing = false;
        document->m_fileUnreadable = false;
        document->m_leadsTo.clear();
        document->m_nameTaken = false;
        document->m_lastError.clear();
        // A draft whose file is gone makes its folder and file afresh, and a
        // named one its file, at that path: the save's worker never follows
        // a link put there since, and leaves it, and anything else there,
        // as it is, saying so.
        const quint32 mode = document->m_diskVersion.mode;
        document->m_diskVersion = !gone                ? disk.version
                                  : document->isDraft() ? DiskVersion{}
                                                        : DiskVersion{path, 0, 0, {}, false, {}};
        // A file made again has the mode the document's file last had.
        if (gone && !document->isDraft()) document->m_diskVersion.mode = mode;
        // The buffer may be unmodified, as when the file only vanished, and
        // must still be written.
        document->m_diskAhead = true;
        save(document);
        document->refresh();
        return true;
    }

    // Reload: the local text is recorded first, and stays in undo as well.
    // A failed capture does not block it: the text also stays in undo. A
    // document with no text of its own has none to record.
    if (!document->m_textless) keepInHistoryNow(document->m_id, local, History::Reason::Reload);
    takeFile(document, disk);
    return true;
}

void DocumentStore::readAgain(Document *document)
{
    if (m_documents.contains(document) && document->fileUnreadable()) checkDisk(document);
}

History *DocumentStore::history() const
{
    return m_history.get();
}

DocumentStore::Restore DocumentStore::restoreVersion(Document *document, const QString &versionId, QString *refusal)
{
    if (!m_documents.contains(document)) return Restore::Refused;
    // Every refusal gives its reason, and leaves it on the document for a
    // view to show, unless a failed save or a conflict is what the document
    // shows: that stays in view.
    auto refused = [&](const QString &reason) {
        if (refusal) *refusal = reason;
        if (document->m_problem == Document::Problem::None) {
            document->m_lastError = reason;
            document->refresh();
        }
        return Restore::Refused;
    };
    if (document->m_readOnly) return refused(tr("This document is read-only"));
    bool found = false;
    const QByteArray bytes = m_history->read(document->m_id, versionId, &found);
    if (!found) return refused(tr("That version is no longer in history"));
    // The version's bytes are what a save wrote, read as the file's are. The
    // document keeps its own line-ending convention and BOM.
    const DecodedText version = DiskFile::decode(bytes);
    if (!version.valid) return refused(tr("That version is not valid UTF-8 text"));
    const QString &text = version.text;
    const QString before = document->m_buffer->text();
    if (text == before && !document->m_textless) return Restore::Unchanged;
    // A document with no text of its own takes the version's, written as
    // the version was.
    if (document->m_textless) {
        document->m_buffer->replace({0, int(before.size())}, text, EditKind::Other);
        document->m_convention = version.convention;
        document->m_textless = false;
        document->refresh();
        return Restore::Restored;
    }
    // The text about to be replaced is recorded first. When that fails
    // because history could not write, the restore is refused and the
    // document is left as it is, since the text would otherwise exist only in
    // undo. When history is paused, by named versions or because the text does
    // not fit the size limit, the restore goes on, because ordinary work
    // continues while capture is paused.
    const QByteArray current = DiskFile::encode(before, document->m_convention);
    // The status comes from the capture itself: asking paused() afterwards
    // could meet another thread's capture and mistake a failure for a pause.
    // Text history's newest version holds already counts as kept, and one
    // that history did not take in time as not kept.
    const History::CaptureStatus status = keepInHistoryNow(document->m_id, current, History::Reason::Restore);
    if (status == History::CaptureStatus::Failed)
        return refused(tr("The current text could not be saved to history, so nothing was restored"));
    document->m_buffer->replace({0, int(before.size())}, text, EditKind::Other);
    if (document->m_problem == Document::Problem::None) document->m_lastError.clear();
    document->refresh();
    return Restore::Restored;
}

void DocumentStore::waitForSaves()
{
    while (!m_running.isEmpty()) {
        m_running.first()->waitForFinished();
        // The watcher reports through the event loop, which applies the outcome.
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }
    m_registryWriter.waitForDone();
}

bool DocumentStore::waitForSaves(int timeoutMs)
{
    const QDeadlineTimer deadline(timeoutMs);
    // Each round waits for the first save still running, or the deadline. A
    // save that ends applies its outcome first, which can start the next.
    while (!m_running.isEmpty() && !deadline.hasExpired()) {
        QEventLoop loop;
        connect(m_running.first(), &QFutureWatcherBase::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
        QTimer::singleShot(std::max<qint64>(0, deadline.remainingTime()), &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    return m_running.isEmpty() && m_registryWriter.waitForDone(int(std::max<qint64>(0, deadline.remainingTime())));
}

bool DocumentStore::savesRunning() const
{
    return !m_running.isEmpty();
}

void DocumentStore::retain(Document *document)
{
    if (!m_documents.contains(document)) return;
    ++m_views[document];
    m_unviewed.remove(document);
    m_dropping.remove(document);
}

void DocumentStore::release(Document *document)
{
    const auto views = m_views.find(document);
    if (views == m_views.end()) return;
    if (--*views > 0) return;
    m_views.erase(views);
    m_unviewed.insert(document);
    closeIfUnused(document);
}

void DocumentStore::save(Document *document)
{
    if (!m_documents.contains(document)) return;
    // Text read from invalid UTF-8 cannot be written back unchanged, and a
    // conflict waits for a choice, so neither is ever saved over.
    if (document->m_readOnly || document->m_problem == Document::Problem::Conflict) return;
    if (document->m_saving) {
        document->m_saveAgain = true;
        return;
    }
    if (!document->m_buffer->modified() && !document->m_diskAhead) {
        // The file already holds this text, so there is nothing to write and
        // no earlier failure left to show. An untouched draft gets no file.
        if (document->m_problem != Document::Problem::None) {
            document->m_problem = Document::Problem::None;
            document->m_lastError.clear();
            document->refresh();
        }
        return;
    }
    const auto snapshot = document->m_buffer->snapshot();
    SaveJob job;
    job.bytes = DiskFile::encode(snapshot->text, document->m_convention);
    job.revision = snapshot->revision;
    job.expected = document->m_diskVersion;
    // A draft's saves are noted too, for the revision each wrote.
    job.notesSave = true;
    job.from = contentOf(document->m_diskVersion);
    job.keepsReplaced = document->m_diskUnkept && !document->isDraft();
    job.mode = document->m_diskVersion.mode;
    job.name = std::exchange(document->m_askedName, {});
    if (document->isDraft()) {
        job.target = draftFolder(document->m_id) + u'/' + draftName;
        job.savesDraft = true;
    } else {
        job.target = document->m_path;
    }
    run(document, job);
}

void DocumentStore::saveAs(Document *document, const QString &path)
{
    nameAs(document, path, std::nullopt);
}

void DocumentStore::saveAs(Document *document, const QString &path, const DiskVersion &expected)
{
    nameAs(document, path, expected);
}

void DocumentStore::nameAs(Document *document, const QString &path, const std::optional<DiskVersion> &expected)
{
    if (!m_documents.contains(document) || document->m_readOnly || path.isEmpty()) return;
    if (document->m_textless) {
        refuseChoice(document, textlessReason(document), nullptr);
        emit saveFinished(document, path, SaveStatus::Failed);
        return;
    }
    if (document->m_saving) {
        // A newer name replaces an older one, and covers any plain save
        // requested meanwhile. A waiting name claims nothing: it is checked
        // and claimed when it starts, so replacing it leaves no claim behind.
        document->m_pendingSaveAs = path;
        document->m_pendingExpected = expected;
        return;
    }
    // The file at the path is read on the save's worker, when the save
    // starts, and only its name is asked for here.
    const QString target = DiskFile::canonicalPath(path);
    // The document's own name, which leads to another file now, as through
    // a link put there: the text goes at the name, in place of the link,
    // and never through it.
    if (!document->isDraft() && target != document->m_path && path == document->m_path) {
        putHere(document, path);
        return;
    }
    if (!document->isDraft() && target == document->m_path) {
        // Naming a document in conflict after its own file was asked to
        // replace that file, which is what Overwrite does, or what saving
        // again does for a file that is gone.
        document->m_askedName = path;
        if (document->m_problem == Document::Problem::Conflict) resolveConflict(document, ConflictChoice::Overwrite);
        else save(document);
        // A save that started took the name; with none, the naming ends here.
        document->m_askedName.clear();
        if (!document->m_saving)
            emit saveFinished(document, path,
                              document->m_problem == Document::Problem::None       ? SaveStatus::Saved
                              : document->m_problem == Document::Problem::Conflict ? SaveStatus::Conflict
                                                                                   : SaveStatus::Failed);
        return;
    }
    // Another document open at the target, or already saving to it, keeps it.
    if (Document *other = documentAt(target); other && other != document) {
        refuse(document, tr("Another document is already at %1").arg(path));
        emit saveFinished(document, path, SaveStatus::Failed);
        return;
    }
    // A name inside the drafts folder would be removed with a draft, or be another draft's file.
    const QString drafts = canonicalDrafts();
    if (!drafts.isEmpty() && target.startsWith(drafts + u'/')) {
        refuse(document, tr("Choose a place outside parch's drafts folder"));
        emit saveFinished(document, path, SaveStatus::Failed);
        return;
    }
    const auto snapshot = document->m_buffer->snapshot();
    SaveJob job;
    job.target = path;
    job.bytes = DiskFile::encode(snapshot->text, document->m_convention);
    job.revision = snapshot->revision;
    // Without an expected version the file as it is when the save starts is
    // the one replaced; with one, a file that is no longer at it makes the
    // save fail.
    if (expected) job.expected = *expected;
    job.replacesWhatIsThere = !expected;
    job.claim = target;
    job.renames = true;
    job.name = path;
    job.notesSave = true;
    job.from = contentOf(document->m_diskVersion);
    if (document->isDraft()) job.draftFolder = draftFolder(document->m_id);
    else job.ownPath = document->m_path;
    job.mode = document->m_diskVersion.mode;
    m_claims.insert(job.claim, document);
    run(document, job);
}

void DocumentStore::putHere(Document *document, const QString &name)
{
    // Nothing there is replaced but a link, so the conflict ends here; a
    // put that fails says why, and one that finds something else at the
    // name waits on.
    document->m_problem = Document::Problem::None;
    document->m_leadsTo.clear();
    document->m_lastError.clear();
    const auto snapshot = document->m_buffer->snapshot();
    SaveJob job;
    job.target = document->m_path;
    job.bytes = DiskFile::encode(snapshot->text, document->m_convention);
    job.revision = snapshot->revision;
    job.putsHere = true;
    job.mode = document->m_diskVersion.mode;
    job.name = name;
    job.notesSave = true;
    job.from = contentOf(document->m_diskVersion);
    run(document, job);
}

QList<Document *> DocumentStore::documents() const
{
    return m_documents;
}

bool DocumentStore::isUnnamedDraft(const QString &documentId) const
{
    if (!AppPaths::isDocumentId(documentId)) return false;
    if (Document *live = documentWithId(documentId)) return live->isDraft();
    for (const QString &id : std::as_const(m_registry))
        if (id == documentId) return false;
    return true;
}

QString DocumentStore::draftFile(const AppPaths &paths, const QString &documentId)
{
    return paths.drafts() + u'/' + documentId + u'/' + draftName;
}

QString DocumentStore::fileOf(const Document *document) const
{
    return document->isDraft() ? draftFolder(document->m_id) + u'/' + draftName : document->m_path;
}

DocumentStore::SaveOutcome DocumentStore::writeJob(const SaveJob &job, History *history, const Recovery &recovery,
                                                   const QString &documentId)
{
    SaveOutcome outcome;
    // A naming of the document's own path, reached through a link to its
    // folder, puts the text at that name too, and whatever the name leads
    // to is never looked at.
    const bool putsHere = job.putsHere || (!job.ownPath.isEmpty() && DiskFile::literalPath(job.target) == job.ownPath);
    DiskVersion expected =
        job.replacesWhatIsThere && !putsHere ? DiskFile::fingerprint(job.target) : job.expected;
    // A draft's file that is not there, never written or gone with its
    // folder since, is made afresh with the newest text: nothing is there
    // to write over.
    struct stat state {};
    const bool draftGone = job.savesDraft && ::lstat(QFile::encodeName(job.target).constData(), &state) != 0
                           && errno == ENOENT;
    // A file parch makes again, a draft's own or a named one that went, is
    // made at that very path, never through a link, and only while nothing
    // is there; whatever is there is left alone.
    bool creates = false;
    if (putsHere) {
        // Nothing is made but the text's own file, at the name.
    } else if (job.savesDraft && (draftGone || expected.canonicalPath.isEmpty())) {
        // The folder is parch's own, so it is named by what it is for, not
        // where it is, and is for its owner alone, as the text in it is.
        QString reason;
        if (!DiskFile::makeFolders(PathText::folderOf(job.target), &reason, 0700)) {
            outcome.result.error = tr("The drafts folder can't be written (%1)").arg(reason);
            return outcome;
        }
        creates = true;
    } else if (!job.savesDraft && !job.renames && !expected.exists) {
        creates = true;
    }
    Placement placement;
    if (!job.draftFolder.isEmpty()) {
        const Placed placed = placeAssets(job.draftFolder + QStringLiteral("/assets"),
                                          PathText::folderOf(expected.canonicalPath), placement, outcome.result.error,
                                          outcome.assetsPlaced);
        if (placed == Placed::Failed) return outcome;
        if (placed == Placed::Left) outcome.assetsLeft = job.draftFolder + QStringLiteral("/assets");
    }
    // What the file held when it was opened or reloaded goes to history
    // before the save replaces it, if it is still exactly that. A capture
    // that fails costs only that version, never the save.
    if (job.keepsReplaced && expected.exists) {
        QByteArray bytes;
        if (DiskFile::readVersion(job.target, expected, bytes))
            history->captureUnlessNewest(documentId, bytes, History::Reason::Opened);
    }
    // Noted before the file changes, so that the file never holds this
    // content without parch knowing it wrote it. A note that cannot be
    // written costs only that knowledge, never the save.
    if (job.notesSave)
        recovery.noteSave(documentId, job.from, QCryptographicHash::hash(job.bytes, QCryptographicHash::Sha256),
                          job.revision);
    // A file written in place has its current text kept in history first,
    // so that a write cut short leaves it there to go back to, unless parch
    // wrote that text itself with its last save: the newer text is in the
    // buffer and in recovery, and history takes parch's saves at its interval.
    const DiskFile::KeepCurrent keep = [history, &recovery, &documentId](const QByteArray &current, QString *reason) {
        if (recovery.savedLast(documentId, QCryptographicHash::hash(current, QCryptographicHash::Sha256))) return true;
        History::CaptureStatus status = History::CaptureStatus::Failed;
        history->captureUnlessNewest(documentId, current, History::Reason::Save, &status);
        if (status == History::CaptureStatus::Recorded || status == History::CaptureStatus::Held) return true;
        *reason = status == History::CaptureStatus::Paused ? tr("Version history is paused")
                                                           : tr("Version history could not be written");
        return false;
    };
    if (putsHere) {
        outcome.result = DiskFile::putAt(job.ownPath.isEmpty() ? job.target : job.ownPath, job.bytes, job.mode);
        outcome.nameTaken = outcome.result.status == SaveStatus::Conflict;
        outcome.put = true;
    } else if (creates) {
        outcome.result = DiskFile::create(job.target, job.bytes, job.mode);
        // What took the name is said once the outcome is back, where folders are named.
        if (outcome.result.status == SaveStatus::Conflict && job.savesDraft)
            outcome.result.error = tr("A file is already where the draft goes");
        outcome.nameTaken = outcome.result.status == SaveStatus::Conflict && !job.savesDraft;
    } else {
        outcome.result = DiskFile::save(job.target, job.bytes, expected, keep);
    }
    if (outcome.result.status != SaveStatus::Saved) {
        placement.undo();
        return outcome;
    }
    // From here the file holds what the save wrote, not what it replaced.
    if (job.notesSave)
        recovery.noteLanded(documentId, QCryptographicHash::hash(job.bytes, QCryptographicHash::Sha256), job.revision);
    // The version is recorded here, on the worker that wrote the file, since
    // hashing and writing it can take long and the bytes are already in hand.
    // A failed capture is not a failed save. The first save since the file
    // was opened records its text whatever the interval, beside the text it
    // replaced.
    if (job.keepsReplaced) history->capture(documentId, job.bytes);
    else history->captureIfDue(documentId, job.bytes);
    // A named draft's folder goes once the save is applied. Its own file can
    // hold text other than what was named, as after a restart that showed an
    // older text: that text becomes a version first, or the folder stays.
    if (!job.draftFolder.isEmpty()) {
        QFile own(job.draftFolder + u'/' + draftName);
        if (own.exists()) {
            const QByteArray bytes = own.open(QIODevice::ReadOnly) ? own.readAll() : QByteArray();
            History::CaptureStatus status = History::CaptureStatus::Failed;
            if (own.isOpen() && own.error() == QFileDevice::NoError && bytes != job.bytes)
                history->captureUnlessNewest(documentId, bytes, History::Reason::Draft, &status);
            const bool kept = own.isOpen() && own.error() == QFileDevice::NoError
                              && (bytes == job.bytes || status == History::CaptureStatus::Recorded
                                  || status == History::CaptureStatus::Held);
            outcome.keepDraft = !kept;
            if (!kept) outcome.keptTitle = Document::draftTitle(QString::fromUtf8(bytes));
        }
    }
    return outcome;
}

Document *DocumentStore::adopt(Document *document)
{
    m_documents.append(document);
    watchPath(document->m_path);
    emit documentOpened(document);
    return document;
}

// A document handed to a caller is wanted again, so a save that finishes
// later must not close it under that caller.
Document *DocumentStore::handOut(Document *document)
{
    m_unviewed.remove(document);
    return document;
}

// The document that owns a file: the one open at it, or the one saving a new name to it.
Document *DocumentStore::documentAt(const QString &canonicalPath) const
{
    if (canonicalPath.isEmpty()) return nullptr;
    if (Document *claimant = m_claims.value(canonicalPath)) return claimant;
    for (Document *document : m_documents)
        if (document->m_path == canonicalPath) return document;
    return nullptr;
}

Document *DocumentStore::documentWithId(const QString &id) const
{
    for (Document *document : m_documents)
        if (document->m_id == id) return document;
    return nullptr;
}

QString DocumentStore::draftFolder(const QString &id) const
{
    return m_paths.drafts() + u'/' + id;
}

QString DocumentStore::canonicalDrafts() const
{
    return QFileInfo(m_paths.drafts()).canonicalFilePath();
}

QString DocumentStore::draftIdAt(const QString &canonicalPath) const
{
    const QString drafts = canonicalDrafts();
    if (drafts.isEmpty() || !canonicalPath.startsWith(drafts + u'/')) return {};
    const QStringList parts = canonicalPath.mid(drafts.size() + 1).split(u'/');
    if (parts.size() != 2 || parts.at(1) != draftName || !AppPaths::isDocumentId(parts.at(0))) return {};
    return parts.at(0);
}

void DocumentStore::run(Document *document, const SaveJob &job)
{
    document->m_saving = true;
    auto *watcher = new QFutureWatcher<SaveOutcome>(this);
    m_running.append(watcher);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, job, document, target = QPointer<Document>(document)] {
        m_running.removeOne(watcher);
        watcher->deleteLater();
        // The claim ends with the save, whatever its outcome and even if the
        // document is gone, and before any waiting name is tried.
        if (job.renames && m_claims.value(job.claim) == document) m_claims.remove(job.claim);
        if (target) finish(target, job, watcher->result());
    });
    watcher->setFuture(
        QtConcurrent::run(&DocumentStore::writeJob, job, m_history.get(), Recovery(m_paths), document->m_id));
    document->refresh();
}

void DocumentStore::finish(Document *document, const SaveJob &job, SaveOutcome outcome)
{
    SaveResult &result = outcome.result;
    document->m_saving = false;
    if (job.renames && result.status == SaveStatus::Saved) {
        // The claim makes this impossible, but should another document hold
        // the file anyway, this one does not take the name as well: two
        // documents must never write one file.
        Document *holder = documentAt(result.version.canonicalPath);
        if (holder && holder != document)
            result = SaveResult{SaveStatus::Failed, tr("Another document is already at %1").arg(job.target), {}};
    }
    const bool saved = result.status == SaveStatus::Saved;
    // A save at the document's own name, a naming that put the text there
    // included, and where it is.
    const bool ownName = !job.renames || outcome.put;
    const QString own = outcome.put && !job.ownPath.isEmpty() ? job.ownPath : job.target;
    // A save that met a conflict in the document's own file found content
    // parch did not write there, so its saves tell nothing now. One that
    // failed, or a naming that met a change at its new name, left the
    // document's own file as the saves before it wrote, and only its own
    // note goes.
    if (result.status == SaveStatus::Conflict && ownName) forgetSaves(document->m_id);
    else if (!saved && job.notesSave)
        Recovery(m_paths).dropSave(document->m_id, QCryptographicHash::hash(job.bytes, QCryptographicHash::Sha256),
                                   job.revision);
    if (saved) {
        document->m_problem = Document::Problem::None;
        document->m_lastError.clear();
        document->m_diskVersion = result.version;
        document->m_diskUnkept = false;
        if (job.renames) {
            const bool wasDraft = document->isDraft();
            if (!wasDraft) {
                m_registry.remove(document->m_path);
                m_watch.unwatch(document->m_path);
            }
            document->m_path = result.version.canonicalPath;
            watchPath(document->m_path);
            m_registry.insert(document->m_path, document->m_id);
            writeRegistry();
            // The draft goes last, once the named file and its identity are
            // written. One whose text nothing else keeps stays a draft, under
            // an id of its own, since its old one now names the file.
            // Assets that could not be placed stay where they are, in the
            // draft's folder, which keeps them and nothing else.
            if (wasDraft && !outcome.keepDraft && outcome.assetsLeft.isEmpty())
                removeNamedDraft(draftFolder(document->m_id), outcome.assetsPlaced);
            if (wasDraft && !outcome.keepDraft && !outcome.assetsLeft.isEmpty()) {
                QFile::remove(draftFolder(document->m_id) + u'/' + draftName);
                emit assetsKept(document, outcome.assetsLeft);
            }
            if (wasDraft && outcome.keepDraft) {
                const QString kept = newId();
                const bool moved = QDir().rename(draftFolder(document->m_id), draftFolder(kept));
                emit draftKept(document, moved ? kept : QString(), outcome.keptTitle);
            }
        }
        // Only a save of the current revision makes the buffer clean; an edit
        // made while it ran still needs saving.
        document->m_diskAhead = document->m_buffer->revision() != job.revision;
        if (!document->m_diskAhead) document->m_buffer->markSaved();
    } else if (result.status == SaveStatus::Conflict && ownName) {
        document->m_problem = Document::Problem::Conflict;
        document->m_fileMissing = !QFileInfo::exists(own);
        document->m_fileUnreadable = false;
        document->m_leadsTo.clear();
        document->m_nameTaken = outcome.nameTaken;
        document->m_lastError =
            outcome.nameTaken ? tr("Something else is now at “%1” in “%2”, so parch did not save over it.")
                                    .arg(PathText::fileName(own), shownFolder(own))
                              : result.error;
    } else {
        // A new name that could not be taken is a failed save: the document's own file is unchanged.
        document->m_problem = Document::Problem::Failed;
        document->m_lastError = result.error;
    }
    // A newer name covers any plain save requested meanwhile. After a failure
    // only an explicit retry saves again, so the failure stays visible.
    const QString pendingName = std::exchange(document->m_pendingSaveAs, {});
    const std::optional<DiskVersion> pendingExpected = std::exchange(document->m_pendingExpected, std::nullopt);
    const bool again = std::exchange(document->m_saveAgain, false);
    // Text given up while this save ran goes now, with nothing more saved.
    const bool dropping = m_dropping.remove(document) > 0;
    if (!dropping && !pendingName.isEmpty()) nameAs(document, pendingName, pendingExpected);
    else if (!dropping && again && saved) save(document);
    document->refresh();
    if (saved) emit documentSaved(document);
    emit saveFinished(document, job.name, result.status);
    if (dropping) {
        close(document);
        return;
    }
    // Events during the save were not compared, since the file was changing under our own hand.
    if (std::exchange(document->m_checkAgain, false)) checkDisk(document);
    closeIfUnused(document);
    askForView(document);
}

void DocumentStore::refuse(Document *document, const QString &error)
{
    document->m_problem = Document::Problem::Failed;
    document->m_lastError = error;
    document->refresh();
}

void DocumentStore::closeIfUnused(Document *document)
{
    // A document with no text of its own has nothing to keep it open.
    if (!m_unviewed.contains(document) || (document->state() != Document::State::Clean && !document->m_textless))
        return;
    close(document);
}

void DocumentStore::askForView(Document *document)
{
    if (document->m_textless) return;
    const Document::State state = document->state();
    if (m_unviewed.contains(document) && (state == Document::State::SaveFailed || state == Document::State::Conflict))
        emit needsView(document);
}

bool DocumentStore::drop(Document *document)
{
    if (!m_documents.contains(document) || m_views.contains(document)) return false;
    if (document->m_saving) {
        m_dropping.insert(document);
        return true;
    }
    close(document);
    return true;
}

void DocumentStore::close(Document *document)
{
    m_unviewed.remove(document);
    m_dropping.remove(document);
    m_documents.removeOne(document);
    if (!document->m_path.isEmpty()) m_watch.unwatch(document->m_path);
    // Its file holds its text, or the text was given up, so its saves no
    // longer need telling apart from another program's. One with no text of
    // its own leaves recovery as it is.
    if (!document->m_textless) forgetSaves(document->m_id);
    emit documentClosed(document->m_id);
    // Deleted later, because the view that let it go may still be using it.
    document->deleteLater();
}

void DocumentStore::watchPath(const QString &path)
{
    if (!path.isEmpty()) m_watch.watch(path);
}

DocumentStore::DiskCheck DocumentStore::checkJob(const QString &path, const DiskVersion &known)
{
    DiskCheck check;
    struct stat entry {};
    check.entry = ::lstat(QFile::encodeName(path).constData(), &entry) == 0;
    // A file that still stands as parch last read or wrote it, as after
    // parch's own save, is not read again.
    check.now = DiskFile::fingerprint(path, known);
    // A file with the content already known needs no reading.
    if (check.now.exists && !sameContentIdentity(check.now, known)) {
        check.loaded = DiskFile::load(path);
        // What was read is the file the text belongs to, even if it changed since the first look.
        if (check.loaded.ok) check.now = check.loaded.version;
    }
    return check;
}

void DocumentStore::checkDisk(Document *document)
{
    if (!document || !m_documents.contains(document) || document->isDraft()) return;
    // A save changes the file itself; whatever was seen meanwhile is looked at afterwards.
    if (document->m_saving || document->m_checking) {
        document->m_checkAgain = true;
        return;
    }
    document->m_checking = true;
    const DiskVersion known = document->m_diskVersion;
    auto *watcher = new QFutureWatcher<DiskCheck>(this);
    m_checks.append(watcher);
    connect(watcher, &QFutureWatcherBase::finished, this,
            [this, watcher, known, target = QPointer<Document>(document)] {
        m_checks.removeOne(watcher);
        watcher->deleteLater();
        // A document closed meanwhile may wait for deletion and must not be touched.
        if (!target || !m_documents.contains(target)) return;
        target->m_checking = false;
        // Something newer happened: a save moved the document's version, or
        // another event came in. The result is out of date, so look again.
        if (std::exchange(target->m_checkAgain, false) || target->m_saving || target->m_diskVersion != known) {
            checkDisk(target);
            return;
        }
        applyCheck(target, watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(&DocumentStore::checkJob, document->m_path, known));
}

void DocumentStore::applyCheck(Document *document, const DiskCheck &check)
{
    // Nothing is at the path, or something is that cannot be read. The file
    // is read only when it is not what the document last read.
    const bool missing = !check.now.exists || check.loaded.missing;
    const bool unreadable = !check.loaded.unreadable.isEmpty();
    // The path leads to another file than the document's own now, as when a
    // link was put in its place: that file is never taken or written over,
    // but the document waits until the writer chooses. A read-only document
    // has nothing to write, and keeps what it shows.
    const bool elsewhere = !document->isDraft() && check.now.exists && check.now.device != 0
                           && check.now.canonicalPath != document->m_path;
    if (elsewhere) {
        if (!document->m_readOnly
            && (document->m_problem != Document::Problem::Conflict || document->m_leadsTo != check.now.canonicalPath))
            enterElsewhere(document, check.now.canonicalPath);
        return;
    }
    if (document->m_problem == Document::Problem::Conflict && document->m_fileUnreadable && unreadable) {
        document->m_lastError = cannotRead(document->m_path, check.loaded);
        document->refresh();
        return;
    }
    // The document waited because its file could not be read, or its path
    // led to another file, and that has passed: what is there is looked at
    // as any change of the file is.
    bool readAgain = false;
    if (document->m_problem == Document::Problem::Conflict
        && (document->m_fileUnreadable || !document->m_leadsTo.isEmpty())) {
        document->m_problem = Document::Problem::None;
        document->m_fileUnreadable = false;
        document->m_leadsTo.clear();
        document->m_lastError.clear();
        readAgain = true;
    }
    if (document->m_problem == Document::Problem::Conflict) {
        // The choice waits and meets the file as it is then, but what is shown
        // follows the file: gone means the text may be the only copy, back
        // means the reason is no longer that it is gone, and a name that
        // something took and nothing has now is no longer taken.
        if ((!document->m_fileMissing && missing) || (document->m_nameTaken && !check.entry)) {
            document->m_fileMissing = true;
            document->m_nameTaken = false;
            document->m_lastError = tr("The file no longer exists");
            document->refresh();
        } else if (document->m_fileMissing && !missing && !unreadable && !document->differsFromFile()
                   && sameContentIdentity(check.now, document->m_diskVersion)) {
            // Back exactly as parch last read or wrote it, with nothing typed
            // since: there is nothing left to choose, and the text is its file's.
            document->m_problem = Document::Problem::None;
            document->m_fileMissing = false;
            document->m_nameTaken = false;
            document->m_lastError.clear();
            document->m_diskVersion = check.now;
            document->refresh();
        } else if (document->m_fileMissing && !missing) {
            document->m_fileMissing = false;
            document->m_nameTaken = false;
            document->m_lastError = tr("The file has changed since this text was last saved");
            document->refresh();
        }
        return;
    }
    // The file's own save, or a rewrite with the same content: nothing to
    // reload. A new inode is adopted so that the next save is not refused.
    if (sameContentIdentity(check.now, document->m_diskVersion)) {
        document->m_diskVersion = check.now;
        if (readAgain) {
            // The file is what the text was made on, so what the text has
            // that the file lacks is saved, as an edit's would be.
            document->refresh();
            if (document->m_buffer->modified() || document->m_diskAhead) save(document);
        }
        return;
    }
    if (document->m_readOnly) {
        // Nothing here can be unsaved, so a file that reads again is taken as it is.
        if (check.loaded.ok) takeFile(document, check.loaded);
        return;
    }
    const bool unsaved = document->m_buffer->modified() || document->m_diskAhead
        || document->m_problem == Document::Problem::Failed;
    if (missing) {
        enterConflict(document, tr("The file no longer exists"), true);
    } else if (unreadable) {
        // Neither shown nor saved over, since what it holds is not known.
        enterConflict(document, cannotRead(document->m_path, check.loaded), false, true);
    } else if (unsaved) {
        enterConflict(document, tr("The file has changed since this text was last saved"));
    } else if (!check.loaded.ok) {
        enterConflict(document, check.loaded.error);
    } else {
        takeFile(document, check.loaded);
    }
}

void DocumentStore::enterConflict(Document *document, const QString &error, bool missing, bool unreadable)
{
    forgetSaves(document->m_id);
    document->m_problem = Document::Problem::Conflict;
    document->m_fileMissing = missing;
    document->m_fileUnreadable = unreadable;
    document->m_leadsTo.clear();
    document->m_nameTaken = false;
    document->m_lastError = error;
    document->refresh();
    askForView(document);
}

void DocumentStore::enterElsewhere(Document *document, const QString &target)
{
    forgetSaves(document->m_id);
    document->m_problem = Document::Problem::Conflict;
    showElsewhere(document, target);
    askForView(document);
}

void DocumentStore::showElsewhere(Document *document, const QString &target)
{
    document->m_fileMissing = false;
    document->m_fileUnreadable = false;
    document->m_nameTaken = false;
    document->m_leadsTo = target;
    document->m_lastError = leadsElsewhere(document->m_path, target);
    document->refresh();
}

void DocumentStore::forgetSaves(const QString &documentId) const
{
    // No save of the document runs meanwhile, since every caller is on the
    // UI thread between saves, so a save noted later is never lost.
    Recovery(m_paths).forgetSaves(documentId);
}

bool DocumentStore::refuseChoice(Document *document, const QString &error, QString *refusal)
{
    document->m_lastError = error;
    document->refresh();
    if (refusal) *refusal = error;
    return false;
}

void DocumentStore::takeFile(Document *document, const LoadedFile &file)
{
    forgetSaves(document->m_id);
    const QString before = document->m_buffer->text();
    const QString &after = file.text;
    // The shared start and end are kept, so a view keeps its place around the change.
    const qsizetype limit = std::min(before.size(), after.size());
    qsizetype start = 0;
    while (start < limit && before.at(start) == after.at(start)) ++start;
    qsizetype tail = 0;
    while (tail < limit - start && before.at(before.size() - 1 - tail) == after.at(after.size() - 1 - tail)) ++tail;
    // An edit must not cut a character in either text, so the change grows to whole graphemes.
    while (start > 0 && !(sourceGraphemeBoundary(before, int(start)) && sourceGraphemeBoundary(after, int(start)))) --start;
    while (tail > 0
           && !(sourceGraphemeBoundary(before, int(before.size() - tail))
                && sourceGraphemeBoundary(after, int(after.size() - tail))))
        --tail;
    if (before != after)
        document->m_buffer->replace({int(start), int(before.size() - start - tail)},
                                    after.mid(start, after.size() - start - tail));
    document->m_buffer->markSaved();
    document->m_convention = file.convention;
    document->m_readOnly = file.readOnly;
    document->m_textless = false;
    document->m_diskVersion = file.version;
    document->m_diskAhead = false;
    document->m_diskUnkept = !document->isDraft() && file.version.exists;
    document->m_problem = Document::Problem::None;
    document->m_fileMissing = false;
    document->m_fileUnreadable = false;
    document->m_leadsTo.clear();
    document->m_nameTaken = false;
    document->m_lastError.clear();
    document->refresh();
}

QString DocumentStore::cannotRead(const QString &path, const LoadedFile &loaded) const
{
    if (loaded.folderUnreadable)
        return tr("The folder “%1” cannot be opened (%2)").arg(shownFolder(path), loaded.unreadable);
    return tr("“%1” in “%2” cannot be read (%3)").arg(PathText::fileName(path), shownFolder(path), loaded.unreadable);
}

QString DocumentStore::cannotOpen(const QString &path, const LoadedFile &loaded) const
{
    if (loaded.missing)
        return tr("“%1” in “%2” is no longer there").arg(PathText::fileName(path), shownFolder(path));
    if (loaded.notRegular)
        return tr("“%1” in “%2” is not a regular file, so it is not opened").arg(PathText::fileName(path), shownFolder(path));
    return cannotRead(path, loaded);
}

QString DocumentStore::textlessReason(const Document *document)
{
    // Only what can be done is offered: following a link that is there, or
    // taking the file that is at its name.
    if (!document->m_leadsTo.isEmpty())
        return tr("This document has no text of its own: restore a version from its history, or follow the link");
    if (document->m_fileMissing) return tr("This document has no text of its own: restore a version from its history");
    return tr("This document has no text of its own: restore a version from its history, or reload its file");
}

QString DocumentStore::shownFolder(const QString &path) const
{
    return m_folderNames ? m_folderNames(path) : PathText::fileName(PathText::folderOf(path));
}

void DocumentStore::setBeforePlacing(std::function<void(const QString &path)> hook)
{
    beforePlacing = std::move(hook);
}

void DocumentStore::setFolderNames(std::function<QString(const QString &path)> names)
{
    m_folderNames = std::move(names);
}

void DocumentStore::loadRegistry()
{
    // The registry names every document's identity, and so its history. One
    // that is there but cannot be read is moved aside, as a damaged one is,
    // so that the next write does not replace it.
    const QString path = m_paths.registryFile();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        m_registryDamaged = QFile::exists(path) && !DiskFile::moveAside(path);
        return;
    }
    QJsonParseError error{};
    const QJsonDocument read = QJsonDocument::fromJson(file.readAll(), &error);
    const bool readable =
        file.error() == QFileDevice::NoError && error.error == QJsonParseError::NoError && read.isObject();
    file.close();
    if (!readable) {
        m_registryDamaged = !DiskFile::moveAside(path);
        return;
    }
    const QJsonObject entries = read.object();
    for (auto entry = entries.begin(); entry != entries.end(); ++entry)
        if (entry.value().isString()) m_registry.insert(entry.key(), entry.value().toString());
}

void DocumentStore::pruneRegistry()
{
    if (m_registry.isEmpty()) return;
    // Looked at where a file system that stopped answering holds up nothing
    // but this look.
    QtConcurrent::run(DiskFile::looks(), [entries = m_registry, paths = m_paths] {
        QSet<QString> stale;
        for (auto entry = entries.cbegin(); entry != entries.cend(); ++entry) {
            // Only a file that is certainly not there: an error of any other
            // kind says nothing about it.
            struct stat found {};
            if (::lstat(QFile::encodeName(entry.key()).constData(), &found) == 0 || errno != ENOENT) continue;
            const QString &id = entry.value();
            if (QFileInfo::exists(paths.history() + u'/' + id)) continue;
            if (!QDir(paths.recovery()).entryList({id + QStringLiteral(".*")}, QDir::Files).isEmpty()) continue;
            stale.insert(entry.key());
        }
        return stale;
    }).then(this, [this, entries = m_registry](const QSet<QString> &stale) {
        bool pruned = false;
        for (const QString &path : stale) {
            // Taken up again meanwhile, by a document or by text held for it.
            const QString id = entries.value(path);
            if (m_registry.value(path) != id || documentWithId(id) || m_held.contains(id) || heldAt(path)) continue;
            m_registry.remove(path);
            pruned = true;
        }
        if (pruned) writeRegistry();
    });
}

void DocumentStore::writeRegistry()
{
    QJsonObject entries;
    for (auto entry = m_registry.cbegin(); entry != m_registry.cend(); ++entry) entries.insert(entry.key(), entry.value());
    m_registryWriter.start([this, entries] { writeRegistryFile(entries); });
}

void DocumentStore::writeRegistryFile(const QJsonObject &entries)
{
    // A damaged registry that could not be moved aside is tried again, and
    // never written over.
    if (m_registryDamaged) {
        m_registryDamaged = !DiskFile::moveAside(m_paths.registryFile());
        if (m_registryDamaged) return;
    }
    // A registry that cannot be written loses only identity across a restart,
    // never text, so the document opens anyway.
    QDir().mkpath(PathText::folderOf(m_paths.registryFile()));
    QSaveFile file(m_paths.registryFile());
    if (!file.open(QIODevice::WriteOnly)) return;
    file.write(QJsonDocument(entries).toJson());
    file.commit();
}

Document *DocumentStore::documentOf(const QString &canonicalPath) const
{
    if (canonicalPath.isEmpty()) return nullptr;
    for (Document *document : m_documents)
        if (document->m_path == canonicalPath) return document;
    return nullptr;
}

bool DocumentStore::canRelocate(const Document *document, QString *reason) const
{
    const auto refuse = [reason](const QString &why) {
        if (reason) *reason = why;
        return false;
    };
    if (!document || !m_documents.contains(const_cast<Document *>(document)) || document->isDraft())
        return refuse(tr("It is not a file of its own"));
    if (document->m_saving || document->m_resolving || !m_claims.key(const_cast<Document *>(document)).isEmpty())
        return refuse(tr("“%1” is being saved; try again in a moment").arg(document->title()));
    if (document->m_problem == Document::Problem::Conflict || document->m_textless || !document->m_leadsTo.isEmpty())
        return refuse(tr("“%1” waits for a choice about its file; make it first").arg(document->title()));
    return true;
}

void DocumentStore::relocate(Document *document, const QString &canonicalPath, const std::optional<DiskVersion> &version)
{
    if (!m_documents.contains(document) || document->isDraft() || canonicalPath.isEmpty()) return;
    m_registry.remove(document->m_path);
    m_watch.unwatch(document->m_path);
    document->m_path = canonicalPath;
    if (version) document->m_diskVersion = *version;
    else document->m_diskVersion.canonicalPath = canonicalPath;
    watchPath(canonicalPath);
    m_registry.insert(canonicalPath, document->m_id);
    writeRegistry();
    document->refresh();
}

QString DocumentStore::documentIdFor(const QString &canonicalPath)
{
    if (Document *open = documentOf(canonicalPath)) return open->m_id;
    QString id = m_registry.value(canonicalPath);
    if (id.isEmpty()) {
        id = newId();
        m_registry.insert(canonicalPath, id);
        writeRegistry();
    }
    return id;
}
