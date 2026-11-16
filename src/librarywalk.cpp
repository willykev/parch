#include "librarywalk.h"

#include <QCollator>
#include <QFile>
#include <QList>
#include <QSet>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>

namespace {
using Identity = std::pair<quint64, quint64>;

qint64 nanoseconds(const timespec &time)
{
    return qint64(time.tv_sec) * 1000000000 + time.tv_nsec;
}
} // namespace

bool LibraryWalk::isTextDocument(const QString &name)
{
    for (const char *ending : {".md", ".markdown", ".txt"})
        if (name.endsWith(QLatin1String(ending), Qt::CaseInsensitive) && name.size() > qsizetype(std::strlen(ending)))
            return true;
    return false;
}

LibraryWalk::Stats LibraryWalk::walk(const QStringList &folders, const std::function<bool(const Found &)> &found,
                                     const std::atomic<bool> *cancel)
{
    Stats stats;
    QSet<Identity> seenFolders, seenFiles;
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    const auto byName = [&collator](const QString &a, const QString &b) {
        const int order = collator.compare(a, b);
        return order != 0 ? order < 0 : a < b;
    };
    int files = 0;
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
    // Depth first, each folder's files before its subfolders, so that the
    // documents come in the order a tree of the folders shows them.
    struct Pending {
        QString path;
        int depth;
    };
    for (const QString &root : folders) {
        QList<Pending> stack{{root, 0}};
        while (!stack.isEmpty()) {
            if (cancelled()) return stats;
            const Pending folder = stack.takeLast();
            const QByteArray encoded = QFile::encodeName(folder.path);
            struct stat state {};
            if (::stat(encoded.constData(), &state) != 0 || !S_ISDIR(state.st_mode)) continue;
            if (seenFolders.contains({quint64(state.st_dev), quint64(state.st_ino)})) continue;
            seenFolders.insert({quint64(state.st_dev), quint64(state.st_ino)});
            DIR *directory = ::opendir(encoded.constData());
            if (!directory) {
                ++stats.unreadableFolders;
                continue;
            }
            QStringList names, subfolders;
            QList<Found> documents;
            while (const dirent *entry = ::readdir(directory)) {
                const QByteArray raw(entry->d_name);
                const QString name = QFile::decodeName(raw);
                // Hidden entries, and a name that would not lead back to its file, are left out.
                if (name.startsWith(u'.') || QFile::encodeName(name) != raw) continue;
                const bool maybeFolder = entry->d_type == DT_DIR || entry->d_type == DT_LNK || entry->d_type == DT_UNKNOWN;
                if (!maybeFolder && !(entry->d_type == DT_REG && isTextDocument(name))) continue;
                names.append(name);
            }
            ::closedir(directory);
            for (const QString &name : std::as_const(names)) {
                const QString path = folder.path + u'/' + name;
                struct stat target {};
                if (::stat(QFile::encodeName(path).constData(), &target) != 0) continue;
                if (S_ISDIR(target.st_mode)) {
                    subfolders.append(name);
                } else if (S_ISREG(target.st_mode) && isTextDocument(name)) {
                    if (seenFiles.contains({quint64(target.st_dev), quint64(target.st_ino)})) continue;
                    seenFiles.insert({quint64(target.st_dev), quint64(target.st_ino)});
                    documents.append({path, quint64(target.st_dev), quint64(target.st_ino), qint64(target.st_size),
                                      nanoseconds(target.st_mtim)});
                }
            }
            std::sort(documents.begin(), documents.end(), [&](const Found &a, const Found &b) {
                return byName(a.path.mid(folder.path.size() + 1), b.path.mid(folder.path.size() + 1));
            });
            for (const Found &document : std::as_const(documents)) {
                if (cancelled()) return stats;
                if (files >= MaxFiles) {
                    stats.truncated = true;
                    return stats;
                }
                ++files;
                if (!found(document)) return stats;
            }
            if (folder.depth >= MaxDepth) {
                if (!subfolders.isEmpty()) stats.truncated = true;
                continue;
            }
            // Reversed onto the stack, so that the first by name is walked first.
            std::sort(subfolders.begin(), subfolders.end(), byName);
            for (qsizetype i = subfolders.size(); i-- > 0;)
                stack.append({folder.path + u'/' + subfolders.at(i), folder.depth + 1});
        }
    }
    return stats;
}

QStringList LibraryWalk::folders(const QStringList &roots, int limit, const std::atomic<bool> *cancel)
{
    QStringList found;
    QSet<Identity> seen;
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    collator.setNumericMode(true);
    struct Pending {
        QString path;
        int depth;
    };
    for (const QString &root : roots) {
        QList<Pending> stack{{root, 0}};
        while (!stack.isEmpty() && found.size() < limit) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return found;
            const Pending folder = stack.takeLast();
            const QByteArray encoded = QFile::encodeName(folder.path);
            struct stat state {};
            if (::stat(encoded.constData(), &state) != 0 || !S_ISDIR(state.st_mode)) continue;
            if (seen.contains({quint64(state.st_dev), quint64(state.st_ino)})) continue;
            seen.insert({quint64(state.st_dev), quint64(state.st_ino)});
            found.append(folder.path);
            if (folder.depth >= MaxDepth) continue;
            DIR *directory = ::opendir(encoded.constData());
            if (!directory) continue;
            QStringList subfolders;
            while (const dirent *entry = ::readdir(directory)) {
                const QByteArray raw(entry->d_name);
                const QString name = QFile::decodeName(raw);
                if (name.startsWith(u'.') || QFile::encodeName(name) != raw) continue;
                if (entry->d_type != DT_DIR && entry->d_type != DT_LNK && entry->d_type != DT_UNKNOWN) continue;
                struct stat target {};
                if (::stat((encoded + '/' + raw).constData(), &target) == 0 && S_ISDIR(target.st_mode))
                    subfolders.append(name);
            }
            ::closedir(directory);
            std::sort(subfolders.begin(), subfolders.end(), [&collator](const QString &a, const QString &b) {
                const int order = collator.compare(a, b);
                return order != 0 ? order < 0 : a < b;
            });
            for (qsizetype i = subfolders.size(); i-- > 0;)
                stack.append({folder.path + u'/' + subfolders.at(i), folder.depth + 1});
        }
    }
    return found;
}
