#pragma once

#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

// Every text document in a set of folders, as the library lists them: .md,
// .markdown and .txt files, hidden entries left out. A walk runs on whatever
// thread calls it and asks the file system as it goes, so it belongs off the
// UI thread. Links within the library are followed. Each folder and each file is visited
// once, by device and inode, so that a link back into a folder already
// walked, or a loop of links, ends there.
namespace LibraryWalk {
// How deep a walk goes below each folder it is given, and how many
// documents it finds at most.
constexpr int MaxDepth = 32;
constexpr int MaxFiles = 200000;

// What a walk found the file system to be, beside the documents.
struct Stats {
    // Folders that are there but could not be read.
    int unreadableFolders = 0;
    // The walk stopped at MaxFiles or MaxDepth.
    bool truncated = false;
};

// What is known of a document when it is found, from the one stat that
// found it.
struct Found {
    QString path;
    quint64 device = 0;
    quint64 inode = 0;
    qint64 size = 0;
    // Nanoseconds.
    qint64 modified = 0;
};

// Whether a file name is one the library lists as a text document.
bool isTextDocument(const QString &name);

// Calls `found` for each document beneath the folders, in the order given
// and, within a folder, its files before its subfolders, each sorted by
// name. Stops early once `cancel` is set or `found` returns false.
// The outermost configured folder containing an already resolved path.
QString rootOf(const QString &canonicalPath, const QStringList &roots);
// Resolve the existing prefix even when the chosen folder has yet to be made.
QString resolvedRoot(const QString &path);

Stats walk(const QStringList &folders, const std::function<bool(const Found &)> &found,
           const std::atomic<bool> *cancel = nullptr, const QStringList &allowedRoots = {});

// Every folder beneath the folders given, and those folders themselves, in
// the order a tree of them shows them, each once; at most `limit`.
QStringList folders(const QStringList &roots, int limit, const std::atomic<bool> *cancel = nullptr);
} // namespace LibraryWalk
