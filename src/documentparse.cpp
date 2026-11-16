#include "documentparse.h"

#include "document.h"
#include "mdinclude.h"
#include "pathtext.h"
#include "sourcebuffer.h"

#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent>
#include <atomic>
#include <tuple>

namespace {

std::atomic<int> workerDelay{0};

// Parses run here, off the views' threads, one at a time for each document.
QThreadPool *parsePool()
{
    static QPointer<QThreadPool> pool;
    if (!pool) {
        pool = new QThreadPool(QCoreApplication::instance());
        pool->setObjectName(QStringLiteral("parse"));
        pool->setMaxThreadCount(qBound(1, QThread::idealThreadCount() / 2, 2));
    }
    return pool;
}

struct Job {
    QString text;
    Revision revision = 0;
    Dialect::Layers dialect;
    QString path;
    QString directory;
    QString libraryRoot;
};

// On the worker: the text read as every output of the document reads it,
// with its includes from disk and its bibliography's keys.
ParsedTextPtr parseJob(Job job)
{
    if (const int delay = workerDelay; delay > 0) QThread::msleep(delay);
    md::FileIncludeResolver resolver;
    md::Source source;
    source.path = job.path;
    source.directory = job.directory;
    source.libraryRoot = job.libraryRoot;
    source.resolver = &resolver;
    auto parsed = std::make_shared<ParsedText>();
    parsed->revision = job.revision;
    parsed->text = std::move(job.text);
    parsed->reading = DocumentReading::parse(parsed->text, job.dialect, source);
    return parsed;
}

} // namespace

DocumentParse::DocumentParse(Document *document) : QObject(document), m_document(document)
{
    qRegisterMetaType<ParsedTextPtr>();
}

DocumentParse::~DocumentParse() { letGo(std::move(m_current)); }

void DocumentParse::setWorkerDelay(int milliseconds) { workerDelay = std::max(0, milliseconds); }

DocumentParse::Key DocumentParse::keyNow() const
{
    Key key;
    key.revision = m_document->buffer()->revision();
    key.dialect = m_document->savedDialect();
    key.path = m_document->path();
    if (m_places) {
        std::tie(key.directory, key.libraryRoot) = m_places(m_document);
    } else {
        // Relative paths are found from the document's own folder: its
        // file's, a draft's own, or, for parch's own text, its resources.
        key.directory = m_document->isScratch() ? m_document->scratchFolder()
                        : m_document->isDraft() ? m_draftFolder
                                                : PathText::folderOf(key.path);
        key.libraryRoot = m_document->libraryRoot();
    }
    key.reads = m_reads;
    return key;
}

ParsedTextPtr DocumentParse::current() const
{
    return m_current && m_currentKey == keyNow() ? m_current : nullptr;
}

ParsedTextPtr DocumentParse::request()
{
    if (ParsedTextPtr parse = current()) return parse;
    if (!m_running) start();
    return nullptr;
}

void DocumentParse::whenParsed(QObject *context, std::function<void(const ParsedTextPtr &)> then)
{
    if (const ParsedTextPtr parse = request()) {
        then(parse);
        return;
    }
    connect(this, &DocumentParse::parsed, context, [then = std::move(then)](const ParsedTextPtr &parse) { then(parse); },
            Qt::SingleShotConnection);
}

void DocumentParse::invalidate()
{
    ++m_reads;
}

void DocumentParse::start()
{
    m_running = true;
    const Key key = keyNow();
    Job job;
    job.revision = key.revision;
    job.dialect = key.dialect;
    job.path = key.path;
    job.directory = key.directory;
    job.libraryRoot = key.libraryRoot;
    // A copy of the text of its own, made now rather than as a key is
    // typed: the parse keeps it, and no later edit has to copy the buffer's.
    {
        const QString shared = m_document->buffer()->text();
        job.text = QString(shared.constData(), shared.size());
    }
    const QPointer<DocumentParse> self(this);
    parsePool()->start([self, key, job = std::move(job)]() mutable {
        ParsedTextPtr parse = parseJob(std::move(job));
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, key, parse = std::move(parse)]() mutable {
                if (self) self->finished(std::move(parse), key);
                else letGo(std::move(parse));
            },
            Qt::QueuedConnection);
    });
}

void DocumentParse::letGo(ParsedTextPtr parse)
{
    // A book's tree takes milliseconds to free: a worker frees it, when this
    // was the last hold on it, rather than the thread that draws.
    if (parse) parsePool()->start([parse = std::move(parse)] {});
}

void DocumentParse::finished(ParsedTextPtr parse, const Key &key)
{
    m_running = false;
    ++m_parses;
    // Never older text as current: the text or how it is read changed
    // while it was parsed, so it is parsed again, as it is now, for
    // whoever asked and is still waiting.
    if (!(key == keyNow())) {
        ++m_dropped;
        letGo(std::move(parse));
        start();
        return;
    }
    letGo(std::exchange(m_current, std::move(parse)));
    m_currentKey = key;
    emit parsed(m_current);
}
