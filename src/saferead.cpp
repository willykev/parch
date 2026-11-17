#include "saferead.h"

#include "mdinclude.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace SafeRead {
namespace {

constexpr qint64 Piece = qint64(64) << 10;

QString tr(const char *text)
{
    return QCoreApplication::translate("SafeRead", text);
}

QString sizeText(qint64 bytes)
{
    if (bytes >= (qint64(1) << 20) && bytes % (qint64(1) << 20) == 0)
        return QStringLiteral("%1 MiB").arg(bytes >> 20);
    if (bytes >= 1024 && bytes % 1024 == 0) return QStringLiteral("%1 KiB").arg(bytes >> 10);
    return QStringLiteral("%1 bytes").arg(bytes);
}

QString systemError(int number)
{
    return QString::fromLocal8Bit(std::strerror(number));
}

} // namespace

bool isSystemPath(const QString &canonicalPath)
{
    for (const QLatin1StringView root : {QLatin1StringView("/proc"), QLatin1StringView("/sys"), QLatin1StringView("/dev")})
        if (canonicalPath == root || (canonicalPath.startsWith(root) && canonicalPath.size() > root.size()
                                      && canonicalPath[root.size()] == u'/'))
            return true;
    return false;
}

QString systemPathReason(const QString &canonicalPath)
{
    const QString root = canonicalPath.section(u'/', 0, 1);
    return tr("it is a file of the system, in %1, which parch does not read").arg(root.isEmpty() ? canonicalPath : root);
}

QString tooLargeReason(qint64 limit)
{
    return tr("it is larger than the %1 parch reads").arg(sizeText(limit));
}

bool readDescriptor(int descriptor, qint64 limit, QByteArray &bytes, QString &error, bool *tooLarge, qint64 sizeHint)
{
    if (tooLarge) *tooLarge = false;
    bytes.clear();
    limit = std::max<qint64>(0, limit);
    // One byte past the limit tells a file of exactly the limit from a larger one.
    const qint64 most = limit + 1;
    qint64 filled = 0;
    bytes.resize(qsizetype(std::clamp<qint64>(sizeHint + 1, 1, std::min(most, Piece))));
    for (;;) {
        if (filled == bytes.size()) {
            if (filled >= most) break;
            const qint64 grown = std::min(most, std::max<qint64>(filled + Piece, filled * 2));
            bytes.resize(qsizetype(grown));
        }
        const ssize_t got = ::read(descriptor, bytes.data() + filled, size_t(bytes.size() - filled));
        if (got < 0) {
            if (errno == EINTR) continue;
            const int number = errno;
            bytes.clear();
            error = systemError(number);
            return false;
        }
        if (got == 0) break;
        filled += got;
    }
    if (filled > limit) {
        bytes.clear();
        if (tooLarge) *tooLarge = true;
        error = tooLargeReason(limit);
        return false;
    }
    bytes.resize(qsizetype(filled));
    return true;
}

bool readForCopy(int descriptor, qint64 expectedSize, QByteArray &bytes, QString &error)
{
    if (expectedSize < 0 || expectedSize >= QByteArray::maxSize()) {
        bytes.clear();
        error = tr("the file is too large to copy");
        return false;
    }
    if (!readDescriptor(descriptor, expectedSize, bytes, error, nullptr, expectedSize)) return false;
    if (bytes.size() == expectedSize) return true;
    bytes.clear();
    error = tr("the file ended before its reported size");
    return false;
}

namespace {

QString outsideFolders(const QString &canonical, const QStringList &folders)
{
    for (const QString &folder : folders)
        if (md::isWithin(canonical, folder)) return {};
    return tr("it is outside the document's folder and library");
}

Result readFile(const QString &path, qint64 limit, const QStringList *folders)
{
    Result result;
    if (path.isEmpty()) {
        result.missing = true;
        result.error = systemError(ENOENT);
        return result;
    }
    // parch's own files, such as its guides and what they include, are
    // resources in memory, whose size is what they hold.
    if (path.startsWith(QLatin1StringView(":/"))) {
        QFile file(path);
        if (!file.exists() || QFileInfo(path).isDir()) {
            result.missing = !file.exists();
            result.error = result.missing ? systemError(ENOENT) : tr("it is a folder");
            return result;
        }
        result.canonicalPath = QDir::cleanPath(path);
        if (folders) result.error = outsideFolders(result.canonicalPath, *folders);
        if (!result.error.isEmpty()) return result;
        result.size = file.size();
        if (result.size > limit) {
            result.tooLarge = true;
            result.error = tooLargeReason(limit);
            return result;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            result.error = file.errorString();
            return result;
        }
        result.bytes = file.read(limit);
        result.size = result.bytes.size();
        result.ok = true;
        return result;
    }
    // A FIFO blocks a plain open until a writer comes, so the file is
    // opened without blocking, and looked at before anything is read.
    const int descriptor = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) {
        const int number = errno;
        result.missing = number == ENOENT || number == ENOTDIR;
        // A FIFO with no reader refuses a write-only open, never this one,
        // but a socket refuses any open: it is no regular file either.
        result.error = number == ENXIO ? tr("it is not a regular file") : systemError(number);
        return result;
    }
    const auto done = qScopeGuard([descriptor] { ::close(descriptor); });
    struct stat state {};
    if (::fstat(descriptor, &state) != 0) {
        result.error = systemError(errno);
        return result;
    }
    char name[PATH_MAX];
    const ssize_t length = ::readlink(QStringLiteral("/proc/self/fd/%1").arg(descriptor).toLatin1().constData(), name,
                                      sizeof name - 1);
    if (length > 0) result.canonicalPath = QFile::decodeName(QByteArray(name, length));
    if (result.canonicalPath.isEmpty() || isSystemPath(result.canonicalPath)) {
        result.error = result.canonicalPath.isEmpty() ? tr("its real name cannot be found")
                                                      : systemPathReason(result.canonicalPath);
        return result;
    }
    if (folders) result.error = outsideFolders(result.canonicalPath, *folders);
    if (!result.error.isEmpty()) return result;
    if (S_ISDIR(state.st_mode)) {
        result.error = tr("it is a folder");
        return result;
    }
    if (!S_ISREG(state.st_mode)) {
        result.error = tr("it is not a regular file");
        return result;
    }
    result.size = qint64(state.st_size);
    if (result.size > limit) {
        result.tooLarge = true;
        result.error = tooLargeReason(limit);
        return result;
    }
    bool tooLarge = false;
    if (!readDescriptor(descriptor, limit, result.bytes, result.error, &tooLarge, result.size)) {
        result.tooLarge = tooLarge;
        if (tooLarge) result.size = limit + 1;
        return result;
    }
    result.size = result.bytes.size();
    result.ok = true;
    return result;
}

} // namespace

Result read(const QString &path, qint64 limit)
{
    return readFile(path, limit, nullptr);
}

Result readPicture(const QString &path, const QStringList &folders)
{
    return readFile(path, PictureBytes, &folders);
}

QString pictureRefusal(const QString &path, const QStringList &folders)
{
    QFileInfo info(path);
    QString canonical = info.canonicalFilePath();
    if (canonical.isEmpty() && !path.startsWith(QLatin1StringView(":/"))) {
        // A missing leaf can still be reached through a link in an existing parent.
        QStringList missing;
        while (canonical.isEmpty()) {
            struct stat st {};
            if (::lstat(QFile::encodeName(info.filePath()).constData(), &st) == 0)
                return tr("its path cannot be resolved");
            const int reason = errno;
            if (reason != ENOENT) return systemError(reason);
            missing.prepend(info.fileName());
            const QString parent = info.absolutePath();
            if (parent == info.filePath()) return tr("its path cannot be resolved");
            info.setFile(parent);
            canonical = info.canonicalFilePath();
        }
        canonical = QDir(canonical).filePath(missing.join(u'/'));
    }
    if (canonical.isEmpty()) canonical = QDir::cleanPath(path);
    if (isSystemPath(canonical)) return systemPathReason(canonical);
    if (QFileInfo candidate(path); candidate.exists() && !candidate.isFile()) return tr("it is not a regular file");
    return outsideFolders(canonical, folders);
}

} // namespace SafeRead
