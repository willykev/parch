#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

// Reading a file that a document names, or that is in a library: the
// document itself, an included file, a bibliography, a citation style or
// locale, a picture, a file a folder search reads. Any of them can come
// from someone else, and a name can lead anywhere: to a device such as
// /dev/zero, which never ends; to a FIFO, which blocks a plain open until a
// writer comes; or to /proc, whose files say they are empty and are not.
// So every such read:
//
//   - opens the file without blocking, and reads only a regular file;
//   - refuses anything whose real name, links resolved, is in /proc, /sys
//     or /dev;
//   - reads in pieces and stops at the limit for its kind, whatever size the
//     file says it has, so that it never holds more than that in memory.
//
// A refusal says why, in words that fit after "cannot be read: ". A path
// that starts with ":/" is one of parch's own resources, such as its guide
// and the files beside it, read from memory within the same limit.
namespace SafeRead {

// The most bytes read of each kind of file.
inline constexpr qint64 DocumentBytes = qint64(64) << 20;  // documents and included files
inline constexpr qint64 ReferenceBytes = qint64(32) << 20; // bibliographies, citation styles and locales
inline constexpr qint64 PictureBytes = qint64(64) << 20;   // pictures

struct Result {
    bool ok = false;
    QByteArray bytes;
    // The kernel's name for the file that was opened, every link resolved;
    // empty when nothing was opened.
    QString canonicalPath;
    // Nothing is there: the path, or a folder on it, does not exist.
    bool missing = false;
    // Larger than the limit, and so not read; `size` is what was seen of it.
    bool tooLarge = false;
    qint64 size = 0;
    // Why it was not read.
    QString error;
};

// Reads the file at the path, refusing it, with the reason, as above.
Result read(const QString &path, qint64 limit);
// Also checks the opened file is within the canonical folders before reading.
Result readWithin(const QString &path, qint64 limit, const QStringList &folders);
Result readPicture(const QString &path, const QStringList &folders);
// Checks a picture reference for diagnostics and links. Reads check the
// opened descriptor as well, before consuming any bytes.
enum class PictureProblem { Unreadable, OutsideFolders };
QString pictureRefusal(const QString &path, const QStringList &folders, PictureProblem *problem = nullptr);

// Reads at most `limit` bytes from an open descriptor, in pieces, from where
// it stands to its end. False, with the reason, when more is there than the
// limit (`tooLarge` set) or a read fails. The size the file says it has is
// only a hint for the first piece.
bool readDescriptor(int descriptor, qint64 limit, QByteArray &bytes, QString &error, bool *tooLarge = nullptr,
                    qint64 sizeHint = 0);

// The name of the opened file or directory, with every link resolved.
QString descriptorPath(int descriptor);

// Whether a canonical path is in /proc, /sys or /dev (or is one of them):
// the system's files, never a document or a picture.
bool isSystemPath(const QString &canonicalPath);
// The reason a file in one of them is not read.
QString systemPathReason(const QString &canonicalPath);
// The reason a file larger than the limit is not read.
QString tooLargeReason(qint64 limit);

} // namespace SafeRead
