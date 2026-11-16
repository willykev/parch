#pragma once

#include "renderrequest.h"

#include <QCache>
#include <QHash>
#include <QString>

#include <optional>

// Drawn formulas and diagrams, kept so that the same one is never drawn
// twice: in memory for the session, and on disk across sessions, each
// within a size bound, the least recently used going first. A key names
// everything that went into an artifact (the helper's identity, the source,
// the colours, the size, the scale and the fonts), so a key never needs
// invalidating: anything that changes makes another key. Only artifacts
// that were drawn go to disk; refusals stay in memory for the session.
// Used from one thread.
class RenderCache {
public:
    RenderCache(const QString &folder, qint64 diskBytes, qint64 memoryBytes);

    // $XDG_CACHE_HOME/parch/render.
    static QString standardFolder();

    std::optional<RenderResult> find(const QString &key);
    void insert(const QString &key, const RenderResult &result);

    QString folder() const { return m_folder; }
    qint64 diskBytes();
    int evictions() const { return m_evictions; }
    // Whether the last find() was answered from disk.
    bool lastFromDisk() const { return m_lastFromDisk; }

private:
    struct Entry {
        qint64 bytes = 0;
        qint64 used = 0; // a counter, not a time: higher is more recent
    };
    void load();
    QString path(const QString &key) const;
    void evict();

    QString m_folder;
    qint64 m_diskLimit;
    QCache<QString, RenderResult> m_memory;
    QHash<QString, Entry> m_disk;
    qint64 m_diskTotal = 0;
    qint64 m_clock = 0;
    int m_evictions = 0;
    bool m_loaded = false;
    bool m_lastFromDisk = false;
};
