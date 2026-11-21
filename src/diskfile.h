#pragma once

#include <QByteArray>
#include <QString>
#include <functional>
#include <optional>
#include <sys/stat.h>

// What stat says of a file, in nanoseconds, enough to know that it has not
// been written since: every write moves its ctime, as does a rename into
// place, and nothing can set the ctime back.
struct FileStamp {
    qint64 size = -1;
    qint64 modified = 0;
    qint64 changed = 0;
    // When the stamp was taken, on the same clock, which is no part of what
    // it says of the file.
    qint64 taken = 0;
    bool operator==(const FileStamp &other) const
    {
        return size == other.size && modified == other.modified && changed == other.changed;
    }
    // Whether the stamp can stand for the file's bytes. A write in the same
    // tick of the file system's clock as the last one leaves the times as
    // they were, so, as git's "racily clean" rule has it, only a stamp taken
    // more than a tick after the file's times can: the tick is taken to be
    // 2 s for times in whole seconds, and 1 s for any other, since file
    // systems and kernels that keep finer times can still tick coarsely,
    // and a network file system's clock is not this one's.
    bool trusted() const
    {
        const qint64 second = 1000000000;
        const qint64 tick = modified % second == 0 && changed % second == 0 ? 2 * second : second;
        return size >= 0 && taken - modified > tick && taken - changed > tick;
    }
};

// Identity and content of a file at one moment. Two versions are equal only if
// the same file, with the same bytes, is at the same canonical path, so a
// replaced, edited or deleted file never matches the version parch last read.
struct DiskVersion {
    QString canonicalPath;
    quint64 device = 0;
    quint64 inode = 0;
    QByteArray sha256;
    bool exists = false;
    // How the file stood when the version was read or written. A file at the
    // same place, device and inode that still stands so is this version, and
    // is not read again. No part of what the version is: two versions of the
    // same bytes at the same file are equal whatever their stamps.
    FileStamp stamp;
    // The file's permission bits as they were seen, which a file parch makes
    // again in its place is given; 0 when they are not known. No part of
    // what the version is either.
    quint32 mode = 0;
    bool operator==(const DiskVersion &other) const
    {
        return canonicalPath == other.canonicalPath && device == other.device && inode == other.inode
               && sha256 == other.sha256 && exists == other.exists;
    }
};

// Whether two versions are the same file content at the same place, ignoring
// device and inode. Those are only stable while a system runs: after a restart
// a filesystem can number its devices or inodes differently, and an unchanged
// file must not then look changed. Within one session compare versions with ==.
inline bool sameContentIdentity(const DiskVersion &a, const DiskVersion &b)
{
    return a.canonicalPath == b.canonicalPath && a.exists == b.exists && a.sha256 == b.sha256;
}

// What a file's bytes say about how to write the text back unchanged.
struct TextConvention {
    bool bom = false;
    QString newline = QStringLiteral("\n");
};

// Text from a file's bytes, or from a copy of them such as a version.
struct DecodedText {
    QString text;
    TextConvention convention;
    // False for bytes that are not UTF-8, whose text then holds replacement
    // characters and cannot be written back unchanged.
    bool valid = false;
};

struct LoadedFile {
    bool ok = false;
    bool readOnly = false;
    QString error;
    // Not read because nothing is there: the path, or a folder on it, does
    // not exist.
    bool missing = false;
    // Not read although something is there: the system's reason, and
    // whether the file's folder, rather than the file, cannot be opened.
    // Empty and false otherwise.
    QString unreadable;
    bool folderUnreadable = false;
    // Something is there, but it is no regular file, such as a pipe.
    bool notRegular = false;
    QString text;
    QByteArray originalBytes;
    TextConvention convention;
    DiskVersion version;
};

enum class SaveStatus { Saved, Conflict, Failed, Cancelled };

struct SaveResult {
    SaveStatus status = SaveStatus::Failed;
    QString error;
    DiskVersion version;
};

class QThreadPool;

// Stateless file access. Checks and the final rename are not one atomic
// compare-and-swap: a program that writes in the instant between them can
// still be overwritten, but any change made earlier is always detected.
//
// A path the kernel resolves, links followed, to more than PATH_MAX - 1
// bytes (4,095 on Linux) is refused, as the kernel refuses it, with its
// error ("File name too long"): it is neither read nor written, and never
// cut short to fit.
namespace DiskFile {

// Whether a replacement may inherit this owner's access permissions.
bool ownedBySavingUser(uid_t owner);

// Take an identity and time stamp before comparing a file's contents.
bool stampDescriptor(int descriptor, struct stat &state, FileStamp &stamp);
// Keeps the bytes a file holds before a save writes over them in place, as
// history does, when they need keeping; false, with the reason, when they
// could not be kept.
using KeepCurrent = std::function<bool(const QByteArray &current, QString *reason)>;

// The file's version. A file that still stands as `known` did, at the same
// place, is that version without being read; any other is read and hashed.
DiskVersion fingerprint(const QString &path, const DiskVersion &known = {});
// The link at a chosen name and the regular file it leads to, if any.
struct NameLink {
    QString name;
    QString target;
    quint64 device = 0, inode = 0;
    bool regular = false;
    quint64 targetDevice = 0, targetInode = 0;
    bool operator==(const NameLink &) const = default;
};
std::optional<NameLink> nameLink(const QString &path);
// The path with its folder resolved by the kernel and its last part as it
// is, never followed: where create() makes a file.
QString literalPath(const QString &path);
// A full path tidied as text alone, with no look at the file system: no "."
// or ".." part and no doubled "/". It is for looking a recorded path up,
// never for opening a file. `same`, when given, says whether the tidied
// path is sure to name the same place: it is unless a ".." was taken out,
// which names another place than the kernel's when a link comes before it,
// or the path ended in "/".
QString tidied(const QString &path, bool *same = nullptr);
// A full path as a launch gives it, spelled clean for looking it up: as
// given when it is spelled so already. Else tidied as text, which leaves a
// folder on it that a link replaced since unresolved, so that it is still
// seen, while that names the same place, as the kernel says of a path with
// ".." in it; else its literal path.
QString cleanPath(const QString &path);
// The kernel's name for the file at the path, every link resolved, found
// without reading the file: the name of the file there, or for a path with
// nothing there, the name creating it would give. For a path that leads
// nowhere it can be resolved, the path made absolute.
QString canonicalPath(const QString &path);
// Larger files are not read: load() refuses them as unreadable, saying
// why, so that one never fills memory or holds up the thread reading it.
constexpr qint64 MaxLoadBytes = qint64(64) << 20;
LoadedFile load(const QString &path, const QStringList *folders = nullptr);
// A pool for short looks at the file system made for the UI, as whether a
// folder or file is there, which a file system that stopped answering can
// hold up for ever: a caller waits for one only so long, if at all, and
// the pool is never waited for, not even as the process ends. Never the
// pool saves run on.
QThreadPool *looks();
// The bytes of the file at the path, while it is still the version given;
// false when it is not, or cannot be read.
bool readVersion(const QString &path, const DiskVersion &version, QByteArray &bytes);
// How many times the content of a file has been read and hashed since the
// process began, for tests and measures of how often a save reads.
quint64 contentReads();
// How many times a path's canonical name has been asked of the kernel since
// the process began, for tests of how often opening a file asks.
quint64 canonicalLookups();
// How many times literalPath() has asked the kernel to resolve a path's
// folder since the process began, for tests of how often opening a file asks.
quint64 folderLookups();
// For tests: file times are seen in whole seconds, as some file systems keep them.
void setWholeSecondTimes(bool whole);
// For tests: the moment a stamp is taken, in nanoseconds on the clock file
// times are set from; empty for that clock itself.
void setClock(std::function<qint64()> clock);
// Text from UTF-8 bytes, with every U+FEFF in them kept as text. valid, when
// given, is false for bytes that are not UTF-8, an unfinished sequence at the
// end among them.
QString decodeUtf8(const QByteArray &bytes, bool *valid = nullptr);
// Text from a file's bytes, or from a copy of them, as load() reads them: a
// BOM at the start is the convention's, any other U+FEFF is text, and the
// line ending is the one most breaks use, CRLF when as many use it as LF.
DecodedText decode(const QByteArray &bytes);
// The text already carries its line endings, so only the BOM and the UTF-8
// conversion are applied.
QByteArray encode(const QString &text, const TextConvention &convention);
// Writes a new file beside the target and renames it over the target, once
// the new file has the old one's group, mode (setgid included), ACL and
// other extended attributes. A file with more than one link, or whose
// owner, group or any attribute the new file cannot have, is written in
// place instead, so that it stays the same file: only once keep has taken
// the bytes it holds, since a write cut short leaves a torn file. Without
// keep, such a file is not saved. With an open directory, the expected
// file and its replacement are accessed relative to that directory.
SaveResult save(const QString &path, const QByteArray &bytes, const DiskVersion &expected,
                const KeepCurrent &keep = {}, int directory = -1);
// Makes a new file with the bytes at a name parch made up, or at a file it
// makes again: in the path's folder, resolved, under its last part taken as
// it is. It has the permission bits given, or else the default ones, less
// the umask; putAt() makes its file the same way. Anything already at that name, a link that leads nowhere included,
// leaves it as it is: Conflict, and nothing is written. The file is written
// beside it and takes the name only while nothing has it, so a name taken
// meanwhile fails the same way.
SaveResult create(const QString &path, const QByteArray &bytes, quint32 mode = 0);
// Create in the already-open directory. folder gives its name for messages and the returned version.
SaveResult createAt(int directory, const QString &folder, const QString &leaf, const QByteArray &bytes, quint32 mode = 0);
// Puts the bytes at the path's very name, in its folder resolved, as a new
// file written beside it that then takes the name, in place of a link
// there, which is never followed: the file a link led to is never opened.
// Anything there but a link, a folder or a file, is left as it is:
// Conflict, and nothing is written.
SaveResult putAt(const QString &path, const QByteArray &bytes, quint32 mode = 0, const NameLink *expectedLink = nullptr);
// Streams a regular file into a temporary sibling, then links it at a free name.
SaveResult copyAt(int directory, const QString &folder, const QString &leaf, int source, qint64 size, quint32 mode);

// Checks the published copy by identity and content before deleting either file.
bool copyStillAt(int directory, const QByteArray &leaf, const DiskVersion &expected);

// For tests: called with the path just before create() gives the new file
// its name; and whether the file system is taken to rename without
// replacing, or else to link and unlink, as on one that cannot.
void setBeforeNaming(std::function<void(const QString &path)> hook);
// For tests: called with the path once create() has given the new file its name.
void setAfterNaming(std::function<void(const QString &path)> hook);
void setRenameWithoutReplacing(bool supported);
// For tests: whether the file system is taken to make hard links.
void setLinking(bool supported);
// For tests: called with the path once create() has made the file at its
// name directly, where neither a rename that does not replace nor a link
// works; an error number it returns fails the write as the system would.
void setDirectWriteFailure(std::function<int(const QString &path)> hook);
// How many times a folder has been synced after a name was put in it, for
// tests that every way of naming a file makes the name last.
quint64 folderSyncs();
// The account whose files a save can replace by renaming, since only its
// own files keep their owner: the effective user id. Tests name another,
// so that a file another account owns can be met without being root; a
// negative number goes back to the effective user id.
void setSavingUser(qint64 user);
// For tests: every copy of an extended attribute to a new file fails with
// this error number, as a security module or a network file system can make
// one fail; 0 copies them again.
void failAttributeCopies(int error);
// Moves a file of parch's own that cannot be read out of the way, to
// <name>.bad, or .bad.2 and on beside earlier ones, so that it is never
// written over. True once nothing is left at the name.
bool moveAside(const QString &file);
// Makes a folder of parch's own and the folders above it that are missing,
// each with the mode given, less the umask. False, with the system's
// reason, when one cannot be made.
bool makeFolders(const QString &path, QString *reason = nullptr, mode_t mode = 0777);
}
