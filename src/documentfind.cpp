#include "documentfind.h"

#include "editorbinding.h"
#include "sourceboundaries.h"
#include "sourcebuffer.h"
#include "tablegrids.h"
#include "workerfailure.h"

#include <QFutureWatcher>
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
    cancelReplaceAll();
    m_pool.waitForDone();
}

Document *DocumentFind::document() const { return m_document; }
bool DocumentFind::active() const { return m_active; }
QString DocumentFind::pattern() const { return m_query.pattern; }
bool DocumentFind::caseSensitive() const { return m_query.caseSensitive; }
bool DocumentFind::wholeWord() const { return m_query.wholeWord; }
bool DocumentFind::regex() const { return m_query.regex; }
TextQuery DocumentFind::query() const { return m_query; }
QString DocumentFind::error() const { return m_error.isEmpty() ? m_replaceError : m_error; }
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
    cancelReplaceAll();
    for (const auto &connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    m_document = document;
    if (document) {
        m_connections.append(connect(document->buffer(), &SourceBuffer::sourceChanged, this, &DocumentFind::sourceChanged));
        m_connections.append(connect(document, &Document::changed, this, &DocumentFind::refreshCanReplace));
        m_connections.append(connect(document, &QObject::destroyed, this, &DocumentFind::refreshCanReplace));
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
    m_replaceError.clear();
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
        })
        .onFailed(this, [this](const QUnhandledException &failure) { failed(WorkerFailure::reason(failure.exception())); })
        .onFailed(this, [this] { failed(QString()); });
}

DocumentFind::Found DocumentFind::findJob(QSharedPointer<const SourceSnapshot> snapshot, QRegularExpression expression,
                                          std::shared_ptr<std::atomic<bool>> cancel)
{
    WorkerFailure::check(WorkerFailure::Job::Search);
    Found found;
    found.revision = snapshot->revision;
    found.matches = TextSearch::findAll(snapshot->text, expression, TextSearch::MaxMatches, &found.truncated, cancel.get());
    return found;
}

// A search that failed, as when the text ran it out of memory: it says
// why, as an error of the query does, and the next query searches again.
void DocumentFind::failed(const QString &reason)
{
    m_running = false;
    m_searchAgain = false;
    m_error = tr("The search stopped: %1").arg(reason.isEmpty() ? QStringLiteral("it could not be made") : reason);
    m_forQuery = true;
    emit resultsChanged();
}

void DocumentFind::setBinding(EditorBinding *binding)
{
    if (binding == m_binding) return;
    if (m_binding) disconnect(m_binding->tables(), nullptr, this, nullptr);
    m_binding = binding;
    // A table shown as a grid, or as its Markdown again, changes what is to
    // be found in it: the text is looked in again.
    if (binding)
        connect(binding->tables(), &TableGrids::gridsChanged, this, [this] {
            if (m_active && m_document && m_expression) queryEdited();
        });
    emit bindingChanged();
}

QList<SourceRange> DocumentFind::inCellText(const QList<SourceRange> &matches) const
{
    if (!m_binding || m_binding->tables()->grids().isEmpty()) return matches;
    QList<SourceRange> kept;
    kept.reserve(matches.size());
    for (const SourceRange &match : matches)
        if (m_binding->tables()->inCellText(match.start, match.start + match.length)) kept.append(match);
    return kept;
}

void DocumentFind::take(const Found &found)
{
    // A table's pipes and dashes are found nowhere in a grid.
    m_matches = inCellText(found.matches);
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
    if (!can) cancelReplaceAll();
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
    // In a grid, the cell's text is replaced and the table written tidy
    // around it; a replacement it cannot take is refused, and why said.
    SourceBuffer::EditOutcome outcome;
    const TableGrids::Adapted adapted =
        m_binding ? m_binding->tables()->adapt(range, text) : TableGrids::Adapted{};
    if (adapted.kind == TableGrids::Adapted::Refused) {
        outcome = SourceBuffer::EditOutcome::Rejected;
        emit m_binding->tables()->refused(adapted.reason);
    } else if (adapted.kind == TableGrids::Adapted::Changed) {
        outcome = buffer->replace(adapted.edits);
    } else {
        outcome = buffer->replace(range, text, EditKind::Other);
    }
    const bool rewritten = adapted.kind == TableGrids::Adapted::Changed && outcome == SourceBuffer::EditOutcome::Applied;
    // A table written anew all through took the matches in it with it: they
    // are found in it again at once, for the next to be among them.
    if (rewritten) findInTable(adapted.cursor);
    if (m_matches.isEmpty()) return;
    // The next match after what was written, which a match the replacement
    // made is not.
    int next;
    if (outcome == SourceBuffer::EditOutcome::Applied) {
        const int after = rewritten ? adapted.cursor : range.start + int(text.size());
        const auto at = std::lower_bound(m_matches.cbegin(), m_matches.cend(), after,
                                         [](const SourceRange &found, int position) { return found.start < position; });
        next = at == m_matches.cend() ? 0 : int(at - m_matches.cbegin());
    } else {
        next = (index + 1) % int(m_matches.size());
    }
    selectMatch(next);
}

void DocumentFind::findInTable(int position)
{
    TableGrid *grid = m_binding ? m_binding->tables()->gridAt(position) : nullptr;
    if (!grid || !m_expression || !m_document) return;
    const int start = grid->start(), end = grid->end();
    const QString text = m_document->buffer()->text();
    QList<SourceRange> found;
    QRegularExpressionMatchIterator matches = m_expression->globalMatch(text, start);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const SourceRange range{int(match.capturedStart()), int(match.capturedLength())};
        if (range.start > end) break;
        if (range.start + range.length > end || !sourceGraphemeRange(text, range)) continue;
        found.append(range);
    }
    found = inCellText(found);
    // The table's matches in place of those around it that the edit left.
    QList<SourceRange> merged;
    merged.reserve(m_matches.size() + found.size());
    for (const SourceRange &match : std::as_const(m_matches))
        if (match.start < start) merged.append(match);
    merged += found;
    for (const SourceRange &match : std::as_const(m_matches))
        if (match.start > end) merged.append(match);
    m_matches = std::move(merged);
    emit resultsChanged();
    refreshCurrent();
}

void DocumentFind::replaceAll(const QString &replacement)
{
    if (!m_canReplace || !m_expression || !m_document || m_replaceRequest) return;
    const auto request = std::make_shared<ReplaceRequest>(m_document, *m_expression, replacement, m_query.regex);
    m_replaceRequest = request;
    startReplaceAll(request);
    if (!m_replaceError.isEmpty()) {
        m_replaceError.clear();
        emit resultsChanged();
    }
}

bool DocumentFind::replaceInGrids() const
{
    return m_binding && m_binding->document() == m_document && !m_binding->tables()->grids().isEmpty();
}

void DocumentFind::cancelReplaceAll()
{
    if (!m_replaceRequest) return;
    m_replaceRequest->canceled.store(true, std::memory_order_relaxed);
    m_replaceRequest.reset();
}

void DocumentFind::startReplaceAll(const std::shared_ptr<const ReplaceRequest> &request)
{
    const bool each = replaceInGrids();
    auto *watcher = new QFutureWatcher<Replaced>(this);
    // Deliver even a fast result after the current editor event has finished.
    connect(watcher, &QFutureWatcher<Replaced>::finished, this, [this, watcher, request, each] {
        watcher->deleteLater();
        if (m_replaceRequest != request) return;
        try {
            takeReplacements(request, watcher->result(), each);
        } catch (...) {
            if (m_replaceRequest != request) return;
            m_replaceRequest.reset();
            m_replaceError = tr("Replace All stopped: %1").arg(WorkerFailure::reason(std::current_exception()));
            emit resultsChanged();
        }
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, &DocumentFind::replaceJob, request->document->buffer()->snapshot(),
                                         request, each));
}

void DocumentFind::takeReplacements(const std::shared_ptr<const ReplaceRequest> &request, const Replaced &result,
                                    bool each)
{
    if (!request->document || request->document != m_document || !m_canReplace) {
        cancelReplaceAll();
        return;
    }
    SourceBuffer *buffer = request->document->buffer();
    if (result.revision != buffer->revision() || each != replaceInGrids()) {
        startReplaceAll(request);
        return;
    }
    int count = result.count;
    int unchanged = 0;
    QList<SourceEdit> edits;
    QString refusal;
    if (each) {
        TableGrids *tables = m_binding->tables();
        const QString source = buffer->text();
        // Only cell text is searchable in a grid. Count matches that
        // already have the requested text without rewriting their cells.
        for (const TextSearch::Edit &edit : result.edits) {
            if (!tables->inCellText(edit.range.start, edit.range.start + edit.range.length)) continue;
            if (QStringView(source).mid(edit.range.start, edit.range.length) == edit.text) ++unchanged;
            else edits.append({edit.range, edit.text});
        }
        int left = 0;
        if (!edits.isEmpty()) {
            TableGrids::Adapted adapted = tables->adapt(edits, 0, 0, &left);
            count = unchanged + int(edits.size()) - left;
            if (adapted.kind != TableGrids::Adapted::Pass) edits = std::move(adapted.edits);
            refusal = std::move(adapted.reason);
        } else {
            count = unchanged;
        }
    } else if (count > 0) {
        edits.append({result.edit.range, result.edit.text});
    }
    if (!edits.isEmpty() && buffer->replaceAtRevision(result.revision, edits) == SourceBuffer::EditOutcome::Rejected) {
        count = unchanged;
        refusal = tr("The replacements could not be written.");
    }
    if (m_replaceRequest != request) return;
    m_replaceRequest.reset();
    if (!refusal.isEmpty() && m_binding) emit m_binding->tables()->refused(refusal);
    emit replacedAll(count);
}

DocumentFind::Replaced DocumentFind::replaceJob(QSharedPointer<const SourceSnapshot> snapshot,
                                                const std::shared_ptr<const ReplaceRequest> &request,
                                                bool each)
{
    Replaced replaced;
    replaced.revision = snapshot->revision;
    if (request->canceled.load(std::memory_order_relaxed)) return replaced;
    WorkerFailure::check(WorkerFailure::Job::Search);
    const QString &text = snapshot->text;
    QList<TextSearch::Edit> edits;
    TextSearch::forEachMatch(text, request->expression,
                            [&](const QRegularExpressionMatch &match) {
                                edits.append({{int(match.capturedStart()), int(match.capturedLength())},
                                              TextSearch::expand(request->replacement, match, request->regex)});
                                return true;
                            },
                            &request->canceled);
    if (request->canceled.load(std::memory_order_relaxed)) return replaced;
    replaced.count = int(edits.size());
    if (each) replaced.edits = std::move(edits);
    else replaced.edit = TextSearch::combine(text, edits);
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
