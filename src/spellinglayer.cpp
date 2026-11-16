#include "spellinglayer.h"

#include "document.h"
#include "sourcebuffer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QMutex>
#include <QSharedPointer>
#include <algorithm>

// What earlier checks found in paragraphs, by their text and where in them
// prose is, as offsets from the paragraph's start.
struct SpellingLayer::Cache {
    QMutex mutex;
    QHash<size_t, std::vector<std::pair<int, int>>> found;
};

namespace {

bool blank(QStringView line)
{
    return std::all_of(line.begin(), line.end(), [](QChar c) { return c.isSpace(); });
}

// The text in paragraphs that together cover all of it: each from a line
// after a blank line to the next such line.
std::vector<SourceRange> paragraphsOf(QStringView text)
{
    std::vector<int> starts{0};
    bool afterBlank = false;
    int line = 0;
    for (int i = 0; i <= int(text.size()); ++i) {
        if (i < text.size() && text[i] != u'\n' && text[i] != u'\r' && text[i] != QChar(0x2029)) continue;
        const bool empty = blank(text.sliced(line, i - line));
        if (!empty && afterBlank && line > 0) starts.push_back(line);
        afterBlank = empty;
        if (i < text.size() && text[i] == u'\r' && i + 1 < text.size() && text[i + 1] == u'\n') ++i;
        line = i + 1;
    }
    std::vector<SourceRange> paragraphs;
    for (size_t i = 0; i < starts.size(); ++i) {
        const int end = i + 1 < starts.size() ? starts[i + 1] : int(text.size());
        paragraphs.push_back({starts[i], end - starts[i]});
    }
    return paragraphs;
}

struct Check {
    Revision revision;
    // A copy of the text of its own, made as the check starts once typing
    // has paused, so that no later edit has to copy the buffer's text.
    QString text;
    SourceRange visible;
    std::shared_ptr<SpellEngine> engine;
    std::shared_ptr<std::atomic_bool> cancelled;
    QPointer<SpellingLayer> layer;
    quint64 number;
    // The dialect the text is read in, which says where its prose is.
    md::Options options;
};

} // namespace

SpellingLayer::SpellingLayer(QObject *parent) : QObject(parent), m_cache(std::make_shared<Cache>())
{
    m_settle.setSingleShot(true);
    connect(&m_settle, &QTimer::timeout, this, &SpellingLayer::start);
}

SpellingLayer::~SpellingLayer() { cancel(); }

void SpellingLayer::setDecorations(EditorDecorations *decorations)
{
    if (m_decorations == decorations) return;
    if (m_decorations) m_decorations->clearLayer(layerName());
    for (const QMetaObject::Connection &connection : std::as_const(m_decorationConnections)) disconnect(connection);
    m_decorationConnections.clear();
    m_decorations = decorations;
    if (decorations) {
        defineLayer();
        m_decorationConnections = {
            connect(decorations, &EditorDecorations::bindingChanged, this, &SpellingLayer::connectBinding),
            // The view shows its text anew: all of it is checked at once.
            connect(decorations, &EditorDecorations::layersReset, this, [this] { schedule(0); }),
        };
    }
    connectBinding();
    emit decorationsChanged();
}

void SpellingLayer::setChecker(SpellChecker *checker)
{
    if (m_checker == checker) return;
    for (const QMetaObject::Connection &connection : std::as_const(m_checkerConnections)) disconnect(connection);
    m_checkerConnections.clear();
    m_checker = checker;
    if (checker) {
        m_checkerConnections = {
            connect(checker, &SpellChecker::activeChanged, this, &SpellingLayer::refresh),
            connect(checker, &SpellChecker::wordAccepted, this, &SpellingLayer::accepted),
        };
    }
    refresh();
    emit checkerChanged();
}

void SpellingLayer::setColor(const QColor &color)
{
    if (color == m_color) return;
    m_color = color;
    defineLayer();
    emit colorChanged();
}

void SpellingLayer::defineLayer()
{
    if (!m_decorations) return;
    QTextCharFormat wavy;
    wavy.setUnderlineStyle(QTextCharFormat::WaveUnderline);
    wavy.setUnderlineColor(m_color.isValid() ? m_color : QColor(Qt::red));
    // Over every other layer, and gone as soon as its word is edited.
    m_decorations->defineLayer(layerName(), {wavy}, {.z = 100, .dropEdited = true});
}

void SpellingLayer::connectBinding()
{
    for (const QMetaObject::Connection &connection : std::as_const(m_bindingConnections)) disconnect(connection);
    m_bindingConnections.clear();
    m_binding = m_decorations ? m_decorations->binding() : nullptr;
    if (m_binding) {
        m_bindingConnections = {
            // Typing waits: the check starts once it has paused.
            connect(m_binding, &EditorBinding::sourceEdited, this, [this] {
                if (active()) schedule(SettleMs);
            }),
            connect(m_binding, &EditorBinding::documentChanged, this, &SpellingLayer::watchDialect),
        };
    }
    watchDialect();
    refresh();
}

void SpellingLayer::watchDialect()
{
    disconnect(m_dialect);
    // Read in another dialect, other parts of the text are prose.
    if (Document *document = m_binding ? m_binding->document() : nullptr)
        m_dialect = connect(document, &Document::dialectChanged, this, [this] { schedule(0); });
}

bool SpellingLayer::active() const
{
    return m_checker && m_checker->active() && m_decorations && m_binding && m_binding->document()
        && m_binding->textDocument();
}

void SpellingLayer::refresh()
{
    if (active()) {
        schedule(0);
        return;
    }
    cancel();
    if (m_decorations) m_decorations->clearLayer(layerName());
}

void SpellingLayer::schedule(int delayMs)
{
    if (!active()) return;
    const bool was = checking();
    m_settle.start(delayMs);
    if (!was) emit checkingChanged();
}

void SpellingLayer::checkNow()
{
    if (active()) start();
}

void SpellingLayer::cancel()
{
    const bool was = checking();
    if (m_cancelled) m_cancelled->store(true);
    m_cancelled.reset();
    m_settle.stop();
    m_running = false;
    if (was) emit checkingChanged();
}

void SpellingLayer::start()
{
    m_settle.stop();
    if (!active()) return;
    // A check still running reads text that has changed since.
    if (m_cancelled) m_cancelled->store(true);
    m_cancelled = std::make_shared<std::atomic_bool>(false);
    m_running = true;
    m_stale = false;
    SourceBuffer *buffer = m_binding->document()->buffer();
    QString text;
    {
        const QString shared = buffer->text();
        text = QString(shared.constData(), shared.size());
    }
    Check check{buffer->revision(), std::move(text), m_binding->visibleRange(), m_checker->engine(), m_cancelled, this,
                ++m_check, m_binding->document()->markdownOptions()};
    auto cache = m_cache;
    m_checker->pool()->start([check = std::move(check), cache]() mutable {
        if (check.cancelled->load()) return;
        const Revision revision = check.revision;
        const QString &text = check.text;
        const std::vector<md::Range> prose = spellingProse(text, check.options);
        std::vector<SourceRange> paragraphs = paragraphsOf(text);
        // The paragraphs in view first, then the rest in order.
        const int top = check.visible.start, bottom = check.visible.start + check.visible.length;
        const auto inView = [&](const SourceRange &paragraph) {
            return paragraph.start <= bottom && paragraph.start + paragraph.length >= top;
        };
        const auto shown = std::stable_partition(paragraphs.begin(), paragraphs.end(), inView);
        const size_t firstBatch = size_t(shown - paragraphs.begin());

        std::vector<Found> batch;
        QElapsedTimer sinceSent;
        sinceSent.start();
        const auto send = [&](bool last) {
            QMetaObject::invokeMethod(
                QCoreApplication::instance(),
                [layer = check.layer, number = check.number, revision, found = std::move(batch), last] {
                    if (layer) layer->take(number, revision, found, last);
                },
                Qt::QueuedConnection);
            batch = {};
            sinceSent.restart();
        };
        for (size_t i = 0; i < paragraphs.size(); ++i) {
            if (check.cancelled->load()) return;
            const SourceRange paragraph = paragraphs[i];
            const QStringView words = QStringView(text).sliced(paragraph.start, paragraph.length);
            // Its text and where its prose is say what it holds.
            size_t key = qHash(words);
            auto from = std::partition_point(prose.begin(), prose.end(),
                                             [&](const md::Range &range) { return range.end <= paragraph.start; });
            for (; from != prose.end() && from->start < paragraph.start + paragraph.length; ++from)
                key = qHashMulti(key, from->start - paragraph.start, from->length());
            std::vector<std::pair<int, int>> misspelled;
            bool known = false;
            {
                QMutexLocker lock(&cache->mutex);
                if (const auto it = cache->found.constFind(key); it != cache->found.cend()) {
                    misspelled = *it;
                    known = true;
                }
            }
            if (!known) {
                for (const SourceRange &word :
                     spellingWords(text, paragraph.start, paragraph.start + paragraph.length, prose))
                    if (!check.engine->check(text.mid(word.start, word.length)))
                        misspelled.emplace_back(word.start - paragraph.start, word.length);
                QMutexLocker lock(&cache->mutex);
                if (cache->found.size() >= 20000) cache->found.clear();
                cache->found.insert(key, misspelled);
            }
            Found found{paragraph, {}};
            for (const auto &[start, length] : misspelled)
                found.words.push_back({paragraph.start + start, paragraph.start + start + length, 0});
            batch.push_back(std::move(found));
            // What is in view goes at once; the rest a few dozen paragraphs at a time.
            if (i + 1 == firstBatch || batch.size() >= 64 || sinceSent.elapsed() >= 20) send(false);
        }
        send(true);
    });
    emit checkingChanged();
}

void SpellingLayer::take(quint64 check, Revision revision, const std::vector<Found> &found, bool last)
{
    // Found for an earlier check, which this one has replaced.
    if (check != m_check || !m_running || !m_decorations) return;
    for (const Found &paragraph : found)
        if (!m_decorations->replaceSpans(layerName(), revision, paragraph.paragraph, paragraph.words)) m_stale = true;
    if (!last) return;
    m_running = false;
    // Paragraphs edited meanwhile are checked again once typing pauses.
    if (std::exchange(m_stale, false)) schedule(SettleMs);
    if (!checking()) emit checkingChanged();
}

void SpellingLayer::accepted(const QString &word)
{
    {
        QMutexLocker lock(&m_cache->mutex);
        m_cache->found.clear();
    }
    if (!m_decorations || !m_binding || !m_binding->document()) return;
    // The underline goes from every place the word is, at once; the rest of
    // the text is checked again for the forms of it the dictionary now takes.
    const QString text = m_binding->document()->buffer()->text();
    std::vector<DecorationSpan> kept;
    for (const DecorationSpan &span : m_decorations->spans(layerName()))
        if (QStringView(text).sliced(span.start, span.end - span.start) != word) kept.push_back(span);
    m_decorations->setSpans(layerName(), m_decorations->revision(), kept);
    schedule(0);
}

QVariantMap SpellingLayer::misspellingAt(int position) const
{
    if (!m_decorations || !m_binding || !m_binding->document()) return {};
    const DecorationSpan *span = m_decorations->spanAt(layerName(), position);
    if (!span) return {};
    const QString word = m_binding->document()->buffer()->text().mid(span->start, span->end - span->start);
    return {{QStringLiteral("start"), span->start}, {QStringLiteral("end"), span->end}, {QStringLiteral("word"), word}};
}

bool SpellingLayer::replaceWord(int start, int end, const QString &word, const QString &replacement)
{
    if (!m_binding || !m_binding->document() || !m_binding->editable() || start < 0 || end < start) return false;
    SourceBuffer *buffer = m_binding->document()->buffer();
    if (end > buffer->length() || buffer->text().mid(start, end - start) != word) return false;
    const Revision before = buffer->revision();
    m_binding->replaceEditorRange(m_binding->sourceToEditor(start), m_binding->sourceToEditor(end), replacement);
    return buffer->revision() != before;
}
