#include "foldersearch.h"

#include "diskfile.h"
#include "document.h"
#include "documentstore.h"
#include "librarywalk.h"
#include "pathtext.h"
#include "settings.h"
#include "sourcebuffer.h"

#include <QCache>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QMutex>
#include <QSet>
#include <algorithm>
#include <sys/stat.h>
#include <utility>

namespace {
using Identity = std::pair<quint64, quint64>;

// How much of a line a result shows on either side of its match.
constexpr int beforeUnits = 48;
constexpr int afterUnits = 120;
constexpr int lineUnits = 400;
// Matches beyond this many in one document are not counted.
constexpr int countedPerFile = 100000;

std::atomic<quint64> reads{0};

// What a document's file held, for as long as the file still stands so.
struct Cached {
    qint64 size = 0;
    qint64 modified = 0;
    QString text;
};

struct TextCache {
    QMutex mutex;
    QCache<QString, Cached> entries{FolderSearch::CacheUnits};
};

TextCache &textCache()
{
    static TextCache cache;
    return cache;
}

qint64 nanoseconds(const timespec &time)
{
    return qint64(time.tv_sec) * 1000000000 + time.tv_nsec;
}

QString leftTrimmed(const QString &text)
{
    qsizetype at = 0;
    while (at < text.size() && text.at(at).isSpace()) ++at;
    return text.mid(at);
}

bool lineBreakAt(const QString &text, qsizetype at)
{
    const QChar c = text.at(at);
    return c == u'\n' || (c == u'\r' && (at + 1 >= text.size() || text.at(at + 1) != u'\n'));
}
} // namespace

struct FolderSearch::Item {
    // A file's path; a draft's file, for a draft not open.
    QString path;
    // A draft's id.
    QString documentId;
    // The library folder it lies in.
    QString root;
    qint64 size = 0;
    qint64 modified = 0;
    // An open document's text, as it is now.
    bool hasText = false;
    QString text;
};

struct FolderSearch::Hit {
    int line = 0;
    int column = 0;
    int start = 0;
    int length = 0;
    QString before;
    QString match;
    QString after;
    QString lineText;
};

struct FolderSearch::FileResult {
    QString key;
    Section section;
    QList<Hit> hits;
    int count = 0;
};

struct FolderSearch::Batch {
    QList<FileResult> files;
    int searched = 0;
    int unreadable = 0;
};

struct FolderSearch::Run {
    std::atomic<bool> cancel{false};
    QRegularExpression expression;
    QStringList folders;
    AppPaths paths;
    // The text of each open named document, by its file's device and inode.
    QHash<Identity, QString> openTexts;
    QList<Item> openDrafts;
    QSet<QString> openDraftIds;
    QMutex mutex;
    QMap<int, Batch> done;
    // How many batches the walk made, once it has ended.
    int batches = -1;
    int unreadableFolders = 0;
    bool truncated = false;
};

FolderSearch::FolderSearch(const AppPaths &paths, DocumentStore *store, Settings *settings, QObject *parent)
    : QAbstractListModel(parent), m_paths(paths), m_store(store), m_settings(settings)
{
    // The walk and the workers it hands documents to.
    m_pool.setMaxThreadCount(Workers + 1);
    m_taker.setInterval(TakeEveryMs);
    connect(&m_taker, &QTimer::timeout, this, &FolderSearch::take);
}

FolderSearch::~FolderSearch()
{
    if (m_run) m_run->cancel.store(true);
    m_pool.waitForDone();
}

int FolderSearch::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant FolderSearch::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const Row &row = m_rows.at(index.row());
    switch (role) {
    case SectionRole:
        return row.section;
    case PathRole:
        return m_sections.value(row.section).path;
    case DocumentIdRole:
        return m_sections.value(row.section).documentId;
    case TitleRole:
        return m_sections.value(row.section).title;
    case FolderRole:
        return m_sections.value(row.section).folder;
    case LineRole:
        return row.line;
    case ColumnRole:
        return row.column;
    case StartRole:
        return row.start;
    case LengthRole:
        return row.length;
    case BeforeRole:
        return row.before;
    case Qt::DisplayRole:
    case MatchRole:
        return row.match;
    case AfterRole:
        return row.after;
    case LineTextRole:
        return row.lineText;
    default:
        return {};
    }
}

QHash<int, QByteArray> FolderSearch::roleNames() const
{
    return {{SectionRole, "section"}, {PathRole, "path"},       {DocumentIdRole, "documentId"}, {TitleRole, "title"},
            {FolderRole, "folder"},   {LineRole, "line"},       {ColumnRole, "column"},         {StartRole, "start"},
            {LengthRole, "length"},   {BeforeRole, "before"},   {MatchRole, "match"},           {AfterRole, "after"},
            {LineTextRole, "lineText"}};
}

QString FolderSearch::pattern() const { return m_query.pattern; }
bool FolderSearch::caseSensitive() const { return m_query.caseSensitive; }
bool FolderSearch::wholeWord() const { return m_query.wholeWord; }
bool FolderSearch::regex() const { return m_query.regex; }
QString FolderSearch::error() const { return m_error; }
bool FolderSearch::searching() const { return m_searching; }
bool FolderSearch::finished() const { return m_finished; }
int FolderSearch::filesSearched() const { return m_filesSearched; }
int FolderSearch::filesMatched() const { return m_filesMatched; }
int FolderSearch::matchCount() const { return m_matchCount; }
int FolderSearch::unreadable() const { return m_unreadable; }
bool FolderSearch::truncated() const { return m_truncated; }
int FolderSearch::folderCount() const { return m_folderCount; }

void FolderSearch::setPattern(const QString &pattern)
{
    if (pattern == m_query.pattern) return;
    m_query.pattern = pattern;
    queryEdited();
}

void FolderSearch::setCaseSensitive(bool on)
{
    if (on == m_query.caseSensitive) return;
    m_query.caseSensitive = on;
    queryEdited();
}

void FolderSearch::setWholeWord(bool on)
{
    if (on == m_query.wholeWord) return;
    m_query.wholeWord = on;
    queryEdited();
}

void FolderSearch::setRegex(bool on)
{
    if (on == m_query.regex) return;
    m_query.regex = on;
    queryEdited();
}

void FolderSearch::queryEdited()
{
    TextSearch::compile(m_query, &m_error);
    emit queryChanged();
}

QVariantMap FolderSearch::section(const QString &section) const
{
    const Section found = m_sections.value(section);
    return {{QStringLiteral("title"), found.title},
            {QStringLiteral("folder"), found.folder},
            {QStringLiteral("path"), found.path},
            {QStringLiteral("documentId"), found.documentId},
            {QStringLiteral("matches"), found.matches}};
}

QVariantMap FolderSearch::row(int row) const
{
    QVariantMap values;
    if (row < 0 || row >= m_rows.size()) return values;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto role = roles.cbegin(); role != roles.cend(); ++role)
        values.insert(QString::fromLatin1(role.value()), data(index(row), role.key()));
    return values;
}

bool FolderSearch::readText(const QString &path, qint64 size, qint64 modified, QString *text)
{
    TextCache &cache = textCache();
    {
        QMutexLocker lock(&cache.mutex);
        const Cached *cached = cache.entries.object(path);
        if (cached && cached->size == size && cached->modified == modified) {
            *text = cached->text;
            return true;
        }
    }
    if (size > MaxFileBytes) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) return false;
    ++reads;
    const DecodedText decoded = DiskFile::decode(bytes);
    if (!decoded.valid) return false;
    *text = decoded.text;
    QMutexLocker lock(&cache.mutex);
    cache.entries.insert(path, new Cached{size, modified, decoded.text}, std::max<qsizetype>(1, decoded.text.size()));
    return true;
}

quint64 FolderSearch::fileReads() { return reads.load(); }

void FolderSearch::clearCache()
{
    TextCache &cache = textCache();
    QMutexLocker lock(&cache.mutex);
    cache.entries.clear();
}

void FolderSearch::start()
{
    stop();
    beginResetModel();
    m_rows.clear();
    m_sections.clear();
    endResetModel();
    m_nextBatch = 0;
    m_finished = false;
    m_filesSearched = m_filesMatched = m_matchCount = m_unreadable = 0;
    m_truncated = false;
    const std::optional<QRegularExpression> expression = TextSearch::compile(m_query, &m_error);
    emit queryChanged();
    if (!expression) {
        emit progressChanged();
        return;
    }
    auto run = std::make_shared<Run>();
    run->expression = *expression;
    run->folders = m_settings->libraryFolders();
    run->paths = m_paths;
    // Open documents are searched as their text is now.
    for (Document *document : m_store->documents()) {
        if (document->isTextless()) continue;
        const QString text = document->buffer()->text();
        if (document->isDraft()) {
            // A draft nothing is written in holds nothing to find.
            if (text.isEmpty()) continue;
            Item draft;
            draft.documentId = document->id();
            draft.hasText = true;
            draft.text = text;
            run->openDrafts.append(draft);
            run->openDraftIds.insert(document->id());
            continue;
        }
        const DiskVersion version = document->diskVersion();
        if (version.exists) run->openTexts.insert({version.device, version.inode}, text);
    }
    m_folderCount = int(run->folders.size());
    m_run = run;
    m_searching = true;
    m_taker.start();
    QThreadPool *pool = &m_pool;
    m_pool.start([run, pool] { walkJob(run, pool); });
    emit progressChanged();
}

void FolderSearch::cancel()
{
    const bool was = m_searching;
    stop();
    if (was) emit progressChanged();
}

void FolderSearch::stop()
{
    if (m_run) m_run->cancel.store(true);
    m_run.reset();
    m_taker.stop();
    m_searching = false;
}

void FolderSearch::walkJob(const std::shared_ptr<Run> &run, QThreadPool *pool)
{
    int sequence = 0;
    QList<Item> batch;
    const auto hand = [&] {
        if (batch.isEmpty()) return;
        pool->start([run, at = sequence, items = std::exchange(batch, {})] { searchJob(run, at, items); });
        ++sequence;
    };
    // Open drafts first, then the other drafts, then the library's folders.
    batch = run->openDrafts;
    const QDir drafts(run->paths.drafts());
    for (const QString &id : drafts.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (run->cancel.load() || run->openDraftIds.contains(id) || !AppPaths::isDocumentId(id)) continue;
        Item draft;
        draft.path = DocumentStore::draftFile(run->paths, id);
        draft.documentId = id;
        struct stat state {};
        if (::stat(QFile::encodeName(draft.path).constData(), &state) != 0 || !S_ISREG(state.st_mode)) continue;
        draft.size = qint64(state.st_size);
        draft.modified = nanoseconds(state.st_mtim);
        batch.append(draft);
    }
    hand();
    const LibraryWalk::Stats stats = LibraryWalk::walk(run->folders, [&](const LibraryWalk::Found &found) {
        Item item;
        item.path = found.path;
        for (const QString &folder : std::as_const(run->folders))
            if (found.path.startsWith(folder + u'/')) {
                item.root = folder;
                break;
            }
        item.size = found.size;
        item.modified = found.modified;
        const auto open = run->openTexts.constFind({found.device, found.inode});
        if (open != run->openTexts.cend()) {
            item.hasText = true;
            item.text = *open;
        }
        batch.append(item);
        if (batch.size() >= BatchFiles) hand();
        return true;
    }, &run->cancel);
    hand();
    QMutexLocker lock(&run->mutex);
    run->batches = sequence;
    run->unreadableFolders = stats.unreadableFolders;
    run->truncated = stats.truncated;
}

void FolderSearch::searchJob(const std::shared_ptr<Run> &run, int sequence, const QList<Item> &items)
{
    Batch batch;
    for (const Item &item : items) {
        if (run->cancel.load(std::memory_order_relaxed)) break;
        bool read = false;
        FileResult result = searchItem(*run, item, &read);
        if (!read) {
            ++batch.unreadable;
            continue;
        }
        ++batch.searched;
        if (result.count > 0) batch.files.append(std::move(result));
    }
    QMutexLocker lock(&run->mutex);
    run->done.insert(sequence, std::move(batch));
}

FolderSearch::FileResult FolderSearch::searchItem(const Run &run, const Item &item, bool *read)
{
    FileResult result;
    QString text;
    if (item.hasText) {
        text = item.text;
    } else if (!readText(item.path, item.size, item.modified, &text)) {
        return result;
    }
    *read = true;
    // Line numbers are counted as the matches come, from where the last one was.
    int line = 0;
    qsizetype lineStart = 0, scanned = 0;
    TextSearch::forEachMatch(text, run.expression, [&](const QRegularExpressionMatch &match) {
        const qsizetype start = match.capturedStart(), length = match.capturedLength();
        if (length == 0) return true;
        for (; scanned < start; ++scanned) {
            if (!lineBreakAt(text, scanned)) continue;
            ++line;
            lineStart = scanned + 1;
        }
        ++result.count;
        if (result.hits.size() < MaxRowsPerFile) {
            qsizetype lineEnd = start;
            while (lineEnd < text.size() && text.at(lineEnd) != u'\n' && text.at(lineEnd) != u'\r') ++lineEnd;
            const qsizetype matchEnd = std::min(start + length, lineEnd);
            Hit hit;
            hit.line = line + 1;
            hit.column = int(start - lineStart) + 1;
            hit.start = int(start);
            hit.length = int(length);
            const qsizetype from = std::max(lineStart, start - beforeUnits);
            hit.before = text.mid(from, start - from);
            // From the start of the line its indent goes; from within it, a mark says so.
            if (from == lineStart) hit.before = leftTrimmed(hit.before);
            else hit.before.prepend(u'…');
            hit.match = text.mid(start, matchEnd - start);
            const qsizetype until = std::min(lineEnd, matchEnd + afterUnits);
            hit.after = text.mid(matchEnd, until - matchEnd) + (until < lineEnd ? QStringLiteral("…") : QString());
            hit.lineText = text.mid(lineStart, std::min(lineEnd - lineStart, qsizetype(lineUnits))).trimmed();
            result.hits.append(hit);
        }
        return result.count < countedPerFile;
    }, &run.cancel);
    if (result.count == 0) return result;
    if (!item.documentId.isEmpty()) {
        result.key = u"draft:" + item.documentId;
        result.section.title = Document::draftTitle(text);
        result.section.folder = QCoreApplication::translate("FolderSearch", "Drafts");
        result.section.documentId = item.documentId;
    } else {
        result.key = item.path;
        result.section.title = PathText::fileName(item.path);
        result.section.path = item.path;
        // The library folder by its own name, then the folders below it.
        QStringList parts{PathText::fileName(item.root)};
        parts += PathText::folderOf(item.path).mid(item.root.size()).split(u'/', Qt::SkipEmptyParts);
        result.section.folder = parts.join(QStringLiteral(" / "));
    }
    result.section.matches = result.count;
    return result;
}

void FolderSearch::take()
{
    if (!m_run) return;
    QList<Batch> ready;
    int batches = -1;
    {
        QMutexLocker lock(&m_run->mutex);
        int rows = 0;
        while (rows < RowsPerTake && m_run->done.contains(m_nextBatch)) {
            Batch batch = m_run->done.take(m_nextBatch++);
            for (const FileResult &file : std::as_const(batch.files)) rows += int(file.hits.size());
            ready.append(std::move(batch));
        }
        batches = m_run->batches;
        if (batches >= 0 && m_nextBatch >= batches) {
            m_unreadable += m_run->unreadableFolders;
            m_truncated = m_truncated || m_run->truncated;
        }
    }
    QList<Row> added;
    for (const Batch &batch : std::as_const(ready)) {
        m_filesSearched += batch.searched;
        m_unreadable += batch.unreadable;
        for (const FileResult &file : batch.files) {
            // A draft folder that a named document left behind is no draft.
            if (!file.section.documentId.isEmpty() && !m_store->isUnnamedDraft(file.section.documentId)) continue;
            ++m_filesMatched;
            m_matchCount += file.count;
            if (file.count > file.hits.size()) m_truncated = true;
            if (m_rows.size() + added.size() >= MaxRows) {
                m_truncated = true;
                continue;
            }
            m_sections.insert(file.key, file.section);
            for (const Hit &hit : file.hits) {
                if (m_rows.size() + added.size() >= MaxRows) {
                    m_truncated = true;
                    break;
                }
                added.append({file.key, hit.line, hit.column, hit.start, hit.length, hit.before, hit.match, hit.after,
                              hit.lineText});
            }
        }
    }
    if (!added.isEmpty()) {
        beginInsertRows({}, int(m_rows.size()), int(m_rows.size() + added.size()) - 1);
        m_rows.append(added);
        endInsertRows();
    }
    if (batches >= 0 && m_nextBatch >= batches) {
        m_run.reset();
        m_taker.stop();
        m_searching = false;
        m_finished = true;
    }
    if (!ready.isEmpty() || !m_searching) emit progressChanged();
}
