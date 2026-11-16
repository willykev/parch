#include "history.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimeZone>
#include <algorithm>
#include <utility>

namespace {
const QString indexName = QStringLiteral("index.json");
const QString timeFormat = QStringLiteral("yyyyMMddTHHmmsszzz");

// Version ids become file names, so only the form capture writes is accepted.
bool isVersionId(const QString &id)
{
    static const QRegularExpression form(QStringLiteral(R"(^\d{8}T\d{9}-[0-9a-f]{12}(-\d+)?$)"));
    return form.match(id).hasMatch();
}

QDateTime takenOf(const QString &versionId)
{
    // The digits are UTC, so the date and the time are read apart and put
    // together in UTC: read as one local time first, a time the local clock
    // skips would come out shifted, or not at all.
    const QDate date = QDate::fromString(versionId.left(8), QStringLiteral("yyyyMMdd"));
    const QTime time = QTime::fromString(versionId.mid(9, 9), QStringLiteral("HHmmsszzz"));
    return QDateTime(date, time, QTimeZone::UTC);
}

QDateTime utc(const QDateTime &time)
{
    return time.toUTC();
}

// How a reason is written in the index, which must never change.
const std::pair<History::Reason, const char *> reasonKeys[] = {
    {History::Reason::Opened, "opened"},       {History::Reason::Reload, "reload"},
    {History::Reason::Overwrite, "overwrite"}, {History::Reason::Restore, "restore"},
    {History::Reason::Recovery, "recovery"},   {History::Reason::Draft, "draft"},
    {History::Reason::Closed, "closed"},       {History::Reason::Links, "links"},
};

QString keyOf(History::Reason reason)
{
    for (const auto &[known, key] : reasonKeys)
        if (known == reason) return QString::fromLatin1(key);
    return {};
}

// Anything else, a missing field among it, is a save.
History::Reason reasonOf(const QString &key)
{
    for (const auto &[known, written] : reasonKeys)
        if (key == QLatin1String(written)) return known;
    return History::Reason::Save;
}

// What tests make of a capture: a step before it begins.
std::function<void()> beforeCapture;
} // namespace

History::History(const AppPaths &paths, HistoryLimits limits, std::function<QDateTime()> now, QObject *parent)
    : QObject(parent), m_paths(paths), m_limits(limits), m_now(std::move(now))
{
    // Every document's index is read once, here, on a thread of its own, so
    // that no caller, the UI thread among them, does it; one that needs the
    // totals sooner waits for them. Unnamed versions past the age limit go
    // as it reads, from documents that may never be captured again.
    const QDateTime limit = utc(m_now()).addDays(-m_limits.days);
    m_scan = std::thread([this, limit] {
        QMutexLocker lock(&m_mutex);
        for (const QString &id : documentIds()) {
            const QList<Version> all = load(id);
            QList<Version> kept;
            for (const Version &version : all) {
                if (version.name.isEmpty() && utc(version.taken) < limit) removeFiles(id, version);
                else kept.append(version);
            }
            if (kept.size() == all.size()) continue;
            noteDocument(id, kept);
            store(id, kept);
        }
        m_totals = sumLocked();
        m_scanned.wakeAll();
    });
    // From then on every document's versions are pruned by count and age
    // now and then, on a thread of the pruner's own, so that a document
    // never captured again loses its old versions while parch runs too.
    m_pruner.setMaxThreadCount(1);
    m_pruneTimer.setInterval(m_limits.pruneEveryMs);
    connect(&m_pruneTimer, &QTimer::timeout, this, [this] {
        // One at a time: a prune still running is not joined by another.
        if (m_pruner.activeThreadCount() > 0) return;
        m_pruner.start([this] {
            QMutexLocker lock(&m_mutex);
            pruneAll();
            announceLocked();
        });
    });
    m_pruneTimer.start();
}

History::Usage History::sumLocked() const
{
    Usage totals;
    for (const Usage &share : std::as_const(m_documentUsage)) {
        totals.bytes += share.bytes;
        totals.named += share.named;
    }
    return totals;
}


History::~History()
{
    m_pruneTimer.stop();
    m_pruner.waitForDone();
    m_scan.join();
}

History::Usage History::totalsLocked() const
{
    while (!m_totals) m_scanned.wait(&m_mutex);
    return *m_totals;
}

void History::noteDocument(const QString &documentId, const QList<Version> &versions) const
{
    Usage now;
    for (const Version &version : versions) {
        now.bytes += version.bytes;
        if (!version.name.isEmpty()) now.named += version.bytes;
    }
    const Usage before = m_documentUsage.value(documentId);
    if (versions.isEmpty()) m_documentUsage.remove(documentId);
    else m_documentUsage.insert(documentId, now);
    // Before the scan has finished, its sum takes this in.
    if (!m_totals) return;
    m_totals->bytes += now.bytes - before.bytes;
    m_totals->named += now.named - before.named;
}

History::Usage History::usage() const
{
    QMutexLocker lock(&m_mutex);
    return totalsLocked();
}

int History::indexReads(const QString &documentId) const
{
    QMutexLocker lock(&m_mutex);
    if (!documentId.isEmpty()) return m_indexReads.value(documentId);
    int all = 0;
    for (const int reads : std::as_const(m_indexReads)) all += reads;
    return all;
}

QString History::folderOf(const QString &documentId) const
{
    return m_paths.history() + u'/' + documentId;
}

QStringList History::documentIds() const
{
    QStringList ids;
    for (const QString &name : QDir(m_paths.history()).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (AppPaths::isDocumentId(name)) ids << name;
    return ids;
}

// The index is a cache of the folder, and the files are the versions: one
// missing from the index, as after a crash between the two writes or a lost
// index, is adopted from its file. A name is kept beside its file, and the
// file counts even for a version the index lists without one, as after a
// crash or a failed index write while it was named, so that a checkpoint
// never becomes a version that is pruned.
QList<History::Version> History::load(const QString &documentId) const
{
    if (!AppPaths::isDocumentId(documentId)) return {};
    ++m_indexReads[documentId];
    const QString folder = folderOf(documentId);
    QList<Version> versions;
    QSet<QString> listed;
    // One listing of the folder says which versions have a file and a name.
    QSet<QString> named, kept;
    QStringList files;
    for (const QString &file : QDir(folder).entryList({QStringLiteral("*.md"), QStringLiteral("*.name")}, QDir::Files)) {
        if (file.endsWith(QStringLiteral(".name"))) {
            named.insert(file.chopped(5));
        } else {
            files.append(file);
            kept.insert(file.chopped(3));
        }
    }
    const auto nameFile = [&](const QString &versionId) {
        QFile name(folder + u'/' + versionId + QStringLiteral(".name"));
        return name.open(QIODevice::ReadOnly) ? QString::fromUtf8(name.readAll()) : QString();
    };
    QFile index(folder + u'/' + indexName);
    if (index.open(QIODevice::ReadOnly)) {
        const QJsonDocument parsed = QJsonDocument::fromJson(index.readAll());
        for (const QJsonValue &value : parsed.array()) {
            const QJsonObject entry = value.toObject();
            Version version;
            version.id = entry.value(QStringLiteral("id")).toString();
            version.taken = QDateTime::fromString(entry.value(QStringLiteral("taken")).toString(), Qt::ISODateWithMs);
            version.name = entry.value(QStringLiteral("name")).toString();
            version.bytes = qint64(entry.value(QStringLiteral("bytes")).toDouble());
            version.reason = reasonOf(entry.value(QStringLiteral("reason")).toString());
            if (isVersionId(version.id) && version.taken.isValid() && kept.contains(version.id)) {
                if (version.name.isEmpty() && named.contains(version.id)) version.name = nameFile(version.id);
                listed.insert(version.id);
                versions.append(version);
            }
        }
    }
    for (const QString &file : std::as_const(files)) {
        Version version;
        version.id = file.chopped(3);
        if (!isVersionId(version.id) || listed.contains(version.id)) continue;
        version.taken = takenOf(version.id);
        version.bytes = QFileInfo(folder + u'/' + file).size();
        if (named.contains(version.id)) version.name = nameFile(version.id);
        versions.append(version);
    }
    // Versions taken in the same millisecond keep the index's order, which
    // is the order they were taken in, newest first.
    std::stable_sort(versions.begin(), versions.end(), [](const Version &a, const Version &b) { return a.taken > b.taken; });
    // What was read is what the document's versions take, even after a
    // change made behind history's back.
    noteDocument(documentId, versions);
    return versions;
}

bool History::writeName(const QString &documentId, const QString &versionId, const QString &name) const
{
    QSaveFile file(folderOf(documentId) + u'/' + versionId + QStringLiteral(".name"));
    return file.open(QIODevice::WriteOnly) && file.write(name.toUtf8()) == name.toUtf8().size() && file.commit();
}

bool History::store(const QString &documentId, const QList<Version> &versions) const
{
    QJsonArray entries;
    for (const Version &version : versions) {
        QJsonObject entry;
        entry.insert(QStringLiteral("id"), version.id);
        entry.insert(QStringLiteral("taken"), version.taken.toString(Qt::ISODateWithMs));
        if (!version.name.isEmpty()) entry.insert(QStringLiteral("name"), version.name);
        entry.insert(QStringLiteral("bytes"), double(version.bytes));
        if (version.reason != Reason::Save) entry.insert(QStringLiteral("reason"), keyOf(version.reason));
        entries.append(entry);
    }
    QSaveFile file(folderOf(documentId) + u'/' + indexName);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(entries).toJson(QJsonDocument::Compact));
    return file.commit();
}

QList<History::Version> History::versions(const QString &documentId) const
{
    QMutexLocker lock(&m_mutex);
    // Not before the start has removed what is past the age limit.
    totalsLocked();
    return load(documentId);
}

QByteArray History::read(const QString &documentId, const QString &versionId, bool *found) const
{
    if (found) *found = false;
    if (!AppPaths::isDocumentId(documentId) || !isVersionId(versionId)) return {};
    QMutexLocker lock(&m_mutex);
    totalsLocked();
    QFile file(folderOf(documentId) + u'/' + versionId + QStringLiteral(".md"));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = file.readAll();
    if (found) *found = file.error() == QFileDevice::NoError;
    return bytes;
}

bool History::paused() const
{
    QMutexLocker lock(&m_mutex);
    return pauseLocked() != Pause::None;
}

History::Pause History::pause() const
{
    QMutexLocker lock(&m_mutex);
    return pauseLocked();
}

History::Pause History::pauseLocked() const
{
    if (totalsLocked().named > m_limits.bytes) return Pause::NamedVersionsFill;
    return m_full ? Pause::VersionDidNotFit : Pause::None;
}

QString History::capture(const QString &documentId, const QByteArray &bytes, const QString &name, CaptureStatus *status,
                         Reason reason)
{
    QMutexLocker lock(&m_mutex);
    CaptureStatus outcome = CaptureStatus::Failed;
    const QString id = captureLocked(documentId, load(documentId), bytes, name, outcome, reason);
    if (status) *status = outcome;
    announceLocked();
    return id;
}

QString History::captureIfDue(const QString &documentId, const QByteArray &bytes)
{
    QMutexLocker lock(&m_mutex);
    const QList<Version> existing = load(documentId);
    if (!existing.isEmpty() && utc(existing.first().taken).addSecs(CaptureIntervalMinutes * 60) > utc(m_now()))
        return {};
    CaptureStatus ignored = CaptureStatus::Failed;
    const QString id = captureLocked(documentId, existing, bytes, {}, ignored, Reason::Save);
    announceLocked();
    return id;
}

QString History::captureUnlessNewest(const QString &documentId, const QByteArray &bytes, Reason reason,
                                     CaptureStatus *status)
{
    if (status) *status = CaptureStatus::Failed;
    if (!AppPaths::isDocumentId(documentId)) return {};
    if (beforeCapture) beforeCapture();
    QMutexLocker lock(&m_mutex);
    const QList<Version> existing = load(documentId);
    if (!existing.isEmpty()) {
        QFile newest(folderOf(documentId) + u'/' + existing.first().id + QStringLiteral(".md"));
        if (newest.size() == bytes.size() && newest.open(QIODevice::ReadOnly) && newest.readAll() == bytes
            && newest.error() == QFileDevice::NoError) {
            if (status) *status = CaptureStatus::Held;
            return existing.first().id;
        }
    }
    CaptureStatus outcome = CaptureStatus::Failed;
    const QString id = captureLocked(documentId, existing, bytes, {}, outcome, reason);
    if (status) *status = outcome;
    announceLocked();
    return id;
}

bool History::rename(const QString &documentId, const QString &versionId, const QString &name)
{
    if (name.isEmpty() || !AppPaths::isDocumentId(documentId) || !isVersionId(versionId)) return false;
    QMutexLocker lock(&m_mutex);
    QList<Version> versions = load(documentId);
    const auto found = std::find_if(versions.begin(), versions.end(), [&](const Version &v) { return v.id == versionId; });
    if (found == versions.end()) return false;
    // The name file goes first, as in a capture, so that a lost index never
    // turns the named version back into one that can be pruned. With it, the
    // version counts as named, whatever becomes of the index.
    bool named = writeName(documentId, versionId, name);
    if (named) {
        found->name = name;
        noteDocument(documentId, versions);
        named = store(documentId, versions);
    }
    announceLocked();
    return named;
}

bool History::remove(const QString &documentId, const QString &versionId)
{
    if (!AppPaths::isDocumentId(documentId) || !isVersionId(versionId)) return false;
    QMutexLocker lock(&m_mutex);
    QList<Version> versions = load(documentId);
    if (!versions.removeIf([&](const Version &v) { return v.id == versionId; })) return false;
    const QString base = folderOf(documentId) + u'/' + versionId;
    // The version's file goes first: once it is gone the index no longer
    // lists it, even an index that cannot be written, and a failure before
    // then leaves the version whole, its name included.
    if (!QFile::remove(base + QStringLiteral(".md")) && QFile::exists(base + QStringLiteral(".md"))) return false;
    QFile::remove(base + QStringLiteral(".name"));
    noteDocument(documentId, versions);
    store(documentId, versions);
    // Room was made, so the next capture is tried again rather than taken
    // for one that cannot fit; whether named versions still fill history
    // follows from the totals, which the removal brought down.
    m_full = false;
    announceLocked();
    return true;
}

void History::setBeforeCapture(std::function<void()> hook)
{
    beforeCapture = std::move(hook);
}

void History::checkPaused()
{
    QMutexLocker lock(&m_mutex);
    announceLocked();
}

void History::announceLocked()
{
    // Said under the lock, so that the changes of two threads reach a
    // receiver in the order they were made.
    const Pause now = pauseLocked();
    if (now == std::exchange(m_announced, now)) return;
    emit pausedChanged(now);
}

QString History::captureLocked(const QString &documentId, QList<Version> versions, const QByteArray &bytes,
                               const QString &name, CaptureStatus &status, Reason reason)
{
    status = CaptureStatus::Failed;
    if (!AppPaths::isDocumentId(documentId)) return {};
    // A pause from a full budget ends with the next attempt, since a smaller
    // text can fit where the last did not; only the named versions alone keep
    // history paused for good.
    m_full = false;
    const qint64 named = totalsLocked().named;
    // A capture that is refused still lets the document's own versions go
    // that are past the count or the age, as any capture does.
    if (named > m_limits.bytes) {
        pruneOwn(documentId, versions);
        status = CaptureStatus::Paused;
        return {};
    }
    // Text that cannot fit beside the named versions is refused before
    // anything is written. Writing it first would let pruning delete other
    // versions that did fit, and then delete it as well. A named version is
    // recorded even if it makes the named versions exceed the budget, since
    // the writer asked for it by name; history is then paused, and once named
    // versions alone exceed the budget no further capture is accepted.
    if (name.isEmpty() && named + bytes.size() > m_limits.bytes) {
        pruneOwn(documentId, versions);
        m_full = true;
        status = CaptureStatus::Paused;
        return {};
    }
    const QString folder = folderOf(documentId);
    if (!QDir().mkpath(folder)) return {};
    const QDateTime taken = utc(m_now());
    const QString baseId = taken.toString(timeFormat) + u'-'
        + QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().left(12));
    // An id already taken is the same version only if its bytes and name
    // agree. Otherwise the new version gets a suffixed id, so that no
    // version is ever replaced or renamed.
    QString id = baseId;
    for (int suffix = 2;; ++suffix) {
        const auto clash = std::find_if(versions.begin(), versions.end(), [&](const Version &v) { return v.id == id; });
        if (clash == versions.end() && !QFileInfo::exists(folder + u'/' + id + QStringLiteral(".md"))) break;
        QFile existing(folder + u'/' + id + QStringLiteral(".md"));
        const bool sameBytes = existing.open(QIODevice::ReadOnly) && existing.readAll() == bytes;
        if (sameBytes && clash != versions.end() && (name.isEmpty() || clash->name == name)) {
            status = CaptureStatus::Recorded;
            return id;
        }
        if (sameBytes && clash != versions.end() && clash->name.isEmpty()) {
            clash->name = name;
            if (!writeName(documentId, id, name)) return {};
            noteDocument(documentId, versions);
            if (!store(documentId, versions)) return {};
            status = CaptureStatus::Recorded;
            return id;
        }
        id = baseId + u'-' + QString::number(suffix);
    }
    const QString path = folder + u'/' + id + QStringLiteral(".md");
    // The name goes first: a crash after it leaves a harmless note, never a
    // checkpoint that has lost its name.
    // A name that cannot be written fails the capture, since the checkpoint
    // would lose its name with the index.
    if (!name.isEmpty() && !writeName(documentId, id, name)) {
        QFile::remove(folder + u'/' + id + QStringLiteral(".name"));
        return {};
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        QFile::remove(folder + u'/' + id + QStringLiteral(".name"));
        return {};
    }
    Version version;
    version.id = id;
    version.taken = taken;
    version.name = name;
    version.bytes = bytes.size();
    version.reason = reason;
    versions.prepend(version);
    if (!store(documentId, versions)) {
        // A version missing from the index would be adopted later as one the
        // caller was told was not recorded.
        QFile::remove(path);
        QFile::remove(folder + u'/' + id + QStringLiteral(".name"));
        return {};
    }
    noteDocument(documentId, versions);
    prune(documentId, versions);
    // Pruning every unnamed version can still leave no room for this one;
    // that is a pause the caller can see, not a version that was recorded.
    if (!QFile::exists(path)) {
        m_full = true;
        status = CaptureStatus::Paused;
        return {};
    }
    status = CaptureStatus::Recorded;
    return id;
}

void History::removeFiles(const QString &documentId, const Version &version) const
{
    QFile::remove(folderOf(documentId) + u'/' + version.id + QStringLiteral(".md"));
    QFile::remove(folderOf(documentId) + u'/' + version.id + QStringLiteral(".name"));
}

// Unnamed versions go in this order: those of the document just captured
// beyond the count or older than the age limit; then, only while all
// documents together exceed the byte budget, every document's beyond the
// count or the age, and the oldest of all until the total fits. Named
// versions are never removed.
void History::prune(const QString &documentId, const QList<Version> &versions)
{
    pruneOwn(documentId, versions);
    if (totalsLocked().bytes > m_limits.bytes) pruneAll();
}

void History::pruneOwn(const QString &documentId, const QList<Version> &versions)
{
    const QDateTime limit = utc(m_now()).addDays(-m_limits.days);
    QList<Version> kept;
    int unnamed = 0;
    for (const Version &version : versions) {
        if (version.name.isEmpty() && (++unnamed > m_limits.versions || utc(version.taken) < limit)) {
            removeFiles(documentId, version);
            continue;
        }
        kept.append(version);
    }
    if (kept.size() != versions.size()) {
        noteDocument(documentId, kept);
        store(documentId, kept);
    }
}

void History::pruneAll()
{
    struct Item {
        QString documentId;
        Version version;
    };
    const QDateTime limit = utc(m_now()).addDays(-m_limits.days);
    QList<Item> oldestFirst;
    // Every index is read here, which also brings the totals up to date with
    // anything changed behind history's back.
    for (const QString &documentId : documentIds()) {
        const QList<Version> all = load(documentId);
        QList<Version> kept;
        int unnamed = 0;
        for (const Version &version : all) {
            if (version.name.isEmpty()) {
                if (++unnamed > m_limits.versions || utc(version.taken) < limit) {
                    removeFiles(documentId, version);
                    continue;
                }
                oldestFirst.prepend({documentId, version});
            }
            kept.append(version);
        }
        if (kept.size() != all.size()) {
            noteDocument(documentId, kept);
            store(documentId, kept);
        }
    }
    qint64 total = totalsLocked().bytes;
    if (total <= m_limits.bytes) return;
    // Removing unnamed versions helps only if it can bring the total within
    // the budget. When the named versions alone exceed it, the rest stay and
    // history is paused, since deleting them would lose them for nothing.
    if (totalsLocked().named > m_limits.bytes) return;
    std::stable_sort(oldestFirst.begin(), oldestFirst.end(),
                     [](const Item &a, const Item &b) { return a.version.taken < b.version.taken; });
    QHash<QString, QSet<QString>> dropped;
    for (const Item &item : std::as_const(oldestFirst)) {
        if (total <= m_limits.bytes) break;
        removeFiles(item.documentId, item.version);
        dropped[item.documentId].insert(item.version.id);
        total -= item.version.bytes;
    }
    for (auto it = dropped.cbegin(); it != dropped.cend(); ++it) {
        QList<Version> kept = load(it.key());
        kept.removeIf([&](const Version &v) { return it.value().contains(v.id); });
        noteDocument(it.key(), kept);
        store(it.key(), kept);
    }
}
