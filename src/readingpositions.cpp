#include "readingpositions.h"

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
const auto filesKey = QStringLiteral("files");
} // namespace

ReadingPositions::ReadingPositions(const AppPaths &paths, QObject *parent)
    : QObject(parent), m_file(paths.positionsFile())
{
    connect(&m_watcher, &QFutureWatcherBase::finished, this, &ReadingPositions::finished);
    load();
}

ReadingPositions::~ReadingPositions()
{
    m_watcher.waitForFinished();
    if (m_waiting && !writeFile(m_file, *m_waiting))
        qWarning("parch: the reading positions could not be saved to %s", qPrintable(m_file));
}

void ReadingPositions::note(const QString &path, const Position &position)
{
    if (!PathText::isFullPath(path)) return;
    // A position kept already keeps its place, so that noting every tab
    // again writes nothing.
    const auto known = m_positions.constFind(path);
    if (known != m_positions.cend() && *known == position) return;
    m_positions.insert(path, position);
    m_order.removeAll(path);
    m_order.prepend(path);
    while (m_order.size() > MaxPaths) m_positions.remove(m_order.takeLast());
    write();
}

std::optional<ReadingPositions::Position> ReadingPositions::at(const QString &path) const
{
    const auto known = m_positions.constFind(path);
    if (known == m_positions.cend()) return std::nullopt;
    return *known;
}

void ReadingPositions::relocate(const QString &from, const QString &to)
{
    bool changed = false;
    for (QString &path : m_order) {
        if (path != from && !path.startsWith(from + u'/')) continue;
        const QString moved = to + path.mid(from.size());
        m_positions.insert(moved, m_positions.take(path));
        path = moved;
        changed = true;
    }
    if (changed) write();
}

QStringList ReadingPositions::paths() const { return m_order; }

void ReadingPositions::load()
{
    QFile input(m_file);
    if (!input.exists()) return;
    QJsonParseError error{};
    const QJsonDocument document =
        input.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(input.readAll(), &error) : QJsonDocument();
    input.close();
    const QJsonObject root = document.object();
    const bool readable = error.error == QJsonParseError::NoError && document.isObject()
                          && root.value(versionKey).toInt(-1) == formatVersion && root.value(filesKey).isArray();
    if (!readable) {
        qWarning("parch: the reading positions in %s cannot be read; starting without them", qPrintable(m_file));
        if (!DiskFile::moveAside(m_file))
            qWarning("parch: the unreadable reading positions %s cannot be moved aside", qPrintable(m_file));
        return;
    }
    for (const QJsonValue &value : root.value(filesKey).toArray()) {
        const QJsonObject entry = value.toObject();
        const QString path = entry.value(QStringLiteral("path")).toString();
        if (!PathText::isFullPath(path) || m_positions.contains(path)) continue;
        const auto offset = [&entry](const char *key) { return std::max(0, entry.value(QLatin1String(key)).toInt()); };
        m_positions.insert(path, {offset("cursor"), offset("anchor"), offset("top")});
        m_order.append(path);
        if (m_order.size() == MaxPaths) break;
    }
}

void ReadingPositions::write()
{
    QJsonArray files;
    for (const QString &path : std::as_const(m_order)) {
        const Position position = m_positions.value(path);
        files.append(QJsonObject{{QStringLiteral("path"), path},
                                 {QStringLiteral("cursor"), position.cursor},
                                 {QStringLiteral("anchor"), position.anchor},
                                 {QStringLiteral("top"), position.top}});
    }
    const QByteArray bytes = QJsonDocument(QJsonObject{{versionKey, formatVersion}, {filesKey, files}}).toJson();
    // One write in flight, so that an older list never lands after a newer one.
    if (m_busy) {
        m_waiting = bytes;
        return;
    }
    start(bytes);
}

void ReadingPositions::start(const QByteArray &bytes)
{
    m_busy = true;
    m_watcher.setFuture(QtConcurrent::run(&ReadingPositions::writeFile, m_file, bytes));
}

void ReadingPositions::finished()
{
    m_busy = false;
    if (!m_watcher.result()) qWarning("parch: the reading positions could not be saved to %s", qPrintable(m_file));
    if (m_waiting) start(*std::exchange(m_waiting, std::nullopt));
}

bool ReadingPositions::writeFile(const QString &file, const QByteArray &bytes)
{
    if (!QDir().mkpath(QFileInfo(file).absolutePath())) return false;
    QSaveFile output(file);
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
