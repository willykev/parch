#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QThreadPool>

class QFileSystemWatcher;
class QTimer;

// Tells when watched files are changed, replaced or removed by someone else.
// Each file is watched together with its folder, because a file that is
// replaced by a rename, as Git and sync tools do, drops out of the watch and
// only its folder reports the new file. A burst of events for one path, such
// as a program writing in several steps, is reported once, after it ends. The
// paths should be canonical, since that is what the system reports.
//
// After a burst the file is looked at off the UI thread, since a slow file
// system must never hold up typing; what was seen is taken on the UI thread,
// and a look that a later one has overtaken is dropped.
class FileWatch : public QObject {
    Q_OBJECT
public:
    static constexpr int DebounceMs = 50;

    explicit FileWatch(QObject *parent = nullptr);
    // Looks at files on `pool` rather than on a thread of its own.
    explicit FileWatch(QThreadPool *pool, QObject *parent = nullptr);
    // Waits for a look still running, which reports to this object.
    ~FileWatch() override;

    void watch(const QString &path);
    void unwatch(const QString &path);
    // The folders of the files watched, each once.
    QStringList folders() const;

signals:
    // The file exists after a burst of events. This can also be a change that
    // left the file as it was, so a receiver compares content.
    void changedOnDisk(const QString &path);
    void removedFromDisk(const QString &path);

private:
    // Enough of a file's identity to tell whether its folder's event concerned it.
    struct Stamp {
        bool exists = false;
        quint64 device = 0, inode = 0;
        qint64 size = 0, seconds = 0, nanoseconds = 0;
        // The entry at the name itself, a link that leads nowhere included,
        // so that one put there or taken away is seen too.
        bool entry = false;
        quint64 entryInode = 0;
        bool operator==(const Stamp &) const = default;
    };

    static Stamp stampOf(const QString &path);
    void addToSystem(const QString &path);
    void schedule(const QString &path, bool certain);
    void fire(const QString &path);
    // What the look numbered `look` saw of the file.
    void take(const QString &path, quint64 look, const Stamp &now);

    QFileSystemWatcher *m_watcher;
    // The watch's own thread, unless it was given a pool.
    QThreadPool m_ownPool;
    QThreadPool *m_pool;
    QSet<QString> m_paths;
    QHash<QString, Stamp> m_stamps;
    QHash<QString, QTimer *> m_timers;
    // Paths whose burst held an event that certainly concerned them, as
    // opposed to a folder event that may be about a neighbour; once looked
    // at, until the look's answer is taken.
    QSet<QString> m_certain;
    QSet<QString> m_lookedCertain;
    // The newest look at each path; an answer to any other is out of date.
    QHash<QString, quint64> m_latest;
    quint64 m_looks = 0;
};
