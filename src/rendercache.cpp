#include "rendercache.h"

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>
#include <fcntl.h>
#include <sys/stat.h>

namespace {

constexpr quint32 kMagic = 0x50524331; // "PRC1"
constexpr quint32 kVersion = 1;
const QString kSuffix = QStringLiteral(".render");

QByteArray serialize(const RenderResult &result)
{
    QByteArray bytes;
    QDataStream out(&bytes, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_6_0);
    out << kMagic << kVersion << qint32(result.status) << result.format << result.data << result.size
        << double(result.baseline) << double(result.scale) << result.vector << result.description << result.diagnostic
        << result.note << result.networkIsolated;
    return bytes;
}

std::optional<RenderResult> deserialize(const QByteArray &bytes)
{
    QDataStream in(bytes);
    in.setVersion(QDataStream::Qt_6_0);
    quint32 magic = 0, version = 0;
    qint32 status = 0;
    double baseline = 0, scale = 1;
    RenderResult result;
    in >> magic >> version;
    if (magic != kMagic || version != kVersion) return std::nullopt;
    in >> status >> result.format >> result.data >> result.size >> baseline >> scale >> result.vector >>
        result.description >> result.diagnostic >> result.note >> result.networkIsolated;
    if (in.status() != QDataStream::Ok || status != RenderResult::Rendered) return std::nullopt;
    result.status = RenderResult::Rendered;
    result.baseline = baseline;
    result.scale = scale;
    return result;
}

} // namespace

RenderCache::RenderCache(const QString &folder, qint64 diskBytes, qint64 memoryBytes)
    : m_folder(folder), m_diskLimit(diskBytes)
{
    m_memory.setMaxCost(std::max<qint64>(memoryBytes, 0));
}

QString RenderCache::standardFolder()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/parch/render");
}

QString RenderCache::path(const QString &key) const { return m_folder + u'/' + key + kSuffix; }

void RenderCache::load()
{
    if (m_loaded) return;
    m_loaded = true;
    if (m_folder.isEmpty()) return;
    // The files' times keep the order of use from one session to the next.
    QFileInfoList files = QDir(m_folder).entryInfoList({QStringLiteral("*") + kSuffix}, QDir::Files);
    std::sort(files.begin(), files.end(),
              [](const QFileInfo &a, const QFileInfo &b) { return a.lastModified() < b.lastModified(); });
    for (const QFileInfo &file : std::as_const(files)) {
        const QString key = file.completeBaseName();
        m_disk.insert(key, {file.size(), ++m_clock});
        m_diskTotal += file.size();
    }
    evict();
}

qint64 RenderCache::diskBytes()
{
    load();
    return m_diskTotal;
}

std::optional<RenderResult> RenderCache::find(const QString &key)
{
    m_lastFromDisk = false;
    load();
    if (const RenderResult *kept = m_memory.object(key)) {
        RenderResult result = *kept;
        result.cached = true;
        result.elapsedMs = 0;
        // Still in use, so last in line to leave the disk.
        if (const auto it = m_disk.find(key); it != m_disk.end()) {
            it->used = ++m_clock;
            ::utimensat(AT_FDCWD, QFile::encodeName(path(key)).constData(), nullptr, 0);
        }
        return result;
    }
    const auto it = m_disk.find(key);
    if (it == m_disk.end()) return std::nullopt;
    QFile file(path(key));
    std::optional<RenderResult> result;
    if (file.open(QIODevice::ReadOnly)) result = deserialize(file.readAll());
    file.close();
    if (!result) {
        // Damaged or from another version: drawn again and replaced.
        m_diskTotal -= it->bytes;
        m_disk.erase(it);
        QFile::remove(path(key));
        return std::nullopt;
    }
    it->used = ++m_clock;
    ::utimensat(AT_FDCWD, QFile::encodeName(path(key)).constData(), nullptr, 0);
    m_memory.insert(key, new RenderResult(*result), std::max<qsizetype>(result->data.size(), 1));
    result->cached = true;
    result->elapsedMs = 0;
    m_lastFromDisk = true;
    return result;
}

void RenderCache::insert(const QString &key, const RenderResult &result)
{
    RenderResult kept = result;
    kept.cached = false;
    m_memory.insert(key, new RenderResult(kept), std::max<qsizetype>(kept.data.size(), 1));
    if (result.status != RenderResult::Rendered || m_folder.isEmpty() || m_diskLimit <= 0) return;
    load();
    const QByteArray bytes = serialize(kept);
    if (bytes.size() > m_diskLimit) return;
    QDir().mkpath(m_folder);
    QSaveFile file(path(key));
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return;
    if (const auto it = m_disk.constFind(key); it != m_disk.constEnd()) m_diskTotal -= it->bytes;
    m_disk.insert(key, {bytes.size(), ++m_clock});
    m_diskTotal += bytes.size();
    evict();
}

void RenderCache::evict()
{
    while (m_diskTotal > m_diskLimit && !m_disk.isEmpty()) {
        auto oldest = m_disk.begin();
        for (auto it = m_disk.begin(); it != m_disk.end(); ++it)
            if (it->used < oldest->used) oldest = it;
        QFile::remove(path(oldest.key()));
        m_diskTotal -= oldest->bytes;
        m_disk.erase(oldest);
        ++m_evictions;
    }
}
