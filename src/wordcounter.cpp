#include "wordcounter.h"

#include "mdtree.h"
#include "sourcebuffer.h"

#include <QCoreApplication>
#include <QLocale>
#include <QTextBoundaryFinder>
#include <QtConcurrent>

namespace {

// Written without spaces between words: each character is counted as one.
bool countsByCharacter(char32_t c)
{
    switch (QChar::script(c)) {
    case QChar::Script_Han:
    case QChar::Script_Hiragana:
    case QChar::Script_Katakana:
        return true;
    default:
        return false;
    }
}

} // namespace

WordCounter::WordCounter(QObject *parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(1);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &WordCounter::start);
    connect(&m_watcher, &QFutureWatcher<int>::finished, this, [this] {
        // Only the count of the text and selection as they still are.
        if (m_running == m_generation && !m_watcher.isCanceled()) take(m_watcher.result(), m_runningSelection);
    });
}

WordCounter::~WordCounter()
{
    m_watcher.disconnect(this);
    m_pool.waitForDone();
}

int WordCounter::count(QStringView markdown, const md::Options &options)
{
    return countWords(md::parse(markdown.toString(), options).plainText());
}

int WordCounter::count(const QString &markdown, int from, int to, const md::Options &options)
{
    md::PlainTextOptions plain;
    plain.range = md::Range{from, to};
    return countWords(md::parse(markdown, options).plainText(plain));
}

int WordCounter::countWords(QStringView text)
{
    int words = 0;
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    qsizetype start = 0;
    for (qsizetype end = finder.toNextBoundary(); end > 0; end = finder.toNextBoundary()) {
        const QStringView segment = text.mid(start, end - start);
        start = end;
        int byCharacter = 0;
        bool letters = false;
        for (const char32_t c : segment.toUcs4()) {
            if (countsByCharacter(c)) ++byCharacter;
            else if (QChar::isLetterOrNumber(c)) letters = true;
        }
        words += byCharacter + (letters ? 1 : 0);
    }
    return words;
}

void WordCounter::setDocument(Document *document)
{
    if (document == m_document) return;
    disconnect(m_edits);
    disconnect(m_dialect);
    m_document = document;
    if (document) {
        m_edits = connect(document->buffer(), &SourceBuffer::sourceChanged, this, [this] {
            ++m_generation;
            schedule(DelayMs);
        });
        // Read in another dialect, the same text may hold other words.
        m_dialect = connect(document, &Document::dialectChanged, this, [this] {
            ++m_generation;
            schedule(0);
        });
    }
    ++m_generation;
    // A document shown counts at once, without waiting for a pause.
    schedule(0);
    emit documentChanged();
}

void WordCounter::setSelectionStart(int position)
{
    if (position == m_selectionStart) return;
    m_selectionStart = position;
    ++m_generation;
    schedule(DelayMs);
    emit selectionChanged();
}

void WordCounter::setSelectionEnd(int position)
{
    if (position == m_selectionEnd) return;
    m_selectionEnd = position;
    ++m_generation;
    schedule(DelayMs);
    emit selectionChanged();
}

void WordCounter::schedule(int delay)
{
    // Each edit puts the count off again, until typing pauses; a count due
    // at once stays due at once.
    if (m_timer.isActive() && m_timer.interval() == 0) return;
    m_timer.start(delay);
}

void WordCounter::start()
{
    if (!m_document) {
        take(0, false);
        return;
    }
    QString text = m_document->buffer()->text();
    const int length = int(text.size());
    const int first = qBound(0, qMin(m_selectionStart, m_selectionEnd), length);
    const int last = qBound(0, qMax(m_selectionStart, m_selectionEnd), length);
    const bool selection = first < last;
    // A selection is read as part of the whole text, so that what it holds
    // means what it means there.
    const md::Options options = m_document->markdownOptions();
    const auto counted = [selection, first, last, options](const QString &text) {
        return selection ? count(text, first, last, options) : count(text, options);
    };
    if (length <= ImmediateUnits) {
        take(counted(text), selection);
        return;
    }
    // A count still running is of older text; its result will be dropped.
    m_running = m_generation;
    m_runningSelection = selection;
    m_watcher.setFuture(QtConcurrent::run(&m_pool, [text = std::move(text), counted] { return counted(text); }));
}

void WordCounter::take(int words, bool ofSelection)
{
    if (words == m_words && ofSelection == m_ofSelection) return;
    m_words = words;
    m_ofSelection = ofSelection;
    emit counted();
}

int WordCounter::minutes() const { return (m_words + WordsPerMinute - 1) / WordsPerMinute; }

QString WordCounter::summary() const
{
    const QString words = m_words == 1 ? tr("1 word") : tr("%1 words").arg(QLocale().toString(m_words));
    const QString counted = m_ofSelection ? tr("%1 selected").arg(words) : words;
    if (m_words == 0) return counted;
    return tr("%1 · %2 min").arg(counted).arg(minutes());
}

QString WordCounter::spoken() const
{
    const QString words = m_words == 1 ? tr("1 word") : tr("%1 words").arg(QLocale().toString(m_words));
    const QString counted = m_ofSelection ? tr("%1 selected").arg(words) : words;
    if (m_words == 0) return counted;
    const QString time = minutes() == 1 ? tr("about 1 minute to read") : tr("about %1 minutes to read").arg(minutes());
    return tr("%1, %2").arg(counted, time);
}
