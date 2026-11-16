#include "recentfiles.h"

#include "diskfile.h"
#include "pathtext.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QtConcurrent>
#include <utility>

namespace {
constexpr int formatVersion = 1;
const auto versionKey = QStringLiteral("version");
const auto pathsKey = QStringLiteral("paths");
} // namespace

RecentFiles::RecentFiles(const AppPaths &paths, QObject *parent) : QObject(parent), m_file(paths.recentFile())
{
    connect(&m_watcher, &QFutureWatcherBase::finished, this, &RecentFiles::finished);
    load();
}

RecentFiles::~RecentFiles()
{
    m_watcher.waitForFinished();
    if (m_waiting && !writeFile(m_file, *m_waiting))
        qWarning("parch: the recent files could not be saved to %s", qPrintable(m_file));
}

void RecentFiles::note(const QString &path)
{
    if (!PathText::isFullPath(path) || m_paths.value(0) == path) return;
    m_paths.removeAll(path);
    m_paths.prepend(path);
    while (m_paths.size() > MaxPaths) m_paths.removeLast();
    write();
    emit changed();
}

QStringList RecentFiles::paths() const { return m_paths; }

bool RecentFiles::missing(const QString &path) const { return isMissing(path); }

bool RecentFiles::isMissing(const QString &path) { return !QFileInfo::exists(path); }

void RecentFiles::load()
{
    QFile input(m_file);
    if (!input.exists()) return;
    QJsonParseError error{};
    const QJsonDocument document =
        input.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(input.readAll(), &error) : QJsonDocument();
    input.close();
    const QJsonObject root = document.object();
    const bool readable = error.error == QJsonParseError::NoError && document.isObject()
                          && root.value(versionKey).toInt(-1) == formatVersion && root.value(pathsKey).isArray();
    if (!readable) {
        // A file that cannot be read is kept, aside, and never written over.
        qWarning("parch: the recent files in %s cannot be read; starting without them", qPrintable(m_file));
        if (!DiskFile::moveAside(m_file))
            qWarning("parch: the unreadable recent files %s cannot be moved aside", qPrintable(m_file));
        return;
    }
    // Only full paths, each once, and no more than the list holds.
    for (const QJsonValue &value : root.value(pathsKey).toArray()) {
        const QString path = value.toString();
        if (PathText::isFullPath(path) && !m_paths.contains(path)) m_paths.append(path);
        if (m_paths.size() == MaxPaths) break;
    }
}

void RecentFiles::write()
{
    QJsonObject root;
    root.insert(versionKey, formatVersion);
    root.insert(pathsKey, QJsonArray::fromStringList(m_paths));
    const QByteArray bytes = QJsonDocument(root).toJson();
    // Busy until the finished write has been seen here, so that exactly one
    // write is ever in flight and an older list never lands after a newer one.
    if (m_busy) {
        m_waiting = bytes;
        return;
    }
    start(bytes);
}

void RecentFiles::start(const QByteArray &bytes)
{
    m_busy = true;
    m_watcher.setFuture(QtConcurrent::run(&RecentFiles::writeFile, m_file, bytes));
}

void RecentFiles::finished()
{
    m_busy = false;
    if (!m_watcher.result()) qWarning("parch: the recent files could not be saved to %s", qPrintable(m_file));
    if (m_waiting) start(*std::exchange(m_waiting, std::nullopt));
}

bool RecentFiles::writeFile(const QString &file, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) return false;
    QSaveFile output(file);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
