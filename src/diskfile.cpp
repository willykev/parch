#include "diskfile.h"

#include "saferead.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QScopeGuard>
#include <QStringDecoder>
#include <QThreadPool>
#include <atomic>
#include <fcntl.h>
#include <climits>
#include <limits>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>
#include <utility>

namespace {
// Makes a new or changed name in a folder last through a power cut: the
// file's own data is synced before it is named, and this syncs the folder
// that now holds the name. File systems that cannot sync a folder say so
// with EINVAL, and the name is then as safe as they make it anyway.
std::atomic<quint64> syncedFolders{0};

void syncFolder(int directory)
{
    if (directory < 0) return;
    ::fsync(directory);
    syncedFolders.fetch_add(1);
}

void syncFolderOf(const QByteArray &path)
{
    const qsizetype slash = path.lastIndexOf('/');
    const QByteArray folder = slash > 0 ? path.left(slash) : (slash == 0 ? QByteArray("/") : QByteArray("."));
    const int directory = ::open(folder.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return;
    syncFolder(directory);
    ::close(directory);
}

const QByteArray utf8Bom("\xEF\xBB\xBF", 3);

// Paths are never tidied as text. Removing ".." or "." lexically gives a
// different file than the kernel when a symlinked directory comes before it, so
// a relative path only gets the working directory prefixed.
QString untidiedAbsolute(const QString &path)
{
    if (path.startsWith('/')) return path;
    char buffer[PATH_MAX];
    if (!::getcwd(buffer, sizeof buffer)) return path;
    return QFile::decodeName(buffer) + '/' + path;
}

// Names the file that creating a missing path would produce, with every link
// resolved by the kernel. A dangling link is followed hop by hop: each target
// is joined to the link's real directory without tidying, and the directory
// part is resolved with realpath. The result is not itself a link, so the
// saving code never has to follow one as text. A failure leaves the
// system's error number in code.
bool resolveMissing(QString absolute, QString &resolved, QString &error, int &code)
{
    const auto fail = [&](int number, const QString &message) {
        code = number;
        error = message;
        return false;
    };
    for (int hops = 0; hops < 40; ++hops) {
        const int slash = absolute.lastIndexOf('/');
        const QString leaf = absolute.mid(slash + 1);
        if (slash < 0 || leaf.isEmpty() || leaf == QLatin1String(".") || leaf == QLatin1String(".."))
            return fail(EINVAL, QCoreApplication::translate("DiskFile", "Invalid file path"));
        char *real = ::realpath(QFile::encodeName(slash == 0 ? QStringLiteral("/") : absolute.left(slash)).constData(), nullptr);
        if (!real) {
            const int number = errno;
            return fail(number, QString::fromLocal8Bit(strerror(number)));
        }
        const QString parent = QFile::decodeName(real);
        ::free(real);
        const QString candidate = (parent == QLatin1String("/") ? QString() : parent) + '/' + leaf;
        struct stat state {};
        if (::lstat(QFile::encodeName(candidate).constData(), &state) != 0) {
            const int number = errno;
            if (number == ENOENT) {
                resolved = candidate;
                return true;
            }
            return fail(number, QString::fromLocal8Bit(strerror(number)));
        }
        if (!S_ISLNK(state.st_mode))
            return fail(EEXIST, QCoreApplication::translate("DiskFile", "The file appeared while it was being inspected"));
        char target[PATH_MAX];
        const ssize_t length = ::readlink(QFile::encodeName(candidate).constData(), target, sizeof target - 1);
        if (length < 0) {
            const int number = errno;
            return fail(number, QString::fromLocal8Bit(strerror(number)));
        }
        const QString text = QFile::decodeName(QByteArray(target, length));
        absolute = text.startsWith('/') ? text : (parent == QLatin1String("/") ? QString() : parent) + '/' + text;
    }
    return fail(ELOOP, QString::fromLocal8Bit(strerror(ELOOP)));
}

std::atomic<quint64> readsMade{0};
std::atomic<quint64> lookupsMade{0};
std::atomic<quint64> foldersResolved{0};

std::atomic<bool> wholeSecondTimes{false};

// The moment stamps are taken at, as a test gives it.
std::function<qint64()> givenClock;

// What a stamp is taken on: the clock file times are set from.
qint64 clockNow()
{
    if (givenClock) return givenClock();
    struct timespec now {};
    ::clock_gettime(CLOCK_REALTIME, &now);
    return qint64(now.tv_sec) * 1000000000 + now.tv_nsec;
}

// Stats the file, and stamps it with the moment just before, so that the
// stamp is never later than what it saw.
bool stamped(int descriptor, struct stat &state, FileStamp &stamp)
{
    const qint64 taken = clockNow();
    if (::fstat(descriptor, &state) != 0) return false;
    const bool whole = wholeSecondTimes.load();
    stamp = FileStamp{qint64(state.st_size),
                      qint64(state.st_mtim.tv_sec) * 1000000000 + (whole ? 0 : state.st_mtim.tv_nsec),
                      qint64(state.st_ctim.tv_sec) * 1000000000 + (whole ? 0 : state.st_ctim.tv_nsec), taken};
    return true;
}

// The failure probe gives for something that is no regular file, which no
// errno takes, and for a file of the system, such as one in /proc.
constexpr int notRegularFailure = -1;
constexpr int systemFileFailure = -2;

// Reads the target's identity and bytes. A file that exists but cannot be read
// is an error rather than a version, because a version that was never read
// could wrongly compare equal to another unreadable one. The name and the
// bytes come from the same opened file, so they cannot describe two files.
// A failure leaves the system's error number in failure, when given: ENOENT
// or ENOTDIR for a path that leads nowhere, anything else for a file that is
// there but cannot be read. A file that still stands as `known` did is that
// version, unless its bytes are asked for, and is not read.
bool probe(const QString &path, DiskVersion &version, QByteArray *bytes, QString &error, struct stat *found = nullptr,
           int *failure = nullptr, const DiskVersion *known = nullptr)
{
    int code = 0;
    const auto fail = [&](int number, const QString &message) {
        if (failure) *failure = number;
        error = message;
        return false;
    };
    // What is there is no regular file: told apart from an error of the system's.
    const auto notRegular = [&] {
        if (failure) *failure = notRegularFailure;
        error = QCoreApplication::translate("DiskFile", "Not a regular file");
        return false;
    };
    const QString absolute = untidiedAbsolute(path);
    // A FIFO blocks a plain open until a writer appears, which would freeze the
    // caller, so the descriptor is opened without blocking and checked afterwards.
    const int descriptor = ::open(QFile::encodeName(absolute).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    const int opening = errno;
    if (descriptor < 0 && opening == ENOENT) {
        // Missing, or a dangling link: either way the kernel decides where a
        // save would create the file. Link loops fail the open with ELOOP instead.
        QString resolved, reason;
        if (!resolveMissing(absolute, resolved, reason, code)) return fail(code, reason);
        version = DiskVersion{resolved, 0, 0, {}, false, {}};
        if (bytes) bytes->clear();
        return true;
    }
    if (descriptor < 0) return fail(opening, QString::fromLocal8Bit(strerror(opening)));
    QFile file;
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return fail(EIO, file.errorString());
    }
    // The stamp is taken before the bytes are read, so that it is never
    // newer than they are: a write that comes during the read moves the file
    // on from it, and the next look reads the file again.
    struct stat state {};
    FileStamp stamp;
    if (!stamped(file.handle(), state, stamp) || !S_ISREG(state.st_mode)) return notRegular();
    if (state.st_size > DiskFile::MaxLoadBytes)
        return fail(EFBIG, QCoreApplication::translate("DiskFile", "it is larger than the %1 MiB parch opens")
                               .arg(DiskFile::MaxLoadBytes >> 20));
    char name[PATH_MAX];
    const ssize_t nameLength = ::readlink(QFile::encodeName(QStringLiteral("/proc/self/fd/%1").arg(file.handle())).constData(),
                                          name, sizeof name - 1);
    if (nameLength < 0) {
        const int number = errno;
        return fail(number, QString::fromLocal8Bit(strerror(number)));
    }
    const QString canonical = QFile::decodeName(QByteArray(name, nameLength));
    // A file of /proc says it is empty and is not, and none is a document.
    if (SafeRead::isSystemPath(canonical)) return fail(systemFileFailure, SafeRead::systemPathReason(canonical));
    if (found) *found = state;
    if (!bytes && known && known->exists && known->stamp.trusted() && known->stamp == stamp
        && known->canonicalPath == canonical && known->device == quint64(state.st_dev)
        && known->inode == quint64(state.st_ino)) {
        version = *known;
        return true;
    }
    // Read in pieces up to the limit, whatever size the file said it had.
    QByteArray content;
    QString why;
    bool tooLarge = false;
    if (!SafeRead::readDescriptor(file.handle(), DiskFile::MaxLoadBytes, content,
                                  why, &tooLarge, qint64(state.st_size)))
        return tooLarge ? fail(EFBIG, QCoreApplication::translate("DiskFile", "it is larger than the %1 MiB parch opens")
                                          .arg(DiskFile::MaxLoadBytes >> 20))
                        : fail(EIO, why);
    ++readsMade;
    version = DiskVersion{canonical, quint64(state.st_dev), quint64(state.st_ino),
                          QCryptographicHash::hash(content, QCryptographicHash::Sha256), true, stamp};
    version.mode = quint32(state.st_mode & 0777);
    if (bytes) *bytes = content;
    return true;
}

// Whether a file that is there cannot be read because its folder cannot be
// opened, rather than the file itself.
bool folderStops(const QString &path, int failure)
{
    const QString absolute = untidiedAbsolute(path);
    const qsizetype slash = absolute.lastIndexOf(u'/');
    const QString folder = slash <= 0 ? QStringLiteral("/") : absolute.left(slash);
    return failure == EACCES && ::access(QFile::encodeName(folder).constData(), X_OK) != 0;
}

// A save renames over files of the effective user alone, unless a test names another.
std::atomic<qint64> savingUserOverride{-1};

uid_t savingUser()
{
    const qint64 user = savingUserOverride.load();
    return user < 0 ? ::geteuid() : uid_t(user);
}

QString systemError(int number)
{
    return QString::fromLocal8Bit(std::strerror(number));
}

// The names of a file's extended attributes, each ended by a NUL, or false
// with errno set. A list that grows between the two calls is asked for again.
bool attributeNames(int descriptor, QByteArray &names)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const ssize_t size = ::flistxattr(descriptor, nullptr, 0);
        if (size < 0) return false;
        names.resize(size);
        const ssize_t listed = size == 0 ? 0 : ::flistxattr(descriptor, names.data(), size_t(size));
        if (listed >= 0) {
            names.resize(listed);
            return true;
        }
        if (errno != ERANGE) return false;
    }
    errno = ERANGE;
    return false;
}

bool probeForSave(const QString &path, DiskVersion &version, SaveResult &result, const DiskVersion &known,
                  struct stat *state = nullptr)
{
    int failure = 0;
    if (probe(path, version, nullptr, result.error, state, &failure, &known)) return true;
    if (failure == EFBIG) {
        result.status = SaveStatus::Conflict;
        result.error = QCoreApplication::translate("DiskFile", "The file on disk is now too large to open");
    }
    return false;
}

SaveResult changedOnDisk()
{
    return SaveResult{SaveStatus::Conflict, QCoreApplication::translate("DiskFile", "The file changed on disk"), {}};
}

// Every byte, or false with errno set.
bool writeAll(int descriptor, const QByteArray &bytes)
{
    qsizetype written = 0;
    while (written < bytes.size()) {
        const ssize_t step = ::write(descriptor, bytes.constData() + written, size_t(bytes.size() - written));
        if (step < 0 && errno == EINTR) continue;
        if (step < 0) return false;
        if (step == 0) {
            errno = EIO;
            return false;
        }
        written += step;
    }
    return true;
}

QByteArray randomSuffix()
{
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    QByteArray suffix;
    for (int i = 0; i < 6; ++i) suffix += letters[QRandomGenerator::global()->bounded(int(sizeof letters - 1))];
    return suffix;
}

// What tests make of create(): a step before the new file takes its name,
// and a file system that cannot rename without replacing.
std::function<void(const QString &)> beforeNaming;
std::function<void(const QString &)> afterNaming;
std::atomic<bool> renameWithoutReplacing{true};
std::atomic<bool> linking{true};
// And a write into a file made at its name directly that fails.
std::function<int(const QString &)> directWriteFailure;

// The folder and the last part of a path, apart.
bool splitPath(const QString &path, QString &folder, QString &leaf)
{
    const QString absolute = untidiedAbsolute(path);
    const qsizetype slash = absolute.lastIndexOf(u'/');
    leaf = absolute.mid(slash + 1);
    folder = slash <= 0 ? QStringLiteral("/") : absolute.left(slash);
    return slash >= 0 && !leaf.isEmpty() && leaf != QLatin1String(".") && leaf != QLatin1String("..");
}

// A new file beside the target, under a name nothing else has, that takes
// the target's place on commit and is removed otherwise.
class Replacement {
public:
    explicit Replacement(const QByteArray &target) : m_target(target) {}
    Replacement(const Replacement &) = delete;
    Replacement &operator=(const Replacement &) = delete;
    ~Replacement() { discard(); }

    // False, with errno set, when no file could be made.
    bool open(mode_t mode)
    {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const QByteArray name = m_target + '.' + randomSuffix();
            // The umask applies, as to any file made.
            const int descriptor = ::open(name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
            if (descriptor >= 0) {
                m_descriptor = descriptor;
                m_name = name;
                return true;
            }
            if (errno != EEXIST) return false;
        }
        errno = EEXIST;
        return false;
    }
    int handle() const { return m_descriptor; }
    // False, with errno set, when the rename failed; the new file is then
    // removed. Once renamed, the file is the target's and stays open until
    // this goes, so that it can still be looked at.
    bool commit()
    {
        if (::rename(m_name.constData(), m_target.constData()) != 0) {
            const int failure = errno;
            discard();
            errno = failure;
            return false;
        }
        m_name.clear();
        syncFolderOf(m_target);
        return true;
    }
    void discard()
    {
        if (m_descriptor >= 0) ::close(std::exchange(m_descriptor, -1));
        if (!m_name.isEmpty()) ::unlink(std::exchange(m_name, {}).constData());
    }

private:
    QByteArray m_target;
    QByteArray m_name;
    int m_descriptor = -1;
};

std::atomic<int> attributeCopyFailure{0};

enum class Carried { All, Uncarried, Failed };

// Gives the new file what made the old one what it was, besides its bytes:
// its group, its extended attributes (the ACL among them, and security
// labels), and its mode, setgid included, which goes last since a change of
// group can clear it. Uncarried, when the group or any attribute cannot be
// carried over, for any reason, asks for the file to be written in place
// instead, where it keeps them all, being the same file.
Carried carryAttributes(int from, const struct stat &original, int to, QString &error)
{
    struct stat made {};
    if (::fstat(to, &made) != 0) {
        error = systemError(errno);
        return Carried::Failed;
    }
    if (made.st_gid != original.st_gid && ::fchown(to, uid_t(-1), original.st_gid) != 0) return Carried::Uncarried;
    QByteArray names;
    if (!attributeNames(from, names)) {
        // A file system without extended attributes has none to carry.
        if (errno != ENOTSUP) return Carried::Uncarried;
        names.clear();
    }
    for (qsizetype start = 0; start < names.size();) {
        const qsizetype end = names.indexOf('\0', start);
        const QByteArray name = names.mid(start, (end < 0 ? names.size() : end) - start);
        start = end < 0 ? names.size() : end + 1;
        if (name.isEmpty()) continue;
        const ssize_t size = ::fgetxattr(from, name.constData(), nullptr, 0);
        if (size < 0) return Carried::Uncarried;
        QByteArray value(size, Qt::Uninitialized);
        const ssize_t read = size == 0 ? 0 : ::fgetxattr(from, name.constData(), value.data(), size_t(size));
        if (read < 0) return Carried::Uncarried;
        value.resize(read);
        if (attributeCopyFailure.load() != 0
            || ::fsetxattr(to, name.constData(), value.constData(), size_t(value.size()), 0) != 0)
            return Carried::Uncarried;
    }
    if (::fchmod(to, original.st_mode & 07777) != 0) {
        error = QCoreApplication::translate("DiskFile", "Cannot keep the file's permissions");
        return Carried::Failed;
    }
    return Carried::All;
}

// Writes the bytes into the file itself, for a file that a new one could
// not stand in for. What it holds goes to keep first, and nothing is written
// unless keep has it, since a write cut short leaves the file torn.
SaveResult writeInPlace(const QString &path, const DiskVersion &expected, const QByteArray &bytes,
                        const DiskFile::KeepCurrent &keep)
{
    SaveResult result;
    const QByteArray name = QFile::encodeName(expected.canonicalPath);
    // The file is opened once to read and once to write, and is the expected
    // file both times, never another put at its name meanwhile.
    const auto sameFile = [&expected](int descriptor) {
        struct stat state {};
        return ::fstat(descriptor, &state) == 0 && S_ISREG(state.st_mode) && quint64(state.st_dev) == expected.device
               && quint64(state.st_ino) == expected.inode;
    };
    QFile reader;
    const int readable = ::open(name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (readable < 0 || !reader.open(readable, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        if (readable >= 0) ::close(readable);
        result.error = systemError(errno);
        return result;
    }
    if (!sameFile(reader.handle())) return changedOnDisk();
    QByteArray current;
    bool tooLarge = false;
    if (!SafeRead::readDescriptor(reader.handle(), DiskFile::MaxLoadBytes, current, result.error, &tooLarge)) {
        if (tooLarge) {
            result.status = SaveStatus::Conflict;
            result.error = QCoreApplication::translate("DiskFile", "The file on disk is now too large to open");
        }
        return result;
    }
    ++readsMade;
    reader.close();
    if (QCryptographicHash::hash(current, QCryptographicHash::Sha256) != expected.sha256) return changedOnDisk();
    QString reason;
    if (!keep || !keep(current, &reason)) {
        result.error = QCoreApplication::translate("DiskFile", "The file can only be written in place, and its "
                                                                "current text could not be kept in Version history "
                                                                "first (%1)")
                           .arg(reason.isEmpty() ? QCoreApplication::translate("DiskFile", "no history") : reason);
        return result;
    }

    DiskVersion latest;
    if (!probeForSave(path, latest, result, expected)) return result;
    if (latest != expected) return changedOnDisk();
    const int descriptor = ::open(name.constData(), O_WRONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        result.error = QCoreApplication::translate("DiskFile", "Cannot write the file (%1). Its earlier text is in "
                                                                "Version history.")
                           .arg(systemError(errno));
        return result;
    }
    const auto closing = qScopeGuard([descriptor] { ::close(descriptor); });
    if (!sameFile(descriptor)) return changedOnDisk();
    // The file is stamped as written, and again once it is on disk: a stamp
    // that has moved in between saw another write, and is not kept, so that
    // the next look reads the file.
    struct stat written {}, synced {};
    FileStamp writtenStamp, syncedStamp;
    if (!writeAll(descriptor, bytes) || ::ftruncate(descriptor, off_t(bytes.size())) != 0
        || !stamped(descriptor, written, writtenStamp) || ::fdatasync(descriptor) != 0
        || !stamped(descriptor, synced, syncedStamp)) {
        result.error = QCoreApplication::translate("DiskFile", "Cannot write the file (%1). Its earlier text is in "
                                                                "Version history.")
                           .arg(systemError(errno));
        return result;
    }
    const bool unwritten = synced.st_size == written.st_size && synced.st_mtim.tv_sec == written.st_mtim.tv_sec
                           && synced.st_mtim.tv_nsec == written.st_mtim.tv_nsec;
    result.status = SaveStatus::Saved;
    result.version = DiskVersion{expected.canonicalPath, quint64(synced.st_dev), quint64(synced.st_ino),
                                 QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), true,
                                 unwritten ? syncedStamp : writtenStamp};
    result.version.mode = quint32(synced.st_mode & 0777);
    return result;
}
} // namespace

namespace DiskFile {

bool ownedBySavingUser(uid_t owner)
{
    return owner == savingUser();
}

bool stampDescriptor(int descriptor, struct stat &state, FileStamp &stamp)
{
    return stamped(descriptor, state, stamp);
}

DiskVersion fingerprint(const QString &path, const DiskVersion &known)
{
    DiskVersion version;
    QString error;
    if (!probe(path, version, nullptr, error, nullptr, nullptr, &known)) {
        // An unreadable file gets an identity that matches nothing read earlier,
        // so a save against it is refused rather than allowed.
        version = DiskVersion{untidiedAbsolute(path), 0, 0, {}, true, {}};
    }
    return version;
}

LoadedFile load(const QString &path)
{
    LoadedFile loaded;
    QByteArray bytes;
    int failure = 0;
    if (!probe(path, loaded.version, &bytes, loaded.error, nullptr, &failure)) {
        loaded.missing = failure == ENOENT || failure == ENOTDIR;
        if (!loaded.missing) {
            loaded.unreadable = loaded.error;
            loaded.folderUnreadable = folderStops(path, failure);
            loaded.notRegular = failure == notRegularFailure;
        }
        return loaded;
    }
    if (!loaded.version.exists) {
        loaded.missing = true;
        loaded.error = QCoreApplication::translate("DiskFile", "File does not exist");
        return loaded;
    }
    loaded.originalBytes = bytes;
    DecodedText decoded = decode(bytes);
    loaded.text = std::move(decoded.text);
    loaded.convention = decoded.convention;
    loaded.readOnly = !decoded.valid;
    loaded.ok = true;
    return loaded;
}

QString canonicalPath(const QString &path)
{
    ++lookupsMade;
    const QString absolute = untidiedAbsolute(path);
    // A path descriptor needs no permission to read the file and never
    // blocks, whatever the file is.
    const int descriptor = ::open(QFile::encodeName(absolute).constData(), O_PATH | O_CLOEXEC);
    if (descriptor < 0) {
        QString resolved, error;
        int code = 0;
        return errno == ENOENT && resolveMissing(absolute, resolved, error, code) ? resolved : absolute;
    }
    char name[PATH_MAX];
    const ssize_t length =
        ::readlink(QFile::encodeName(QStringLiteral("/proc/self/fd/%1").arg(descriptor)).constData(), name, sizeof name - 1);
    ::close(descriptor);
    return length < 0 ? absolute : QFile::decodeName(QByteArray(name, length));
}

QString literalPath(const QString &path)
{
    QString folder, leaf;
    if (!splitPath(path, folder, leaf)) return untidiedAbsolute(path);
    ++foldersResolved;
    char *real = ::realpath(QFile::encodeName(folder).constData(), nullptr);
    if (!real) return untidiedAbsolute(path);
    const QString resolved = QFile::decodeName(real);
    ::free(real);
    return (resolved == QLatin1String("/") ? QString() : resolved) + u'/' + leaf;
}

QString tidied(const QString &path, bool *same)
{
    if (same)
        *same = !path.endsWith(u'/') && !path.endsWith(QLatin1String("/..")) && !path.contains(QLatin1String("/../"));
    return QDir::cleanPath(path);
}

QString cleanPath(const QString &path)
{
    bool same = false;
    const QString tidy = tidied(path, &same);
    if (tidy == path || same) return tidy;
    const QString literal = literalPath(path);
    return literalPath(tidy) == literal ? tidy : literal;
}

SaveResult create(const QString &path, const QByteArray &bytes, quint32 mode)
{
    SaveResult result;
    QString folder, leaf;
    if (!splitPath(path, folder, leaf)) {
        result.error = QCoreApplication::translate("DiskFile", "Invalid file path");
        return result;
    }
    // Only the folder is resolved: the name itself is never followed, and
    // every step below is taken in that one folder.
    const int directory = ::open(QFile::encodeName(folder).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        const int reason = errno;
        // The folder is what stops it, so it is named.
        result.error = QCoreApplication::translate("DiskFile", "parch cannot write in “%1” (%2)")
                           .arg(folder, systemError(reason));
        return result;
    }
    const auto closing = qScopeGuard([directory] { ::close(directory); });
    return createAt(directory, folder, leaf, bytes, mode);
}

SaveResult createAt(int directory, const QString &folder, const QString &leaf, const QByteArray &bytes, quint32 mode)
{
    SaveResult result;
    const auto failed = [&result](int number) {
        result.error = systemError(number);
        return result;
    };
    if (leaf.isEmpty() || leaf == u"." || leaf == u".." || leaf.contains(u'/') || leaf.contains(QChar::Null))
        return failed(EINVAL);
    const QByteArray name = QFile::encodeName(leaf);
    struct stat there {};
    if (::fstatat(directory, name.constData(), &there, AT_SYMLINK_NOFOLLOW) == 0) {
        result.status = SaveStatus::Conflict;
        result.error = QCoreApplication::translate("DiskFile", "Something is already at that name");
        return result;
    }
    if (errno != ENOENT) return failed(errno);
    QByteArray temporary;
    int descriptor = -1;
    for (int attempt = 0; attempt < 100 && descriptor < 0; ++attempt) {
        temporary = name + '.' + randomSuffix();
        // The umask applies, as to any file made.
        descriptor = ::openat(directory, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (descriptor < 0 && errno != EEXIST) break;
    }
    if (descriptor < 0) {
        const int failure = errno;
        result.error = systemError(failure);
        if (::faccessat(directory, ".", W_OK, AT_EACCESS) != 0)
            result.error = QCoreApplication::translate("DiskFile", "parch cannot write in “%1” (%2)")
                               .arg(folder, result.error);
        return result;
    }
    const auto removing = qScopeGuard([&] {
        ::close(descriptor);
        if (!temporary.isEmpty()) ::unlinkat(directory, temporary.constData(), 0);
    });
    // The mode of the file it stands for, when one is known.
    if (mode != 0 && ::fchmod(descriptor, mode_t(mode & 0777)) != 0) return failed(errno);
    if (!writeAll(descriptor, bytes)) return failed(errno);
    if (::fdatasync(descriptor) != 0) {
        result.error = QCoreApplication::translate("DiskFile", "Cannot write the file to disk");
        return result;
    }
    if (beforeNaming) beforeNaming(folder + u'/' + leaf);
    // The new file takes the name only while nothing has it. A file system
    // that cannot rename so gets a second link under the name, which fails
    // the same way, and loses the temporary one. One that cannot link
    // either has the file made under its name, again only while nothing
    // has it, and the text written into it there; a file left half written
    // is removed.
    int failure = 0;
    bool named = false;
    if (renameWithoutReplacing) {
        named = ::renameat2(directory, temporary.constData(), directory, name.constData(), RENAME_NOREPLACE) == 0;
        if (named) temporary.clear();
        else failure = errno;
    }
    int written = descriptor;
    if (!named && (!renameWithoutReplacing || failure == EINVAL || failure == ENOSYS || failure == EOPNOTSUPP)) {
        named = linking && ::linkat(directory, temporary.constData(), directory, name.constData(), 0) == 0;
        if (!named) failure = linking ? errno : EINVAL;
        if (!named && failure != EEXIST) {
            written = ::openat(directory, name.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0666);
            if (written >= 0 && mode != 0) ::fchmod(written, mode_t(mode & 0777));
            if (written < 0) {
                failure = errno;
            } else if (const int injected = directWriteFailure ? directWriteFailure(folder + u'/' + leaf) : 0;
                       injected != 0 || !writeAll(written, bytes) || ::fdatasync(written) != 0) {
                failure = injected != 0 ? injected : errno;
                // Only the file it made goes, while the name still holds
                // it: anything put at the name since stays.
                struct stat mine {}, there {};
                if (::fstat(written, &mine) == 0
                    && ::fstatat(directory, name.constData(), &there, AT_SYMLINK_NOFOLLOW) == 0
                    && mine.st_dev == there.st_dev && mine.st_ino == there.st_ino)
                    ::unlinkat(directory, name.constData(), 0);
                ::close(written);
            } else {
                named = true;
            }
        }
    }
    const auto closingWritten = qScopeGuard([&] {
        if (named && written != descriptor) ::close(written);
    });
    if (!named) {
        if (failure != EEXIST) return failed(failure);
        result.status = SaveStatus::Conflict;
        result.error = QCoreApplication::translate("DiskFile", "Something is already at that name");
        return result;
    }
    syncFolder(directory);
    struct stat made {};
    FileStamp stamp;
    if (!stamped(written, made, stamp)) return failed(errno);
    result.status = SaveStatus::Saved;
    result.version = DiskVersion{literalPath(folder + u'/' + leaf), quint64(made.st_dev), quint64(made.st_ino),
                                 QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), true, stamp};
    result.version.mode = quint32(made.st_mode & 0777);
    if (afterNaming) afterNaming(folder + u'/' + leaf);
    return result;
}

SaveResult putAt(const QString &path, const QByteArray &bytes, quint32 mode)
{
    SaveResult result;
    const auto failed = [&result](int number) {
        result.error = systemError(number);
        return result;
    };
    QString folder, leaf;
    if (!splitPath(path, folder, leaf)) {
        result.error = QCoreApplication::translate("DiskFile", "Invalid file path");
        return result;
    }
    // Only the folder is resolved: the name itself is never followed, and
    // every step below is taken in that one folder.
    const int directory = ::open(QFile::encodeName(folder).constData(), O_PATH | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        result.error = QCoreApplication::translate("DiskFile", "parch cannot write in “%1” (%2)")
                           .arg(folder, systemError(errno));
        return result;
    }
    const auto closing = qScopeGuard([directory] { ::close(directory); });
    const QByteArray name = QFile::encodeName(leaf);
    QByteArray temporary;
    int descriptor = -1;
    for (int attempt = 0; attempt < 100 && descriptor < 0; ++attempt) {
        temporary = name + '.' + randomSuffix();
        descriptor = ::openat(directory, temporary.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        if (descriptor < 0 && errno != EEXIST) break;
    }
    if (descriptor < 0) {
        const int failure = errno;
        result.error = systemError(failure);
        if (::faccessat(directory, ".", W_OK, AT_EACCESS) != 0)
            result.error = QCoreApplication::translate("DiskFile", "parch cannot write in “%1” (%2)")
                               .arg(folder, result.error);
        return result;
    }
    const auto removing = qScopeGuard([&] {
        ::close(descriptor);
        if (!temporary.isEmpty()) ::unlinkat(directory, temporary.constData(), 0);
    });
    // The mode of the file it stands for, when one is known.
    if (mode != 0 && ::fchmod(descriptor, mode_t(mode & 0777)) != 0) return failed(errno);
    if (!writeAll(descriptor, bytes)) return failed(errno);
    if (::fdatasync(descriptor) != 0) {
        result.error = QCoreApplication::translate("DiskFile", "Cannot write the file to disk");
        return result;
    }
    if (beforeNaming) beforeNaming(folder + u'/' + leaf);
    // Right before the name is taken, what has it is looked at without
    // following it: a link, or nothing, gives way to the new file; a folder,
    // or a file, is left as it is.
    struct stat there {};
    if (::fstatat(directory, name.constData(), &there, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISLNK(there.st_mode)) {
        result.status = SaveStatus::Conflict;
        result.error = QCoreApplication::translate("DiskFile", "Something is already at that name");
        return result;
    }
    if (::renameat(directory, temporary.constData(), directory, name.constData()) != 0) return failed(errno);
    temporary.clear();
    syncFolder(directory);
    struct stat made {};
    FileStamp stamp;
    if (!stamped(descriptor, made, stamp)) return failed(errno);
    result.status = SaveStatus::Saved;
    result.version = DiskVersion{literalPath(path), quint64(made.st_dev), quint64(made.st_ino),
                                 QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), true, stamp};
    result.version.mode = quint32(made.st_mode & 0777);
    if (afterNaming) afterNaming(folder + u'/' + leaf);
    return result;
}

void setBeforeNaming(std::function<void(const QString &path)> hook)
{
    beforeNaming = std::move(hook);
}

void setAfterNaming(std::function<void(const QString &path)> hook)
{
    afterNaming = std::move(hook);
}

void setRenameWithoutReplacing(bool supported)
{
    renameWithoutReplacing.store(supported);
}

void setClock(std::function<qint64()> clock)
{
    givenClock = std::move(clock);
}

void setLinking(bool supported)
{
    linking.store(supported);
}

void setDirectWriteFailure(std::function<int(const QString &path)> hook)
{
    directWriteFailure = std::move(hook);
}

quint64 folderSyncs()
{
    return syncedFolders.load();
}

bool readVersion(const QString &path, const DiskVersion &version, QByteArray &bytes)
{
    DiskVersion read;
    QString error;
    return probe(path, read, &bytes, error) && read == version;
}

QThreadPool *looks()
{
    // Made once and never deleted, so that nothing waits for a look still
    // running as the process ends.
    static QThreadPool *pool = [] {
        auto *made = new QThreadPool;
        made->setMaxThreadCount(2);
        return made;
    }();
    return pool;
}

quint64 canonicalLookups()
{
    return lookupsMade.load();
}

quint64 folderLookups()
{
    return foldersResolved.load();
}

quint64 contentReads()
{
    return readsMade.load();
}

void setWholeSecondTimes(bool whole)
{
    wholeSecondTimes.store(whole);
}

QString decodeUtf8(const QByteArray &bytes, bool *valid)
{
    // The bytes are complete, so an unfinished trailing sequence is an error
    // too. A U+FEFF at the start is kept rather than dropped, since a BOM
    // that marks the encoding is the convention's and is removed before this.
    QStringDecoder decoder(QStringDecoder::Utf8,
                           QStringDecoder::Flag::Stateless | QStringDecoder::Flag::ConvertInitialBom);
    QString text = decoder(bytes);
    if (valid) *valid = !decoder.hasError();
    return text;
}

DecodedText decode(const QByteArray &bytes)
{
    DecodedText decoded;
    decoded.convention.bom = bytes.startsWith(utf8Bom);
    const QByteArray body = decoded.convention.bom ? bytes.mid(utf8Bom.size()) : bytes;
    // Only the first BOM goes, so that a doubled one survives a save.
    decoded.text = decodeUtf8(body, &decoded.valid);
    qsizetype crlf = 0, bareLf = 0;
    for (qsizetype i = body.indexOf('\n'); i >= 0; i = body.indexOf('\n', i + 1)) {
        if (i > 0 && body.at(i - 1) == '\r') ++crlf;
        else ++bareLf;
    }
    // CRLF wins when its breaks are at least as many as bare LF breaks, so a tie keeps the Windows style.
    if (crlf > 0 && crlf >= bareLf) decoded.convention.newline = QStringLiteral("\r\n");
    return decoded;
}

QByteArray encode(const QString &text, const TextConvention &convention)
{
    QByteArray bytes = text.toUtf8();
    if (convention.bom) bytes.prepend(utf8Bom);
    return bytes;
}

SaveResult save(const QString &path, const QByteArray &bytes, const DiskVersion &expected, const KeepCurrent &keep)
{
    SaveResult result;
    DiskVersion initial;
    struct stat original {};
    if (!probeForSave(path, initial, result, expected, &original)) return result;
    if (initial != expected) return changedOnDisk();
    // A file with other names would lose them to a new file, and a file of
    // another account its owner, so such a file is written in place.
    if (initial.exists && (original.st_nlink > 1 || !ownedBySavingUser(original.st_uid)))
        return writeInPlace(path, initial, bytes, keep);
    // The canonical path is already resolved by the kernel and is not a link, so
    // a symlink path saves into its target and stays a link.
    const QByteArray target = QFile::encodeName(initial.canonicalPath);
    // A file its owner made read-only is not replaced either.
    if (initial.exists && ::faccessat(AT_FDCWD, target.constData(), W_OK, AT_EACCESS) != 0) {
        result.error = QCoreApplication::translate("DiskFile", "The file is read-only (%1)").arg(systemError(errno));
        return result;
    }

    // A new file is made as any other is; one that replaces a file is the
    // owner's alone until it has that file's attributes.
    Replacement output(target);
    if (!output.open(initial.exists ? 0600 : 0666)) {
        result.error = systemError(errno);
        // The new text is written beside the file and then takes its place,
        // so a folder that cannot be written in is what stops it: it is named.
        const QString folder = QFileInfo(initial.canonicalPath).absolutePath();
        if (::access(QFile::encodeName(folder).constData(), W_OK) != 0)
            result.error = QCoreApplication::translate("DiskFile", "parch cannot write in “%1” (%2)")
                               .arg(folder, result.error);
        return result;
    }
    if (!writeAll(output.handle(), bytes)) {
        result.error = systemError(errno);
        return result;
    }
    if (initial.exists) {
        // The old file is read for its attributes through a descriptor of
        // its own, which must be the file the version names.
        const int from = ::open(target.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        struct stat opened {};
        if (from < 0 || ::fstat(from, &opened) != 0 || opened.st_dev != original.st_dev
            || opened.st_ino != original.st_ino) {
            if (from >= 0) ::close(from);
            return changedOnDisk();
        }
        const Carried carried = carryAttributes(from, opened, output.handle(), result.error);
        ::close(from);
        if (carried == Carried::Uncarried) {
            output.discard();
            return writeInPlace(path, initial, bytes, keep);
        }
        if (carried == Carried::Failed) return result;
    }

    // The data is pushed to disk before the final check, and a failed sync
    // is a failed save, so a lost write is never reported as saved.
    if (::fdatasync(output.handle()) != 0) {
        result.error = QCoreApplication::translate("DiskFile", "Cannot write the file to disk");
        return result;
    }
    // The saved version is built from what was written rather than re-read
    // afterwards, so a program that writes right after the rename cannot become
    // the version parch believes it saved. The rename keeps the inode.
    struct stat written {};
    FileStamp writtenStamp;
    if (!stamped(output.handle(), written, writtenStamp)) {
        result.error = QCoreApplication::translate("DiskFile", "Cannot inspect the written file");
        return result;
    }

    // The target is checked again just before the rename so that a change made
    // while the temporary file was being written is not overwritten.
    // The first look's stamp is the newer, and may stand for the file where
    // the expected version's could not.
    DiskVersion latest;
    if (!probeForSave(path, latest, result, initial)) return result;
    if (latest != expected) return changedOnDisk();
    if (!output.commit()) {
        result.error = systemError(errno);
        return result;
    }
    // The rename moves the file's ctime. Seen through the file still open,
    // its stamp is what the next look meets, so that look need not read it
    // again, unless its bytes have changed since they were written.
    struct stat renamed {};
    FileStamp renamedStamp;
    const bool unwritten = stamped(output.handle(), renamed, renamedStamp) && renamed.st_size == written.st_size
                           && renamed.st_mtim.tv_sec == written.st_mtim.tv_sec
                           && renamed.st_mtim.tv_nsec == written.st_mtim.tv_nsec;
    result.status = SaveStatus::Saved;
    result.version = DiskVersion{initial.canonicalPath, quint64(written.st_dev), quint64(written.st_ino),
                                 QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), true,
                                 unwritten ? renamedStamp : writtenStamp};
    result.version.mode = quint32(written.st_mode & 0777);
    return result;
}

void failAttributeCopies(int error)
{
    attributeCopyFailure.store(error);
}

void setSavingUser(qint64 user)
{
    savingUserOverride.store(user);
}

bool moveAside(const QString &file)
{
    if (!QFile::exists(file)) return true;
    for (int copy = 1; copy < 100; ++copy) {
        const QString aside = file + (copy == 1 ? QStringLiteral(".bad") : QStringLiteral(".bad.%1").arg(copy));
        if (QFile::exists(aside)) continue;
        return QFile::rename(file, aside);
    }
    return false;
}

bool makeFolders(const QString &path, QString *reason, mode_t mode)
{
    // One folder at a time, so that a failure gives its own cause.
    QString made;
    for (const QString &part : path.split(u'/', Qt::SkipEmptyParts)) {
        made += u'/' + part;
        if (::mkdir(QFile::encodeName(made).constData(), mode) == 0) continue;
        const int failure = errno;
        if (failure == EEXIST && QFileInfo(made).isDir()) continue;
        if (reason) *reason = QString::fromLocal8Bit(std::strerror(failure == EEXIST ? ENOTDIR : failure));
        return false;
    }
    return true;
}

} // namespace DiskFile
