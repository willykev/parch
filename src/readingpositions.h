#pragma once

#include "apppaths.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <optional>

// Where the writer was in each file: the caret, the selection's other end
// and the first line in view, as source offsets, kept beside the recent
// files in AppPaths::positionsFile() as JSON, for the MaxPaths files whose
// position changed most recently. A file opened again in a new tab starts there. The list
// changes at once; the file is replaced atomically off the UI thread, one
// write at a time, and the newest list waiting is written before the object
// goes. A file that cannot be read as positions is moved aside, as
// <name>.bad, and never written over.
class ReadingPositions : public QObject {
    Q_OBJECT
public:
    static constexpr int MaxPaths = 500;

    struct Position {
        int cursor = 0;
        int anchor = 0;
        int top = 0;
        bool operator==(const Position &) const = default;
    };

    explicit ReadingPositions(const AppPaths &paths, QObject *parent = nullptr);
    ~ReadingPositions() override;

    // Keeps where the writer is in a file, given by its full path, as the
    // newest. The position it has already changes nothing.
    void note(const QString &path, const Position &position);
    std::optional<Position> at(const QString &path) const;
    // A file, or a folder and everything in it, moved: its positions follow.
    void relocate(const QString &from, const QString &to);
    // The paths kept, the newest first.
    QStringList paths() const;

private:
    void load();
    void write();
    void start(const QByteArray &bytes);
    void finished();
    static bool writeFile(const QString &file, const QByteArray &bytes);

    QString m_file;
    QHash<QString, Position> m_positions;
    // The newest first.
    QStringList m_order;
    QFutureWatcher<bool> m_watcher;
    std::optional<QByteArray> m_waiting;
    bool m_busy = false;
};
