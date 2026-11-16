#include "privatefiles.h"

#include "diskfile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace PrivateFiles {

bool makeFolder(const QString &path, QString *reason)
{
    return DiskFile::makeFolders(QDir::cleanPath(QFileInfo(path).absoluteFilePath()), reason, 0700);
}

bool restrict(QFileDevice &file)
{
    // Through the device, since a QSaveFile gives its file the permissions
    // it was last set as it commits, whatever the file had meanwhile.
    return file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

QStringList folders(const AppPaths &paths, const QStringList &others)
{
    QStringList all{paths.data, paths.state, paths.config};
    for (const QString &other : others)
        if (!other.isEmpty()) all.append(other);
    return all;
}

namespace {

// Walks a folder by descriptors, never through a link: each entry is
// opened relative to its folder with O_NOFOLLOW, so that nothing swapped
// for a link meanwhile is changed instead.
void tightenAt(int folder, int depth)
{
    struct stat status;
    if (::fstat(folder, &status) != 0 || !S_ISDIR(status.st_mode)) return;
    if ((status.st_mode & 077) != 0) ::fchmod(folder, status.st_mode & 07700);
    if (depth > 32) return;
    const int listing = ::dup(folder);
    if (listing < 0) return;
    DIR *entries = ::fdopendir(listing);
    if (!entries) {
        ::close(listing);
        return;
    }
    while (const dirent *entry = ::readdir(entries)) {
        const char *name = entry->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        struct stat inner;
        if (::fstatat(folder, name, &inner, AT_SYMLINK_NOFOLLOW) != 0) continue;
        if (S_ISDIR(inner.st_mode)) {
            const int child = ::openat(folder, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) continue;
            tightenAt(child, depth + 1);
            ::close(child);
        } else if (S_ISREG(inner.st_mode) && (inner.st_mode & 077) != 0) {
            const int file = ::openat(folder, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
            if (file < 0) continue;
            struct stat opened;
            if (::fstat(file, &opened) == 0 && S_ISREG(opened.st_mode)) ::fchmod(file, opened.st_mode & 07700);
            ::close(file);
        }
    }
    ::closedir(entries);
}

} // namespace

void tighten(const QStringList &folders)
{
    for (const QString &path : folders) {
        const int folder = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (folder < 0) continue;
        tightenAt(folder, 0);
        ::close(folder);
    }
}

} // namespace PrivateFiles
