#include "filewatch.h"

#include "pathtext.h"

#include <QFile>
#include <QFileSystemWatcher>
#include <QMetaObject>
#include <QTimer>
#include <sys/stat.h>

FileWatch::FileWatch(QObject *parent) : FileWatch(nullptr, parent) {}

FileWatch::FileWatch(QThreadPool *pool, QObject *parent)
    : QObject(parent), m_watcher(new QFileSystemWatcher(this)), m_pool(pool ? pool : &m_ownPool)
{
    // One thread keeps the looks in order and costs nothing while idle.
    m_ownPool.setMaxThreadCount(1);
    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &path) { schedule(path, true); });
    connect(m_watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &folder) {
        // Any file in the folder may be the one that changed.
        const QSet<QString> paths = m_paths;
        for (const QString &path : paths)
            if (PathText::folderOf(path) == folder) schedule(path, false);
    });
}

FileWatch::~FileWatch()
{
    // A look still running reports to this object, so it ends first; its
    // answer, queued for this object, goes with it.
    m_pool->waitForDone();
}

void FileWatch::watch(const QString &path)
{
    if (path.isEmpty() || m_paths.contains(path)) return;
    m_paths.insert(path);
    m_stamps.insert(path, stampOf(path));
    addToSystem(path);
}

void FileWatch::unwatch(const QString &path)
{
    if (!m_paths.remove(path)) return;
    m_stamps.remove(path);
    m_certain.remove(path);
    m_lookedCertain.remove(path);
    m_latest.remove(path);
    delete m_timers.take(path);
    m_watcher->removePath(path);
    // The folder stays watched while another watched file is in it.
    const QString folder = PathText::folderOf(path);
    for (const QString &other : std::as_const(m_paths))
        if (PathText::folderOf(other) == folder) return;
    m_watcher->removePath(folder);
}

QStringList FileWatch::folders() const
{
    QStringList folders;
    for (const QString &path : m_paths) {
        const QString folder = PathText::folderOf(path);
        if (!folders.contains(folder)) folders.append(folder);
    }
    return folders;
}

FileWatch::Stamp FileWatch::stampOf(const QString &path)
{
    struct stat state {};
    Stamp stamp;
    if (::lstat(QFile::encodeName(path).constData(), &state) == 0) {
        stamp.entry = true;
        stamp.entryInode = quint64(state.st_ino);
    }
    if (::stat(QFile::encodeName(path).constData(), &state) != 0) return stamp;
    stamp.exists = true;
    stamp.device = quint64(state.st_dev);
    stamp.inode = quint64(state.st_ino);
    stamp.size = qint64(state.st_size);
    stamp.seconds = qint64(state.st_mtim.tv_sec);
    stamp.nanoseconds = qint64(state.st_mtim.tv_nsec);
    return stamp;
}

void FileWatch::addToSystem(const QString &path)
{
    // A path that cannot be watched now, such as a file that does not exist,
    // is tried again after the next burst of events.
    if (!m_watcher->files().contains(path)) m_watcher->addPath(path);
    const QString folder = PathText::folderOf(path);
    if (!m_watcher->directories().contains(folder)) m_watcher->addPath(folder);
}

void FileWatch::schedule(const QString &path, bool certain)
{
    if (certain) m_certain.insert(path);
    QTimer *&timer = m_timers[path];
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(DebounceMs);
        connect(timer, &QTimer::timeout, this, [this, path] { fire(path); });
    }
    // Every event of a burst restarts the wait.
    timer->start();
}

void FileWatch::fire(const QString &path)
{
    if (!m_paths.contains(path)) return;
    // Certainty carries over to a later look, should this one be overtaken.
    if (m_certain.remove(path)) m_lookedCertain.insert(path);
    const quint64 look = ++m_looks;
    m_latest.insert(path, look);
    m_pool->start([this, path, look] {
        const Stamp now = stampOf(path);
        QMetaObject::invokeMethod(this, [this, path, look, now] { take(path, look, now); }, Qt::QueuedConnection);
    });
}

void FileWatch::take(const QString &path, quint64 look, const Stamp &now)
{
    // Unwatched since, or looked at again since: a later answer counts.
    if (!m_paths.contains(path) || m_latest.value(path) != look) return;
    m_latest.remove(path);
    const bool certain = m_lookedCertain.remove(path);
    const Stamp before = std::exchange(m_stamps[path], now);
    // A file renamed over is a new file. The system usually drops the old one,
    // but a copy still held open or linked elsewhere keeps being watched, and
    // then writes to the new file would go unseen, so the watch is renewed.
    if (now.exists && (now.inode != before.inode || now.device != before.device)) m_watcher->removePath(path);
    addToSystem(path);
    // A folder event about a neighbour leaves this file as it was.
    if (!certain && now == before) return;
    if (now.exists) emit changedOnDisk(path);
    else emit removedFromDisk(path);
}
