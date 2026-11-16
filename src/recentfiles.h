#pragma once

#include "apppaths.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <optional>

// The files opened and saved most recently, newest first and at most
// MaxPaths of them, kept in AppPaths::recentFile() as JSON. The list changes
// at once; the file is replaced atomically off the UI thread, one write at a
// time, and the newest list waiting is written before the object goes.
//
// A file that cannot be read as a list of recent files is never written
// over: it is moved aside as <name>.bad (or .bad.2 and on, beside earlier
// ones), and the list starts empty.
class RecentFiles : public QObject {
    Q_OBJECT
public:
    static constexpr int MaxPaths = 50;

    explicit RecentFiles(const AppPaths &paths, QObject *parent = nullptr);
    // Waits for a write in progress and writes the list if it is still waiting.
    ~RecentFiles() override;

    // Puts a full path first, once. A path already first changes nothing.
    void note(const QString &path);
    QStringList paths() const;
    // Nothing is at the path any more.
    bool missing(const QString &path) const;
    // The same, as asked off the UI thread, since asking the file system
    // about many paths can take long.
    static bool isMissing(const QString &path);

signals:
    void changed();

private:
    void load();
    void write();
    void start(const QByteArray &bytes);
    void finished();
    static bool writeFile(const QString &file, const QByteArray &bytes);

    QString m_file;
    QStringList m_paths;
    QFutureWatcher<bool> m_watcher;
    std::optional<QByteArray> m_waiting;
    bool m_busy = false;
};
