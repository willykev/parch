#include "fileorganizer.h"

#include "diskfile.h"
#include "document.h"
#include "documentstore.h"
#include "foldersearch.h"
#include "history.h"
#include "librarywalk.h"
#include "pathtext.h"
#include "saferead.h"
#include "settings.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"
#include "tablegrids.h"
#include "textsearch.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QPointer>
#include <QStandardPaths>
#include <QtConcurrent>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// How many changes a review lists; the rest are counted.
constexpr int shownChanges = 500;
constexpr int contextUnits = 64;

QString systemError(int error)
{
    return QString::fromLocal8Bit(std::strerror(error));
}

bool entryAt(const QString &path, struct stat *state = nullptr)
{
    struct stat found {};
    const bool there = ::lstat(QFile::encodeName(path).constData(), &found) == 0;
    if (there && state) *state = found;
    return there;
}

bool sameFileState(const struct stat &a, const struct stat &b)
{
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
           && a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}

// Renames without replacing anything at the new name; 0, or the error.
int renameWithoutReplacing(const QString &from, const QString &to)
{
    const QByteArray source = QFile::encodeName(from), target = QFile::encodeName(to);
    if (::renameat2(AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), RENAME_NOREPLACE) == 0) return 0;
    const int error = errno;
    if (error != EINVAL && error != ENOSYS) return error;
    // A file system that cannot refuse to replace is asked first, so that
    // only something made in between could be replaced.
    if (entryAt(to)) return EEXIST;
    return ::rename(source.constData(), target.constData()) == 0 ? 0 : errno;
}

bool canEditReferences(const Document *document)
{
    return document && !document->isTextless() && document->state() != Document::State::ReadOnly
           && document->state() != Document::State::Conflict;
}

QByteArray textDigest(const QString &text)
{
    return QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256);
}
} // namespace

struct FileOrganizer::OpenRanges {
    QPointer<SourceBuffer> buffer;
    Revision revision = 0;
    QList<int> starts;
    QMetaObject::Connection connection;
    ~OpenRanges() { QObject::disconnect(connection); }
};

FileOrganizer::FileOrganizer(DocumentStore *store, Settings *settings, QObject *parent)
    : QObject(parent), m_store(store), m_settings(settings)
{
    m_pool.setMaxThreadCount(2);
}

FileOrganizer::~FileOrganizer()
{
    waitForWork();
}

bool FileOrganizer::busy() const { return m_busy > 0; }
QVariantMap FileOrganizer::trashing() const { return m_trashing; }
QVariantList FileOrganizer::folders() const { return m_folders; }

void FileOrganizer::waitForWork()
{
    if (m_cancel) m_cancel->store(true);
    m_pool.waitForDone();
}

void FileOrganizer::setBusy(int delta)
{
    const bool was = busy();
    m_busy += delta;
    if (busy() != was) emit busyChanged();
}

QString FileOrganizer::nameProblem(const QString &name) const
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) return tr("A name is needed");
    if (trimmed.contains(u'/')) return tr("A name cannot hold “/”");
    if (trimmed == u"." || trimmed == u"..") return tr("“%1” is not a name").arg(trimmed);
    if (trimmed.startsWith(u'.')) return tr("A name that starts with “.” would be hidden from the library");
    for (const QChar c : trimmed)
        if (c.unicode() < 0x20 || c.unicode() == 0x7f) return tr("A name cannot hold control characters");
    if (QFile::encodeName(trimmed).size() > 255) return tr("The name is too long");
    return {};
}

QString FileOrganizer::libraryOf(const QString &path) const
{
    QString outermost;
    for (const QString &folder : m_settings->libraryFolders())
        if ((path == folder || path.startsWith(folder + u'/')) && (outermost.isEmpty() || folder.size() < outermost.size()))
            outermost = folder;
    return outermost;
}

bool FileOrganizer::organizes(const QString &path) const
{
    if (path.isEmpty()) return false;
    for (const QString &folder : m_settings->libraryFolders())
        if (path == folder) return false;
    return !libraryOf(path).isEmpty();
}

QString FileOrganizer::shownName(const QString &path) const
{
    // A folder of the library by the library folder's own name, then the
    // folders below it.
    const QString library = libraryOf(path);
    if (library.isEmpty()) return path;
    QStringList parts{PathText::fileName(library)};
    parts += path.mid(library.size()).split(u'/', Qt::SkipEmptyParts);
    return parts.join(QStringLiteral(" / "));
}

QList<Document *> FileOrganizer::documentsUnder(const QString &canonical) const
{
    QList<Document *> found;
    if (canonical.isEmpty()) return found;
    for (Document *document : m_store->documents())
        if (!document->isDraft() && (document->path() == canonical || document->path().startsWith(canonical + u'/')))
            found.append(document);
    return found;
}

void FileOrganizer::newFile(const QString &folder, const QString &name)
{
    const QString why = nameProblem(name);
    if (!why.isEmpty()) {
        emit problem(tr("Not made: %1").arg(name.trimmed()), why);
        return;
    }
    if (libraryOf(folder).isEmpty()) {
        emit problem(tr("Not made: %1").arg(name.trimmed()), tr("New documents are made in the library's folders."));
        return;
    }
    QString file = name.trimmed();
    if (!LibraryWalk::isTextDocument(file)) file += QStringLiteral(".md");
    const QString path = folder + u'/' + file;
    const auto taken = [&] {
        emit problem(tr("Not made: “%1” already exists").arg(file),
                     tr("“%1” is already in “%2”, so nothing was made.").arg(file, shownName(folder)));
    };
    if (entryAt(path)) {
        taken();
        return;
    }
    const SaveResult made = DiskFile::create(path, {});
    if (made.status == SaveStatus::Conflict) {
        taken();
        return;
    }
    if (made.status != SaveStatus::Saved) {
        emit problem(tr("Not made: %1").arg(file),
                     tr("“%1” could not be made in “%2”: %3").arg(file, shownName(folder), made.error));
        return;
    }
    emit created(path);
}

void FileOrganizer::newFolder(const QString &folder, const QString &name)
{
    const QString why = nameProblem(name);
    const QString trimmed = name.trimmed();
    if (!why.isEmpty()) {
        emit problem(tr("Not made: %1").arg(trimmed), why);
        return;
    }
    if (libraryOf(folder).isEmpty()) {
        emit problem(tr("Not made: %1").arg(trimmed), tr("New folders are made in the library's folders."));
        return;
    }
    const QString path = folder + u'/' + trimmed;
    if (::mkdir(QFile::encodeName(path).constData(), 0777) == 0) return;
    const int error = errno;
    if (error == EEXIST)
        emit problem(tr("Not made: “%1” already exists").arg(trimmed),
                     tr("“%1” is already in “%2”, so nothing was made.").arg(trimmed, shownName(folder)));
    else
        emit problem(tr("Not made: %1").arg(trimmed),
                     tr("“%1” could not be made in “%2”: %3").arg(trimmed, shownName(folder), systemError(error)));
}

void FileOrganizer::rename(const QString &path, const QString &name)
{
    const QString old = PathText::fileName(path);
    const QString why = nameProblem(name);
    if (!why.isEmpty()) {
        emit problem(tr("Not renamed: %1").arg(old), why);
        return;
    }
    struct stat state {};
    if (!entryAt(path, &state)) {
        emit problem(tr("Not renamed: %1").arg(old), tr("“%1” can no longer be found.").arg(old));
        return;
    }
    QString renamed = name.trimmed();
    // A document keeps its kind, and so its place in the library.
    if (!S_ISDIR(state.st_mode) && LibraryWalk::isTextDocument(old) && !LibraryWalk::isTextDocument(renamed))
        renamed += u'.' + QFileInfo(old).suffix();
    const QString to = PathText::folderOf(path) + u'/' + renamed;
    if (to == path) return;
    startMove(path, to);
}

void FileOrganizer::move(const QString &path, const QString &folder)
{
    if (folder == path || folder.startsWith(path + u'/')) {
        emit problem(tr("Not moved: %1").arg(PathText::fileName(path)), tr("A folder cannot go inside itself."));
        return;
    }
    if (PathText::folderOf(path) == folder) return;
    startMove(path, folder + u'/' + PathText::fileName(path));
}

void FileOrganizer::startMove(const QString &from, const QString &to)
{
    const QString name = PathText::fileName(from);
    const bool renaming = PathText::folderOf(from) == PathText::folderOf(to);
    const auto refuse = [&](const QString &detail) {
        emit problem(renaming ? tr("Not renamed: %1").arg(name) : tr("Not moved: %1").arg(name), detail);
    };
    if (!organizes(from)) {
        refuse(tr("Only what is inside the library's folders is renamed or moved here; a library folder itself is "
                  "removed from the library instead."));
        return;
    }
    if (libraryOf(to).isEmpty()) {
        refuse(tr("It can only go to a folder of the library."));
        return;
    }
    if (m_review || busy()) {
        refuse(tr("Another move is waiting; finish it first."));
        return;
    }
    struct stat state {};
    if (!entryAt(from, &state)) {
        refuse(errno == ENOENT || errno == ENOTDIR ? tr("“%1” can no longer be found.").arg(name)
                                                   : tr("“%1” cannot be reached: %2").arg(name, systemError(errno)));
        return;
    }
    struct stat into {};
    if (::stat(QFile::encodeName(PathText::folderOf(to)).constData(), &into) != 0 || !S_ISDIR(into.st_mode)) {
        refuse(tr("“%1” is no longer there.").arg(shownName(PathText::folderOf(to))));
        return;
    }
    if (entryAt(to)) {
        refuse(tr("“%1” is already in “%2”, so nothing was moved.")
                   .arg(PathText::fileName(to), shownName(PathText::folderOf(to))));
        return;
    }
    // The open documents that go along must be free to, now as when the move is made.
    if (!S_ISLNK(state.st_mode)) {
        for (Document *document : documentsUnder(QFileInfo(from).canonicalFilePath())) {
            QString why;
            if (m_store->canRelocate(document, &why)) continue;
            // A save that runs, as autosave starts one, lands first.
            m_store->waitForSaves(2000);
            if (m_store->canRelocate(document, &why)) continue;
            refuse(why);
            return;
        }
    }
    Plan plan;
    plan.from = from;
    plan.to = to;
    plan.library = libraryOf(from);
    plan.destinationLibrary = libraryOf(to);
    plan.folder = S_ISDIR(state.st_mode);
    plan.documents = plan.folder ? 0 : 1;
    if (m_dialects) plan.dialects = m_dialects();
    // Only a document or a folder can be what a reference leads to here.
    if (!plan.folder && !LibraryWalk::isTextDocument(name)) {
        if (perform(plan)) finishMove(plan, false, {});
        return;
    }
    // Open documents are read as their text is now, by their file's identity.
    QHash<std::pair<quint64, quint64>, QString> open;
    for (Document *document : m_store->documents()) {
        if (document->isDraft() || document->isTextless()) continue;
        const DiskVersion version = document->diskVersion();
        if (version.exists) open.insert({version.device, version.inode}, document->buffer()->text());
    }
    setBusy(1);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    QtConcurrent::run(&m_pool, &FileOrganizer::planJob, plan, open, m_cancel).then(this, [this](const Plan &planned) {
        // The question, or the move, comes before the organizer says it is
        // no longer busy, so that a window waiting for it hears of it.
        if (planned.files.isEmpty()) {
            if (perform(planned)) finishMove(planned, false, {});
        } else {
            m_review = planned;
            emit reviewChanged();
        }
        setBusy(-1);
    });
}

FileOrganizer::Plan FileOrganizer::planJob(Plan plan, QHash<std::pair<quint64, quint64>, QString> open,
                                           std::shared_ptr<std::atomic<bool>> cancel)
{
    const QList<DocumentLinks::Move> moves{{plan.from, plan.to}};
    LibraryWalk::walk({plan.library}, [&](const LibraryWalk::Found &found) {
        if (plan.folder && found.path.startsWith(plan.from + u'/')) ++plan.documents;
        QString text;
        const auto opened = open.constFind({found.device, found.inode});
        if (opened != open.cend()) text = *opened;
        else if (!FolderSearch::readText(found.path, found.size, found.modified, &text)) return true;
        const md::Options options = plan.dialects ? plan.dialects(found.path) : md::Options();
        auto changes = DocumentLinks::changes(text, found.path, plan.library, moves, options, plan.destinationLibrary);
        if (!changes.isEmpty()) plan.files.append({found.path, std::move(changes)});
        return true;
    }, cancel.get());
    return plan;
}

QVariantMap FileOrganizer::review() const
{
    if (!m_review) return {};
    const Plan &plan = *m_review;
    const bool renaming = PathText::folderOf(plan.from) == PathText::folderOf(plan.to);
    int changes = 0;
    int unresolved = 0;
    QVariantList rows;
    for (const FilePlan &file : plan.files) {
        for (const DocumentLinks::Change &change : file.changes) {
            ++changes;
            if (!change.problem.isEmpty()) ++unresolved;
            if (rows.size() >= shownChanges) continue;
            rows.append(QVariantMap{{QStringLiteral("file"), PathText::fileName(file.path)},
                                    {QStringLiteral("folder"), shownName(PathText::folderOf(file.path))},
                                    {QStringLiteral("path"), file.path},
                                    {QStringLiteral("line"), change.line + 1},
                                    {QStringLiteral("before"), change.lineBefore},
                                    {QStringLiteral("after"), change.lineAfter}});
        }
    }
    const QString what = PathText::fileName(plan.from);
    const QString title = renaming ? tr("Rename “%1” to “%2”").arg(what, PathText::fileName(plan.to))
                                   : tr("Move “%1” to “%2”").arg(what, shownName(PathText::folderOf(plan.to)));
    const QString references = changes == 1 ? tr("1 link") : tr("%1 links").arg(changes);
    const QString documents = plan.files.size() == 1 ? tr("1 document") : tr("%1 documents").arg(plan.files.size());
    return {{QStringLiteral("title"), title},
            {QStringLiteral("detail"), unresolved > 0
                 ? tr("Review the affected references in %1. Some need attention after the move.").arg(documents)
                 : tr("%1 in %2 would no longer lead where they lead now. Update them as it moves, so that they do?")
                       .arg(references, documents)},
            {QStringLiteral("files"), int(plan.files.size())},
            {QStringLiteral("changes"), changes},
            {QStringLiteral("rows"), rows}};
}

void FileOrganizer::confirmMove(bool updateReferences)
{
    if (!m_review) return;
    const Plan plan = *std::exchange(m_review, std::nullopt);
    setBusy(1);
    const auto release = qScopeGuard([this] { setBusy(-1); });
    emit reviewChanged();
    if (!perform(plan)) return;
    if (!updateReferences) {
        finishMove(plan, false, {});
        return;
    }
    this->updateReferences(plan);
}

void FileOrganizer::cancelMove()
{
    if (!m_review) return;
    m_review.reset();
    emit reviewChanged();
}

bool FileOrganizer::perform(const Plan &plan, std::optional<DiskVersion> *copiedVersion)
{
    const QString name = PathText::fileName(plan.from);
    const bool renaming = PathText::folderOf(plan.from) == PathText::folderOf(plan.to);
    const auto refuse = [&](const QString &detail) {
        emit problem(renaming ? tr("Not renamed: %1").arg(name) : tr("Not moved: %1").arg(name), detail);
        return false;
    };
    // Anything can have changed while the references were read or reviewed.
    struct stat state {};
    if (!entryAt(plan.from, &state)) return refuse(tr("“%1” can no longer be found.").arg(name));
    if (entryAt(plan.to))
        return refuse(tr("“%1” is already in “%2”, so nothing was moved.")
                          .arg(PathText::fileName(plan.to), shownName(PathText::folderOf(plan.to))));
    const bool link = S_ISLNK(state.st_mode);
    const QString canonicalFrom = link ? QString() : QFileInfo(plan.from).canonicalFilePath();
    const QList<Document *> going = documentsUnder(canonicalFrom);
    for (Document *document : going) {
        QString why;
        if (m_store->canRelocate(document, &why)) continue;
        // A save that runs lands first, so that it writes to the file before it moves.
        m_store->waitForSaves(2000);
        if (!m_store->canRelocate(document, &why)) return refuse(why);
        // Saving can replace the file with a new inode at the same path.
        struct stat saved {};
        if (!entryAt(plan.from, &saved) || (saved.st_mode & S_IFMT) != (state.st_mode & S_IFMT))
            return refuse(tr("“%1” changed before it could be moved.").arg(name));
        state = saved;
    }
    int error = renameWithoutReplacing(plan.from, plan.to);
    std::optional<DiskVersion> copied;
    if (error == EXDEV && S_ISREG(state.st_mode)) {
        // Another file system: the file is copied there, and the original
        // goes only once the copy is in place.
        const int fd = ::open(QFile::encodeName(plan.from).constData(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            const int reason = errno;
            return refuse(tr("“%1” could not be read: %2").arg(name, systemError(reason)));
        }
        const auto close = qScopeGuard([fd] { ::close(fd); });
        struct stat opened {};
        if (::fstat(fd, &opened) || !sameFileState(state, opened))
            return refuse(tr("“%1” changed before it could be copied.").arg(name));
        QByteArray bytes;
        QString why;
        if (!SafeRead::readForCopy(fd, opened.st_size, bytes, why))
            return refuse(tr("“%1” could not be read in full: %2. The original was kept.").arg(name, why));
        const SaveResult made = DiskFile::create(plan.to, bytes, state.st_mode & 07777);
        if (made.status == SaveStatus::Conflict) error = EEXIST;
        else if (made.status != SaveStatus::Saved)
            return refuse(tr("“%1” could not be copied to “%2”: %3").arg(name, shownName(PathText::folderOf(plan.to)), made.error));
        else {
            if (DiskFile::fingerprint(plan.to, made.version) != made.version)
                return refuse(tr("The copy of “%1” changed. The original was kept.").arg(name));
            struct stat current {};
            const auto undoCopy = [&] {
                struct stat target {}, checked {}, source {};
                if (!entryAt(plan.to, &target) || !S_ISREG(target.st_mode) || quint64(target.st_dev) != made.version.device
                    || quint64(target.st_ino) != made.version.inode || DiskFile::fingerprint(plan.to, made.version) != made.version)
                    return false;
                return entryAt(plan.from, &source) && source.st_dev == opened.st_dev && source.st_ino == opened.st_ino
                       && entryAt(plan.to, &checked) && sameFileState(target, checked)
                       && ::unlink(QFile::encodeName(plan.to).constData()) == 0;
            };
            if (!entryAt(plan.from, &current) || !sameFileState(opened, current))
                return refuse(undoCopy() ? tr("“%1” changed during the copy. The move was cancelled.").arg(name)
                                         : tr("“%1” changed during the copy. The destination was kept.").arg(name));
            if (::unlink(QFile::encodeName(plan.from).constData()) != 0) {
                const int reason = errno;
                return refuse(undoCopy() ? tr("“%1” could not be moved: %2").arg(name, systemError(reason))
                                         : tr("“%1” was copied, but its original could not be removed: %2").arg(name, systemError(reason)));
            }
            error = 0;
            copied = made.version;
        }
    }
    switch (error) {
    case 0:
        break;
    case EXDEV:
        return refuse(tr("“%1” is a folder on another file system than “%2”, and folders are not moved between file "
                         "systems.")
                          .arg(name, shownName(PathText::folderOf(plan.to))));
    case EEXIST:
    case ENOTEMPTY:
        return refuse(tr("“%1” is already in “%2”, so nothing was moved.")
                          .arg(PathText::fileName(plan.to), shownName(PathText::folderOf(plan.to))));
    case EACCES:
    case EPERM:
    case EROFS:
        return refuse(tr("“%1” could not be moved, since a folder it leaves or goes to cannot be written: %2")
                          .arg(name, systemError(error)));
    default:
        return refuse(tr("“%1” could not be moved: %2").arg(name, systemError(error)));
    }
    // Open documents follow their files.
    const QString canonicalTo = link ? QString() : QFileInfo(plan.to).canonicalFilePath();
    for (Document *document : going) {
        const QString now = canonicalTo + document->path().mid(canonicalFrom.size());
        m_store->relocate(document, now, plan.folder ? std::nullopt : copied);
    }
    m_store->relocateClosedPaths(canonicalFrom, canonicalTo, plan.folder);
    for (Document *document : going) {
        emit documentMoved(document);
    }
    emit relocated(plan.from, plan.to);
    if (!canonicalFrom.isEmpty() && canonicalFrom != plan.from) emit relocated(canonicalFrom, canonicalTo);
    if (copiedVersion) *copiedVersion = copied;
    return true;
}

std::optional<FileOrganizer::Patched> FileOrganizer::inverseFor(const QString &path, const QString &documentId,
                                                                const QString &text, const QList<SourceEdit> &edits,
                                                                int references, QString *written)
{
    Patched patch{path, documentId, {}, {}, references, {}};
    int end = 0;
    for (const SourceEdit &edit : edits) {
        const SourceRange range = edit.range;
        if (range.start < end || !sourceGraphemeRange(text, range)) return std::nullopt;
        written->append(QStringView(text).sliced(end, range.start - end));
        const int start = int(written->size());
        written->append(edit.replacement);
        patch.inverses.append({{{start, int(edit.replacement.size())}, text.mid(range.start, range.length)},
                               edit.replacement, {}, {}});
        end = range.start + range.length;
    }
    written->append(QStringView(text).sliced(end));
    patch.digest = textDigest(*written);
    for (InverseEdit &inverse : patch.inverses) {
        const SourceRange range = inverse.edit.range;
        if (!sourceGraphemeRange(*written, range)) return std::nullopt;
        const int before = std::min(contextUnits, range.start);
        const int after = range.start + range.length;
        inverse.contextBefore = written->mid(range.start - before, before);
        inverse.contextAfter = written->mid(after, contextUnits);
        // Identical surrounding text cannot identify this occurrence once
        // the file has changed and there is no open buffer tracking it.
        const QString context = inverse.contextBefore + inverse.expected + inverse.contextAfter;
        inverse.uniqueContext = !context.isEmpty() && written->indexOf(context) == range.start - before
                                && written->indexOf(context, range.start - before + 1) < 0;
    }
    return patch;
}

std::optional<QList<SourceEdit>> FileOrganizer::inverseEdits(const Patched &patch, const QString &text, SourceBuffer *buffer)
{
    const bool unchanged = textDigest(text) == patch.digest;
    const bool tracked = buffer && patch.openRanges && patch.openRanges->buffer == buffer
                         && patch.openRanges->revision == buffer->revision();
    QList<SourceEdit> edits;
    int end = 0;
    for (qsizetype i = 0; i < patch.inverses.size(); ++i) {
        const InverseEdit &inverse = patch.inverses.at(i);
        SourceEdit edit = inverse.edit;
        if (!unchanged) {
            if (tracked) {
                edit.range.start = patch.openRanges->starts.at(i);
            } else {
                if (!inverse.uniqueContext) return std::nullopt;
                const QString context = inverse.contextBefore + inverse.expected + inverse.contextAfter;
                const qsizetype at = text.indexOf(context);
                if (at < 0 || text.indexOf(context, at + 1) >= 0) return std::nullopt;
                edit.range.start = int(at + inverse.contextBefore.size());
            }
        }
        if (edit.range.start < end || !sourceGraphemeRange(text, edit.range)
            || QStringView(text).sliced(edit.range.start, edit.range.length) != inverse.expected)
            return std::nullopt;
        end = edit.range.start + edit.range.length;
        if (!unchanged) {
            // A prefix or suffix added at the edge can change the reference
            // even when its old bytes are all still present.
            const auto boundary = [](QChar c) { return c.isNull() || c == u'\r' || c == u'\n'; };
            const auto matchesEdge = [&](QChar actual, QChar expected) {
                return actual == expected || (boundary(actual) && boundary(expected));
            };
            if (!matchesEdge(edit.range.start > 0 ? text[edit.range.start - 1] : QChar(),
                             inverse.contextBefore.isEmpty() ? QChar() : inverse.contextBefore.back())
                || !matchesEdge(end < text.size() ? text[end] : QChar(),
                                inverse.contextAfter.isEmpty() ? QChar() : inverse.contextAfter.front()))
                return std::nullopt;
        }
        edits.append(std::move(edit));
    }
    return edits;
}

void FileOrganizer::rewriteOpen(const StagedRewrite &staged, Document *open, Rewritten &done)
{
    if (!canEditReferences(open) || open->id() != staged.documentId || open->buffer() != staged.buffer
        || open->buffer()->revision() != staged.revision) {
        done.failed << staged.path;
        return;
    }
    SourceBuffer *buffer = open->buffer();
    QString written;
    auto patch = inverseFor(open->path(), open->id(), buffer->text(), staged.edits, staged.references, &written);
    if (!patch || buffer->replaceAtRevision(staged.revision, staged.edits) != SourceBuffer::EditOutcome::Applied) {
        done.failed << staged.path;
        return;
    }
    // Track the ranges while this buffer is open. Its other edits can be
    // right beside an update, and don't have to leave its context intact.
    if (buffer->text() == written) {
        auto tracked = std::make_shared<OpenRanges>();
        tracked->buffer = buffer;
        tracked->revision = buffer->revision();
        QList<int> lengths;
        for (const InverseEdit &inverse : std::as_const(patch->inverses)) {
            tracked->starts.append(inverse.edit.range.start);
            lengths.append(inverse.edit.range.length);
        }
        tracked->connection = QObject::connect(
            buffer, &SourceBuffer::sourceChanged, buffer,
            [ranges = tracked.get(), lengths](Revision revision, SourceRange range, const QString &replacement) {
                for (qsizetype i = 0; i < ranges->starts.size(); ++i) {
                    int &start = ranges->starts[i];
                    if (start < 0) continue;
                    if (range.start + range.length <= start) start += int(replacement.size()) - range.length;
                    else if (range.start < start + lengths.at(i)) start = -1;
                }
                ranges->revision = revision;
            });
        patch->openRanges = std::move(tracked);
    }
    done.changes += staged.references;
    done.patched.append(std::move(*patch));
}

void FileOrganizer::rewriteClosed(const StagedRewrite &staged, History *history, Rewritten &done)
{
    const LoadedFile &loaded = staged.loaded;
    QString written;
    auto patch = inverseFor(loaded.version.canonicalPath, staged.documentId, loaded.text, staged.edits,
                            staged.references, &written);
    if (!patch) {
        done.failed << staged.path;
        return;
    }
    // What the file held stays in its history, from where it can come back.
    History::CaptureStatus status = History::CaptureStatus::Failed;
    if (history) history->captureUnlessNewest(staged.documentId, loaded.originalBytes, History::Reason::Links, &status);
    if (status != History::CaptureStatus::Recorded && status != History::CaptureStatus::Held) {
        done.failed << staged.path;
        return;
    }
    const auto keep = [&](const QByteArray &current, QString *why) {
        if (current == loaded.originalBytes) return true;
        *why = tr("The file changed after its version was kept.");
        return false;
    };
    const SaveResult saved = DiskFile::save(staged.path, DiskFile::encode(written, loaded.convention), loaded.version, keep);
    if (saved.status != SaveStatus::Saved) {
        done.failed << staged.path;
        return;
    }
    done.changes += staged.references;
    done.patched.append(std::move(*patch));
}

QList<SourceEdit> FileOrganizer::referenceEdits(const QList<DocumentLinks::Change> &changes, const QString &path, Rewritten &done)
{
    QList<SourceEdit> edits;
    for (const DocumentLinks::Change &change : changes) {
        if (change.problem.isEmpty()) edits.append({change.range, change.after});
        else if (!done.failed.contains(path)) done.failed << path;
    }
    return edits;
}

void FileOrganizer::updateReferences(const Plan &plan)
{
    const QList<DocumentLinks::Move> moves{{plan.from, plan.to}};
    Rewritten done;
    QList<Rewrite> closed;
    for (const FilePlan &file : plan.files) {
        const QString now = DocumentLinks::moved(file.path, moves);
        const QString canonical = QFileInfo(now).canonicalFilePath();
        Document *open = m_store->documentOf(canonical);
        if (!open) {
            closed.append({now, file.path, m_store->documentIdFor(canonical.isEmpty() ? now : canonical)});
            continue;
        }
        if (!canEditReferences(open)) {
            done.failed << now;
            continue;
        }
        SourceBuffer *buffer = open->buffer();
        const QString text = buffer->text();
        const auto found = DocumentLinks::changes(text, file.path, plan.library, moves, open->markdownOptions(),
                                                  plan.destinationLibrary);
        QList<SourceEdit> edits = referenceEdits(found, now, done);
        const int references = int(edits.size());
        if (references == 0) continue;
        if (m_settings && m_settings->tableGrids() && open->markdownOptions().tables)
            edits = TableGrids::tidyCellEdits(text, edits, TableGrids::dialectOf(open->markdownOptions()));
        rewriteOpen({now, open->id(), references, edits, {}, buffer->revision(), buffer}, open, done);
    }
    if (closed.isEmpty()) {
        finishMove(plan, true, std::move(done));
        return;
    }
    setBusy(1);
    QtConcurrent::run(&m_pool, &FileOrganizer::rewriteJob, closed, plan, m_store->history())
        .then(this, [this, plan, done = std::move(done)](const Rewritten &rewritten) mutable {
            done.changes += rewritten.changes;
            done.failed += rewritten.failed;
            done.patched += rewritten.patched;
            finishMove(plan, true, std::move(done));
            setBusy(-1);
        });
}

FileOrganizer::Rewritten FileOrganizer::rewriteJob(QList<Rewrite> files, Plan plan, History *history)
{
    const QList<DocumentLinks::Move> moves{{plan.from, plan.to}};
    Rewritten done;
    for (const Rewrite &file : files) {
        LoadedFile loaded = DiskFile::load(file.path);
        if (!loaded.ok || loaded.readOnly) {
            done.failed << file.path;
            continue;
        }
        const md::Options options = plan.dialects ? plan.dialects(file.oldPath) : md::Options();
        const auto changes = DocumentLinks::changes(loaded.text, file.oldPath, plan.library, moves, options,
                                                    plan.destinationLibrary);
        const auto edits = referenceEdits(changes, file.path, done);
        if (edits.isEmpty()) continue;
        rewriteClosed({file.path, file.documentId, int(edits.size()), edits, std::move(loaded), 0, {}}, history, done);
    }
    return done;
}

void FileOrganizer::undoMove(quint64 move)
{
    if (!m_done || m_done->id != move) return;
    const Done taken = *std::exchange(m_done, std::nullopt);
    if (m_review || busy()) {
        emit problem(tr("Could not undo move"), tr("Another move is waiting; finish it first."));
        return;
    }
    Plan back = taken.plan;
    std::swap(back.from, back.to);
    std::swap(back.library, back.destinationLibrary);
    back.files.clear();

    QList<Patched> open, closed;
    QStringList failed;
    for (const Patched &patch : taken.updated) {
        if (m_store->documentOf(patch.path)) open.append(patch);
        else if (m_store->documentIdFor(patch.path) == patch.documentId) closed.append(patch);
        else failed << patch.path;
    }
    setBusy(1);
    if (closed.isEmpty()) {
        finishUndo(std::move(back), open, {}, std::move(failed));
        return;
    }
    using StagedFiles = std::pair<QList<StagedRewrite>, QStringList>;
    QtConcurrent::run(&m_pool, [closed, failed = std::move(failed)]() mutable {
        QList<StagedRewrite> staged;
        for (const Patched &patch : closed) {
            LoadedFile loaded = DiskFile::load(patch.path);
            auto edits = loaded.ok && !loaded.readOnly && loaded.version.canonicalPath == patch.path
                             ? inverseEdits(patch, loaded.text) : std::nullopt;
            if (!edits) {
                failed << patch.path;
                continue;
            }
            staged.append({patch.path, patch.documentId, patch.references, std::move(*edits), std::move(loaded), 0, {}});
        }
        return StagedFiles{std::move(staged), std::move(failed)};
    }).then(this, [this, back = std::move(back), open](StagedFiles staged) {
        finishUndo(back, open, std::move(staged.first), std::move(staged.second));
    });
}

void FileOrganizer::forgetMove(quint64 move)
{
    if (m_done && m_done->id == move) m_done.reset();
}

void FileOrganizer::finishUndo(Plan back, const QList<Patched> &open, QList<StagedRewrite> closed, QStringList failed)
{
    auto release = qScopeGuard([this] { setBusy(-1); });
    QList<StagedRewrite> buffers;
    for (const Patched &patch : open) {
        Document *document = m_store->documentOf(patch.path);
        if (!canEditReferences(document) || document->id() != patch.documentId) {
            failed << patch.path;
            continue;
        }
        SourceBuffer *buffer = document->buffer();
        auto edits = inverseEdits(patch, buffer->text(), buffer);
        if (!edits) {
            failed << patch.path;
            continue;
        }
        buffers.append({patch.path, patch.documentId, patch.references, std::move(*edits), {}, buffer->revision(), buffer});
    }
    for (const StagedRewrite &file : std::as_const(closed))
        if (m_store->documentOf(file.path)) failed << file.path;
    if (!failed.isEmpty()) {
        QStringList names;
        for (const QString &path : std::as_const(failed)) names << shownName(path);
        emit problem(tr("Could not undo move"),
                     tr("Undo could not verify the references in %1. The files are still in their current locations.")
                         .arg(names.join(QStringLiteral(", "))));
        return;
    }

    std::optional<DiskVersion> copied;
    if (!perform(back, &copied)) return;
    const QList<DocumentLinks::Move> moves{{back.from, back.to}};
    Rewritten done;
    for (StagedRewrite &file : buffers) {
        file.path = DocumentLinks::moved(file.path, moves);
        rewriteOpen(file, m_store->documentOf(file.path), done);
    }
    // A move changes where the staged version is expected. A copy between
    // file systems also has a new inode, which perform() reports itself.
    QList<StagedRewrite> prepared;
    for (StagedRewrite &file : closed) {
        const QString now = DocumentLinks::moved(file.path, moves);
        if (copied && file.path == back.from) {
            if (copied->sha256 != file.loaded.version.sha256) {
                done.failed << now;
                continue;
            }
            file.loaded.version = *copied;
        } else {
            file.loaded.version.canonicalPath = now;
        }
        file.path = now;
        prepared.append(std::move(file));
    }
    if (prepared.isEmpty()) {
        finishMove(back, true, std::move(done));
        return;
    }
    release.dismiss();
    QtConcurrent::run(&m_pool, [prepared, history = m_store->history(), done = std::move(done)]() mutable {
        for (const StagedRewrite &file : prepared) rewriteClosed(file, history, done);
        return done;
    }).then(this, [this, back = std::move(back)](Rewritten rewritten) {
        finishMove(back, true, std::move(rewritten));
        setBusy(-1);
    });
}

void FileOrganizer::finishMove(const Plan &plan, bool updated, Rewritten done)
{
    // It can be taken back, until another move is made.
    const qsizetype files = done.patched.size();
    const int changes = done.changes;
    Plan identity = plan;
    identity.files.clear();
    identity.dialects = {};
    m_done = Done{std::move(identity), std::move(done.patched), ++m_moves};
    const QString what = PathText::fileName(plan.from);
    const bool renaming = PathText::folderOf(plan.from) == PathText::folderOf(plan.to);
    const QString title = renaming ? tr("Renamed “%1” to “%2”").arg(what, PathText::fileName(plan.to))
                                   : tr("Moved “%1” to “%2”").arg(what, shownName(PathText::folderOf(plan.to)));
    QStringList detail;
    if (plan.folder)
        detail << (plan.documents == 0   ? tr("The folder holds no documents.")
                   : plan.documents == 1 ? tr("The document in it went with it.")
                                         : tr("The %1 documents in it went with it.").arg(plan.documents));
    int planned = 0;
    for (const FilePlan &file : plan.files) planned += int(file.changes.size());
    if (updated && changes > 0)
        detail << (changes == 1 ? tr("1 link was updated.")
                                : files == 1 ? tr("%1 links were updated in 1 document.").arg(changes)
                                             : tr("%1 links were updated in %2 documents.").arg(changes).arg(files));
    else if (!updated && planned > 0)
        detail << (planned == 1 ? tr("1 link to it, or from it, was left as it was and no longer leads there.")
                                : tr("%1 links to it, or from it, were left as they were and no longer lead there.")
                                      .arg(planned));
    if (!done.failed.isEmpty()) {
        done.failed.removeDuplicates();
        QStringList names;
        for (const QString &path : std::as_const(done.failed)) names << shownName(path);
        const QString affected = names.join(QStringLiteral(", "));
        detail << tr("References still need attention in %1.").arg(affected);
    }
    emit moved(title, detail.join(u' '), m_moves, !done.failed.isEmpty());
}

void FileOrganizer::askToTrash(const QString &path)
{
    const QString name = PathText::fileName(path);
    if (!organizes(path)) {
        emit problem(tr("Not moved to the trash: %1").arg(name),
                     tr("Only what is inside the library's folders goes to the trash from here; a library folder itself "
                        "is removed from the library instead."));
        return;
    }
    struct stat state {};
    if (!entryAt(path, &state)) {
        emit problem(tr("Not moved to the trash: %1").arg(name), tr("“%1” can no longer be found.").arg(name));
        return;
    }
    const bool folder = S_ISDIR(state.st_mode);
    const QString canonical = S_ISLNK(state.st_mode) ? QString() : QFileInfo(path).canonicalFilePath();
    const int open = int(documentsUnder(canonical).size());
    if (!folder) {
        m_trashing = {{QStringLiteral("path"), path}, {QStringLiteral("name"), name}, {QStringLiteral("folder"), false},
                      {QStringLiteral("documents"), 1}, {QStringLiteral("open"), open}};
        emit trashingChanged();
        return;
    }
    // What the folder holds is counted off the UI thread, so that the
    // question says what goes with it.
    setBusy(1);
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    QtConcurrent::run(&m_pool, [path, cancel = m_cancel] {
        int documents = 0;
        LibraryWalk::walk({path}, [&documents](const LibraryWalk::Found &) {
            ++documents;
            return true;
        }, cancel.get());
        return documents;
    }).then(this, [this, path, name, open](int documents) {
        m_trashing = {{QStringLiteral("path"), path}, {QStringLiteral("name"), name}, {QStringLiteral("folder"), true},
                      {QStringLiteral("documents"), documents}, {QStringLiteral("open"), open}};
        emit trashingChanged();
        setBusy(-1);
    });
}

void FileOrganizer::confirmTrash()
{
    const QVariantMap trashing = std::exchange(m_trashing, {});
    if (trashing.isEmpty()) return;
    emit trashingChanged();
    const QString path = trashing.value(QStringLiteral("path")).toString();
    const QString name = trashing.value(QStringLiteral("name")).toString();
    // The home trash is made in the data folder, which Qt does not make.
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    QFile file(path);
    if (!file.moveToTrash()) {
        emit problem(tr("Not moved to the trash: %1").arg(name),
                     tr("“%1” could not be moved to the trash: %2").arg(name, file.errorString()));
        return;
    }
    emit trashed(path);
    emit news(tr("Moved to the trash: %1").arg(name),
              trashing.value(QStringLiteral("open")).toInt() > 0
                  ? tr("It can be put back from the trash. What is open of it keeps its text in its tab until closed.")
                  : tr("It can be put back from the trash."));
}

void FileOrganizer::cancelTrash()
{
    if (m_trashing.isEmpty()) return;
    m_trashing.clear();
    emit trashingChanged();
}

void FileOrganizer::listFolders()
{
    const QStringList roots = m_settings->libraryFolders();
    setBusy(1);
    QtConcurrent::run(&m_pool, [roots] { return LibraryWalk::folders(roots, MaxFolders); })
        .then(this, [this](const QStringList &found) {
            setBusy(-1);
            QVariantList folders;
            for (const QString &path : found) {
                const QString library = libraryOf(path);
                const int depth = int(path.mid(library.size()).count(u'/'));
                folders.append(QVariantMap{{QStringLiteral("path"), path},
                                           {QStringLiteral("name"), PathText::fileName(path)},
                                           {QStringLiteral("label"), shownName(path)},
                                           {QStringLiteral("depth"), depth}});
            }
            m_folders = folders;
            emit foldersChanged();
        });
}
