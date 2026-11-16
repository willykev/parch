#include "documentreview.h"

#include "diskfile.h"
#include "document.h"
#include "documentstore.h"
#include "sourcebuffer.h"

#include <QFutureWatcher>
#include <QLocale>
#include <QVariantMap>
#include <QtConcurrent>
#include <algorithm>
#include <array>
#include <optional>
#include <vector>
#include <type_traits>
#include <utility>

namespace {
// Text read from a file or a version, without its byte-order mark, or why it
// cannot be shown as text.
QString decoded(const QByteArray &bytes, QString *problem)
{
    // Read as the file's own bytes are, so that a version and the file read alike.
    const DecodedText read = DiskFile::decode(bytes);
    if (read.valid) return read.text;
    *problem = DocumentReview::tr("This is not valid UTF-8 text, so it is not shown.");
    return {};
}
} // namespace

template <typename Work, typename Done>
void DocumentReview::runJob(Work work, Done done)
{
    using Result = std::invoke_result_t<Work>;
    auto *watcher = new QFutureWatcher<Result>(this);
    m_jobs.append(watcher);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, done] {
        m_jobs.removeOne(watcher);
        watcher->deleteLater();
        done(watcher->result());
    });
    watcher->setFuture(QtConcurrent::run(std::move(work)));
}

DocumentReview::DocumentReview(DocumentStore *store, QObject *parent) : QObject(parent), m_store(store)
{
    connect(store, &DocumentStore::diskChanged, this, [this](Document *document) { emit diskChanged(document->id()); });
}

DocumentReview::~DocumentReview()
{
    for (QFutureWatcherBase *job : std::as_const(m_jobs)) job->waitForFinished();
}

void DocumentReview::resolveConflict(Document *document, int choice)
{
    emit acting();
    if (!document) return;
    ConflictChoice chosen = ConflictChoice::Reload;
    switch (choice) {
    case Reload:
        chosen = ConflictChoice::Reload;
        break;
    case KeepBoth:
        chosen = ConflictChoice::KeepBoth;
        break;
    case Overwrite:
        chosen = ConflictChoice::Overwrite;
        break;
    case Follow:
        chosen = ConflictChoice::Follow;
        break;
    case PutHere:
        chosen = ConflictChoice::PutHere;
        break;
    default:
        return;
    }
    QString refusal;
    if (!m_store->resolveConflict(document, chosen, &refusal)) {
        // A refusal is always said: by the document, whose conflict shows
        // the reason, or else here.
        if (document->lastError() != refusal) emit problem(tr("Not resolved"), refusal);
        return;
    }
    // Reload and Overwrite record what they replace.
    emit versionsChanged(document->id());
}

void DocumentReview::checkpoint(Document *document, const QString &name)
{
    emit acting();
    const QString trimmed = name.trimmed();
    if (!document || trimmed.isEmpty()) return;
    const QString id = document->id();
    runJob([history = m_store->history(), id, bytes = document->bytes(), trimmed] {
        History::CaptureStatus status = History::CaptureStatus::Failed;
        history->capture(id, bytes, trimmed, &status);
        return status;
    }, [this, id](History::CaptureStatus status) {
        if (status == History::CaptureStatus::Paused)
            emit problem(tr("Version not kept: history is paused"),
                         tr("Named versions already fill the space history may use. Delete some to make room."));
        else if (status == History::CaptureStatus::Failed)
            emit problem(tr("Version not kept"), tr("The text could not be written to history."));
        emit versionsChanged(id);
    });
}

bool DocumentReview::restoreVersion(Document *document, const QString &versionId)
{
    emit acting();
    if (!document) return false;
    QString refusal;
    if (m_store->restoreVersion(document, versionId, &refusal) == DocumentStore::Restore::Refused) {
        emit problem(tr("Not restored"), refusal);
        return false;
    }
    // The text it replaced is a version now.
    emit versionsChanged(document->id());
    return true;
}

void DocumentReview::nameVersion(Document *document, const QString &versionId, const QString &name)
{
    emit acting();
    const QString trimmed = name.trimmed();
    if (!document || trimmed.isEmpty()) return;
    const QString id = document->id();
    runJob([history = m_store->history(), id, versionId, trimmed] { return history->rename(id, versionId, trimmed); },
           [this, id](bool named) {
        if (!named) emit problem(tr("Version not named"), tr("The name could not be written to history."));
        emit versionsChanged(id);
    });
}

void DocumentReview::removeVersion(Document *document, const QString &versionId)
{
    emit acting();
    if (!document) return;
    const QString id = document->id();
    runJob([history = m_store->history(), id, versionId] { return history->remove(id, versionId); },
           [this, id](bool removed) {
        if (!removed) emit problem(tr("Version not deleted"), tr("The version could not be removed from history."));
        emit versionsChanged(id);
    });
}

int DocumentReview::readVersions(Document *document)
{
    if (!document) return 0;
    const int request = ++m_lastRequest;
    const QString id = document->id();
    runJob([history = m_store->history(), id] {
        const QList<History::Version> kept = history->versions(id);
        QList<QDateTime> taken;
        for (const History::Version &version : kept) taken.append(version.taken);
        const QStringList said = whenAll(taken);
        QVariantList versions;
        for (qsizetype i = 0; i < kept.size(); ++i)
            versions.append(QVariantMap{{QStringLiteral("id"), kept.at(i).id},
                                        {QStringLiteral("taken"), kept.at(i).taken},
                                        {QStringLiteral("when"), said.at(i)},
                                        {QStringLiteral("name"), kept.at(i).name},
                                        {QStringLiteral("reason"), reasonWords(kept.at(i).reason)}});
        return versions;
    }, [this, request, id](const QVariantList &versions) { emit versionsRead(request, id, versions); });
    return request;
}

int DocumentReview::readVersion(Document *document, const QString &versionId)
{
    if (!document) return 0;
    const int request = ++m_lastRequest;
    const QString id = document->id();
    runJob([history = m_store->history(), id, versionId] {
        bool found = false;
        const QByteArray bytes = history->read(id, versionId, &found);
        QString problem;
        const QString text = found ? decoded(bytes, &problem) : QString();
        if (!found) problem = tr("This version is no longer in history.");
        return std::pair(text, problem);
    }, [this, request, id, versionId](const std::pair<QString, QString> &read) {
        emit versionRead(request, id, versionId, read.first, read.second);
    });
    return request;
}

namespace {
// Where each line of a text starts, and the text's end after the last: a
// line ends after LF, or after a CR that no LF follows, and the last line
// may have no ending at all.
QList<qsizetype> lineStarts(QStringView text)
{
    QList<qsizetype> starts{0};
    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u'\n' || (c == u'\r' && (i + 1 == text.size() || text.at(i + 1) != u'\n'))) starts.append(i + 1);
    }
    if (starts.last() != text.size()) starts.append(text.size());
    return starts;
}

} // namespace

namespace {
// Runs of lines that differ, as line numbers within the two middles: a
// line diff (Myers's) that gives up, with nothing, past `limit` edits.
struct LineRun {
    qsizetype aFrom, aTo, bFrom, bTo;
};

std::optional<QList<LineRun>> lineRuns(const QList<QStringView> &a, const QList<QStringView> &b, int limit)
{
    const qsizetype n = a.size(), m = b.size();
    const qsizetype most = std::min<qsizetype>(n + m, limit);
    // The furthest x on each diagonal k = x - y, kept for every round so
    // that the path can be traced back.
    std::vector<int> v(size_t(2 * most + 3), 0);
    const auto at = [&](qsizetype k) -> int & { return v[size_t(k + most + 1)]; };
    std::vector<std::vector<int>> rounds;
    qsizetype found = -1;
    for (qsizetype d = 0; d <= most && found < 0; ++d) {
        for (qsizetype k = -d; k <= d; k += 2) {
            qsizetype x = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? at(k + 1) : at(k - 1) + 1;
            qsizetype y = x - k;
            while (x < n && y < m && a.at(x) == b.at(y)) ++x, ++y;
            at(k) = int(x);
            if (x >= n && y >= m) {
                found = d;
                break;
            }
        }
        rounds.emplace_back(v.begin() + (most + 1 - d), v.begin() + (most + 1 + d + 1));
    }
    if (found < 0) return std::nullopt;
    // The runs of equal lines, traced back from the end, then the gaps between them.
    QList<std::array<qsizetype, 3>> equal;
    qsizetype x = n, y = m;
    for (qsizetype d = found; d > 0; --d) {
        const std::vector<int> &before = rounds[size_t(d - 1)];
        const auto was = [&](qsizetype k) { return qsizetype(before[size_t(k + d - 1)]); };
        const qsizetype k = x - y;
        const qsizetype prevK = (k == -d || (k != d && was(k - 1) < was(k + 1))) ? k + 1 : k - 1;
        const qsizetype prevX = was(prevK), prevY = prevX - prevK;
        const qsizetype startX = prevK == k + 1 ? prevX : prevX + 1;
        if (x > startX) equal.append({startX, startX - k, x - startX});
        x = prevX;
        y = prevY;
    }
    if (x > 0) equal.append({0, 0, x});
    std::reverse(equal.begin(), equal.end());
    equal.append({n, m, 0});
    QList<LineRun> runs;
    qsizetype pa = 0, pb = 0;
    for (const auto &[ex, ey, length] : std::as_const(equal)) {
        if (ex > pa || ey > pb) runs.append({pa, ex, pb, ey});
        pa = ex + length;
        pb = ey + length;
    }
    return runs;
}
} // namespace

QVariantMap DocumentReview::difference(const QString &local, const QString &disk)
{
    if (local == disk) return {{QStringLiteral("differs"), false}};
    const QList<qsizetype> mine = lineStarts(local), theirs = lineStarts(disk);
    // Lines, endings included, as views of their text.
    const auto line = [](const QString &text, const QList<qsizetype> &starts, qsizetype i) {
        return QStringView(text).mid(starts.at(i), starts.at(i + 1) - starts.at(i));
    };
    const qsizetype mineLines = mine.size() - 1, theirLines = theirs.size() - 1;
    qsizetype head = 0;
    while (head < mineLines && head < theirLines && line(local, mine, head) == line(disk, theirs, head)) ++head;
    qsizetype tail = 0;
    while (tail < mineLines - head && tail < theirLines - head
           && line(local, mine, mineLines - 1 - tail) == line(disk, theirs, theirLines - 1 - tail))
        ++tail;
    // A view counts each CRLF as one: how many end the lines before each line start.
    const auto pairsBefore = [](const QString &text, const QList<qsizetype> &starts) {
        QList<qsizetype> pairs{0};
        for (qsizetype i = 1; i < starts.size(); ++i) {
            const qsizetype end = starts.at(i);
            pairs.append(pairs.last() + (end >= 2 && text.at(end - 1) == u'\n' && text.at(end - 2) == u'\r' ? 1 : 0));
        }
        return pairs;
    };
    const QList<qsizetype> minePairs = pairsBefore(local, mine), theirPairs = pairsBefore(disk, theirs);
    // A run, from line numbers of the two texts.
    const auto runAt = [&](qsizetype aFrom, qsizetype aTo, qsizetype bFrom, qsizetype bTo) {
        return QVariantMap{{QStringLiteral("localFrom"), int(mine.at(aFrom))},
                           {QStringLiteral("localTo"), int(mine.at(aTo))},
                           {QStringLiteral("diskFrom"), int(theirs.at(bFrom))},
                           {QStringLiteral("diskTo"), int(theirs.at(bTo))},
                           {QStringLiteral("localStart"), int(mine.at(aFrom) - minePairs.at(aFrom))},
                           {QStringLiteral("localEnd"), int(mine.at(aTo) - minePairs.at(aTo))},
                           {QStringLiteral("diskStart"), int(theirs.at(bFrom) - theirPairs.at(bFrom))},
                           {QStringLiteral("diskEnd"), int(theirs.at(bTo) - theirPairs.at(bTo))}};
    };
    QVariantMap found = runAt(head, mineLines - tail, head, theirLines - tail);
    found.insert(QStringLiteral("differs"), true);
    // The middles, compared line by line, unless either is longer than the
    // diff takes on or needs more edits than it makes: then the whole middle
    // is the one run.
    QList<QStringView> a, b;
    const bool small = mineLines - tail - head <= DiffLines && theirLines - tail - head <= DiffLines;
    if (small) {
        for (qsizetype i = head; i < mineLines - tail; ++i) a.append(line(local, mine, i));
        for (qsizetype i = head; i < theirLines - tail; ++i) b.append(line(disk, theirs, i));
    }
    const std::optional<QList<LineRun>> runs = small ? lineRuns(a, b, DiffEdits) : std::nullopt;
    QVariantList listed;
    if (runs) {
        for (const LineRun &run : *runs)
            listed.append(runAt(head + run.aFrom, head + run.aTo, head + run.bFrom, head + run.bTo));
    } else {
        QVariantMap whole = runAt(head, mineLines - tail, head, theirLines - tail);
        listed.append(whole);
    }
    found.insert(QStringLiteral("runs"), listed);
    found.insert(QStringLiteral("bounded"), !runs.has_value());
    return found;
}

int DocumentReview::readDiskText(Document *document, const QString &compareWith)
{
    if (!document) return 0;
    const int request = ++m_lastRequest;
    const QString id = document->id();
    // A draft's file is its own draft file. What differs is found with the
    // read, off the UI thread, however long the texts.
    struct Read {
        QString text;
        QString problem;
        QVariantMap difference;
    };
    runJob([path = m_store->fileOf(document), compareWith] {
        const LoadedFile loaded = DiskFile::load(path);
        Read read;
        if (!loaded.ok)
            read.problem = DiskFile::fingerprint(path).exists ? loaded.error : tr("The file no longer exists.");
        else if (loaded.readOnly)
            read.problem = tr("The file is not valid UTF-8 text, so it is not shown.");
        else
            read.text = loaded.text;
        if (read.problem.isEmpty()) read.difference = difference(compareWith, read.text);
        return read;
    }, [this, request, id](const Read &read) {
        emit diskTextRead(request, id, read.text, read.problem, read.difference);
    });
    return request;
}

void DocumentReview::record(Document *document, std::function<void(History::CaptureStatus, Revision)> done)
{
    const QString id = document->id();
    const Revision revision = document->buffer()->revision();
    runJob([history = m_store->history(), id, bytes = document->bytes()] {
        History::CaptureStatus status = History::CaptureStatus::Failed;
        history->captureUnlessNewest(id, bytes, History::Reason::Closed, &status);
        return status;
    }, [this, id, revision, done](History::CaptureStatus status) {
        if (status == History::CaptureStatus::Recorded) emit versionsChanged(id);
        done(status, revision);
    });
}

void DocumentReview::checkPaused()
{
    runJob([history = m_store->history()] {
        history->checkPaused();
        return true;
    }, [](bool) {});
}

QString DocumentReview::reasonWords(History::Reason reason)
{
    switch (reason) {
    case History::Reason::Save:
        return {};
    case History::Reason::Opened:
        return tr("as opened");
    case History::Reason::Reload:
        return tr("before reload");
    case History::Reason::Overwrite:
        return tr("before overwrite");
    case History::Reason::Restore:
        return tr("before restore");
    case History::Reason::Recovery:
        return tr("from recovery");
    case History::Reason::Draft:
        return tr("earlier draft");
    case History::Reason::Closed:
        return tr("when closed");
    }
    return {};
}

QStringList DocumentReview::whenAll(const QList<QDateTime> &taken, const QDateTime &now)
{
    // The minute each was taken in, in local time, as when() says it.
    const auto minuteOf = [](const QDateTime &time) {
        const QDateTime local = time.toLocalTime();
        return QDateTime(local.date(), QTime(local.time().hour(), local.time().minute()));
    };
    QStringList said;
    for (qsizetype i = 0; i < taken.size(); ++i) {
        const QDateTime minute = minuteOf(taken.at(i));
        const bool shared = (i > 0 && minuteOf(taken.at(i - 1)) == minute)
                            || (i + 1 < taken.size() && minuteOf(taken.at(i + 1)) == minute);
        said.append(when(taken.at(i), now, shared));
    }
    return said;
}

QString DocumentReview::when(const QDateTime &taken, const QDateTime &now, bool seconds)
{
    const QLocale locale;
    const QDateTime local = taken.toLocalTime();
    const QDate day = local.date(), today = now.toLocalTime().date();
    // A narrow space, as some locales put before AM, reads as none in the
    // interface's monospace font.
    QString format = locale.timeFormat(QLocale::ShortFormat);
    // The seconds go after the minutes, in the locale's own format.
    if (seconds) format.replace(QStringLiteral("mm"), QStringLiteral("mm:ss"));
    QString time = locale.toString(local.time(), format);
    time.replace(QChar(0x202f), u' ');
    if (day == today) return tr("Today, %1").arg(time);
    if (day == today.addDays(-1)) return tr("Yesterday, %1").arg(time);
    const QString date = locale.toString(day, day.year() == today.year() ? tr("MMM d") : tr("MMM d, yyyy"));
    return tr("%1, %2").arg(date, time);
}
