#include "documentfind.h"

#include "sourcebuffer.h"

#include <QtConcurrent>
#include <algorithm>
#include <utility>

DocumentFind::DocumentFind(QObject *parent) : QObject(parent)
{
    // One search or replacement at a time, which a newer one waits behind.
    m_pool.setMaxThreadCount(1);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &DocumentFind::search);
}

DocumentFind::~DocumentFind()
{
    if (m_cancel) m_cancel->store(true);
    m_pool.waitForDone();
}

Document *DocumentFind::document() const { return m_document; }
bool DocumentFind::active() const { return m_active; }
QString DocumentFind::pattern() const { return m_query.pattern; }
bool DocumentFind::caseSensitive() const { return m_query.caseSensitive; }
bool DocumentFind::wholeWord() const { return m_query.wholeWord; }
bool DocumentFind::regex() const { return m_query.regex; }
TextQuery DocumentFind::query() const { return m_query; }
QString DocumentFind::error() const { return m_error; }
int DocumentFind::count() const { return int(m_matches.size()); }
bool DocumentFind::truncated() const { return m_truncated; }
bool DocumentFind::searching() const { return m_active && m_document && m_expression && !m_forQuery; }
int DocumentFind::current() const { return m_current; }
bool DocumentFind::canReplace() const { return m_canReplace; }
QList<SourceRange> DocumentFind::matches() const { return m_matches; }

QVariantList DocumentFind::ranges() const
{
    QVariantList ranges;
    ranges.reserve(m_matches.size());
    for (const SourceRange &range : m_matches)
        ranges.append(QVariantMap{{QStringLiteral("start"), range.start}, {QStringLiteral("length"), range.length}});
    return ranges;
}

void DocumentFind::setDocument(Document *document)
{
    if (document == m_document) return;
    for (const auto &connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    m_document = document;
    if (document) {
        m_connections.append(connect(document->buffer(), &SourceBuffer::sourceChanged, this, &DocumentFind::sourceChanged));
        m_connections.append(connect(document, &Document::changed, this, &DocumentFind::refreshCanReplace));
    }
    m_anchor = m_cursor = 0;
    m_pending = Pending::None;
    refreshCanReplace();
    emit documentChanged();
    // The matches were another text's.
    queryEdited();
}

void DocumentFind::setActive(bool active)
{
    if (active == m_active) return;
    m_active = active;
    emit activeChanged();
    // Closed, the bar keeps nothing and looks for nothing; open, it looks afresh.
    queryEdited();
}

void DocumentFind::setPattern(const QString &pattern)
{
    if (pattern == m_query.pattern) return;
    m_query.pattern = pattern;
    emit queryChanged();
    queryEdited();
}

void DocumentFind::setCaseSensitive(bool on)
{
    if (on == m_query.caseSensitive) return;
    m_query.caseSensitive = on;
    emit queryChanged();
    queryEdited();
}

void DocumentFind::setWholeWord(bool on)
{
    if (on == m_query.wholeWord) return;
    m_query.wholeWord = on;
    emit queryChanged();
    queryEdited();
}

void DocumentFind::setRegex(bool on)
{
    if (on == m_query.regex) return;
    m_query.regex = on;
    emit queryChanged();
    queryEdited();
}

void DocumentFind::queryEdited()
{
    // Whatever runs is for an older query, or another text.
    ++m_generation;
    if (m_cancel) m_cancel->store(true);
    m_timer.stop();
    m_expression = TextSearch::compile(m_query, &m_error);
    m_matches.clear();
    m_truncated = false;
    m_forQuery = false;
    // A move asked for under the old query is not one under this.
    if (!m_expression || !m_active || !m_document) m_pending = Pending::None;
    emit resultsChanged();
    refreshCurrent();
    if (m_active && m_document && m_expression) schedule(0);
}

void DocumentFind::schedule(int delayMs)
{
    m_timer.start(delayMs);
}

void DocumentFind::search()
{
    if (!m_active || !m_document || !m_expression) return;
    // One search runs at a time: the one running stops, and this follows it.
    if (m_running) {
        m_searchAgain = true;
        if (m_cancel) m_cancel->store(true);
        return;
    }
    m_running = true;
    m_searchAgain = false;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = m_generation;
    QtConcurrent::run(&m_pool, &DocumentFind::findJob, m_document->buffer()->snapshot(), *m_expression, m_cancel)
        .then(this, [this, generation](const Found &found) {
            m_running = false;
            const bool again = std::exchange(m_searchAgain, false);
            if (generation == m_generation && !again && m_document && found.revision == m_document->buffer()->revision()) {
                take(found);
                return;
            }
            // An edit since then asks for its own search once the text rests.
            if (again || generation != m_generation) search();
        });
}

DocumentFind::Found DocumentFind::findJob(QSharedPointer<const SourceSnapshot> snapshot, QRegularExpression expression,
                                          std::shared_ptr<std::atomic<bool>> cancel)
{
    Found found;
    found.revision = snapshot->revision;
    found.matches = TextSearch::findAll(snapshot->text, expression, TextSearch::MaxMatches, &found.truncated, cancel.get());
    return found;
}

void DocumentFind::take(const Found &found)
{
    m_matches = found.matches;
    m_truncated = found.truncated;
    m_forQuery = true;
    emit resultsChanged();
    refreshCurrent();
    runPending();
}

void DocumentFind::sourceChanged(Revision, SourceRange range, const QString &replacement)
{
    if (!m_active || !m_expression) return;
    // The matches the edit did not touch move with the text; those it did
    // go, until the text is looked in again.
    const int end = range.start + range.length;
    const int delta = int(replacement.size()) - range.length;
    QList<SourceRange> kept;
    kept.reserve(m_matches.size());
    for (const SourceRange &match : std::as_const(m_matches)) {
        const int matchEnd = match.start + match.length;
        if (matchEnd < range.start || (matchEnd == range.start && match.length > 0)) kept.append(match);
        else if (match.start >= end && !(match.length == 0 && match.start == range.start)) kept.append({match.start + delta, match.length});
    }
    const bool changed = kept.size() != m_matches.size();
    m_matches = std::move(kept);
    if (changed) emit resultsChanged();
    refreshCurrent();
    schedule(EditSettleMs);
}

void DocumentFind::setSelection(int anchor, int cursor)
{
    m_anchor = anchor;
    m_cursor = cursor;
    refreshCurrent();
}

void DocumentFind::refreshCurrent()
{
    const int start = std::min(m_anchor, m_cursor);
    const int length = std::abs(m_cursor - m_anchor);
    int found = -1;
    const auto first = std::lower_bound(m_matches.cbegin(), m_matches.cend(), start,
                                        [](const SourceRange &match, int at) { return match.start < at; });
    for (auto at = first; at != m_matches.cend() && at->start == start; ++at) {
        if (at->length == length) {
            found = int(at - m_matches.cbegin());
            break;
        }
    }
    if (found == m_current) return;
    m_current = found;
    emit currentChanged();
}

void DocumentFind::refreshCanReplace()
{
    const bool can = m_document && m_document->state() != Document::State::ReadOnly && !m_document->isTextless();
    if (can == m_canReplace) return;
    m_canReplace = can;
    emit canReplaceChanged();
}

bool DocumentFind::settled() const { return m_forQuery; }

void DocumentFind::next() { step(true); }
void DocumentFind::previous() { step(false); }

void DocumentFind::step(bool forward)
{
    if (!m_expression || !m_document) return;
    if (!settled()) {
        // Once the query's matches are found.
        m_pending = forward ? Pending::Next : Pending::Previous;
        return;
    }
    if (m_matches.isEmpty()) return;
    const int count = int(m_matches.size());
    const int start = std::min(m_anchor, m_cursor);
    const int end = std::max(m_anchor, m_cursor);
    int index;
    if (m_current >= 0) {
        index = (m_current + (forward ? 1 : -1) + count) % count;
    } else if (forward) {
        // The first match at or after the selection's end, or the first of all.
        const auto at = std::lower_bound(m_matches.cbegin(), m_matches.cend(), end,
                                         [](const SourceRange &match, int position) { return match.start < position; });
        index = at == m_matches.cend() ? 0 : int(at - m_matches.cbegin());
    } else {
        // The last match that starts before the selection, or the last of all.
        const auto at = std::lower_bound(m_matches.cbegin(), m_matches.cend(), start,
                                         [](const SourceRange &match, int position) { return match.start < position; });
        index = at == m_matches.cbegin() ? count - 1 : int(at - m_matches.cbegin()) - 1;
    }
    selectMatch(index);
}

void DocumentFind::seek()
{
    if (!m_expression || !m_document) return;
    if (!settled()) {
        m_pending = Pending::Seek;
        return;
    }
    if (m_matches.isEmpty()) return;
    const int start = std::min(m_anchor, m_cursor);
    const auto at = std::lower_bound(m_matches.cbegin(), m_matches.cend(), start,
                                     [](const SourceRange &match, int position) { return match.start < position; });
    selectMatch(at == m_matches.cend() ? 0 : int(at - m_matches.cbegin()));
}

void DocumentFind::selectMatch(int index)
{
    const SourceRange match = m_matches.at(index);
    m_anchor = match.start;
    m_cursor = match.start + match.length;
    refreshCurrent();
    emit selectRequested(m_anchor, m_cursor);
}

void DocumentFind::replace(const QString &replacement)
{
    if (!m_canReplace || !m_expression || !m_document) return;
    if (!settled()) {
        m_pending = Pending::Replace;
        m_pendingReplacement = replacement;
        return;
    }
    // A selection that is no match is moved to one first, which the next
    // Replace then replaces.
    if (m_current < 0) {
        step(true);
        return;
    }
    replaceCurrent(replacement);
}

void DocumentFind::replaceCurrent(const QString &replacement)
{
    SourceBuffer *buffer = m_document->buffer();
    const SourceRange range = m_matches.at(m_current);
    const int index = m_current;
    // The text there is matched again, for the groups a replacement names,
    // and to be sure it still matches after edits the matches moved with.
    const QRegularExpressionMatch match = TextSearch::matchAt(buffer->text(), *m_expression, range);
    if (!match.hasMatch()) {
        m_pending = Pending::Replace;
        m_pendingReplacement = replacement;
        m_forQuery = false;
        schedule(0);
        return;
    }
    const QString text = TextSearch::expand(replacement, match, m_query.regex);
    const auto outcome = buffer->replace(range, text, EditKind::Other);
    if (m_matches.isEmpty()) return;
    // The next match after what was written, which a match the replacement
    // made is not.
    int next;
    if (outcome == SourceBuffer::EditOutcome::Applied) {
        const int after = range.start + int(text.size());
        const auto at = std::lower_bound(m_matches.cbegin(), m_matches.cend(), after,
                                         [](const SourceRange &found, int position) { return found.start < position; });
        next = at == m_matches.cend() ? 0 : int(at - m_matches.cbegin());
    } else {
        next = (index + 1) % int(m_matches.size());
    }
    selectMatch(next);
}

void DocumentFind::replaceAll(const QString &replacement)
{
    if (!m_canReplace || !m_expression || !m_document || m_replacing) return;
    startReplaceAll(replacement);
}

void DocumentFind::startReplaceAll(const QString &replacement)
{
    m_replacing = true;
    const QPointer<Document> document = m_document;
    QtConcurrent::run(&m_pool, &DocumentFind::replaceJob, m_document->buffer()->snapshot(), *m_expression, replacement,
                      m_query.regex)
        .then(this, [this, document, replacement](const Replaced &replaced) {
            m_replacing = false;
            if (!document || document != m_document || !m_canReplace) return;
            SourceBuffer *buffer = document->buffer();
            // Written to meanwhile: the matches are found again in the newer text.
            if (replaced.revision != buffer->revision()) {
                startReplaceAll(replacement);
                return;
            }
            if (replaced.count > 0) buffer->replace(replaced.edit.range, replaced.edit.text, EditKind::Other);
            emit replacedAll(replaced.count);
        });
}

DocumentFind::Replaced DocumentFind::replaceJob(QSharedPointer<const SourceSnapshot> snapshot,
                                                QRegularExpression expression, QString replacement, bool regex)
{
    Replaced replaced;
    replaced.revision = snapshot->revision;
    const QString &text = snapshot->text;
    QList<TextSearch::Edit> edits;
    TextSearch::forEachMatch(text, expression, [&](const QRegularExpressionMatch &match) {
        edits.append({{int(match.capturedStart()), int(match.capturedLength())},
                      TextSearch::expand(replacement, match, regex)});
        return true;
    });
    replaced.count = int(edits.size());
    replaced.edit = TextSearch::combine(text, edits);
    return replaced;
}

void DocumentFind::runPending()
{
    switch (std::exchange(m_pending, Pending::None)) {
    case Pending::Next:
        step(true);
        break;
    case Pending::Previous:
        step(false);
        break;
    case Pending::Seek:
        seek();
        break;
    case Pending::Replace:
        replace(m_pendingReplacement);
        break;
    case Pending::None:
        break;
    }
}
