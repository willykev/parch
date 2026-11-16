#include "sourcebuffer.h"
#include "sourceboundaries.h"
#include <QElapsedTimer>
#include <algorithm>
#include <memory>
#include <utility>

namespace {
// A pause this long between keystrokes starts a new undo step.
constexpr qint64 groupingWindowMs = 2000;

bool hasLineBreak(const QString &text)
{
    return text.contains(u'\n') || text.contains(u'\r') || text.contains(QChar(0x2029));
}
}

SourceBuffer::SourceBuffer(QString initial, QObject *parent)
    : QObject(parent), m_text(std::move(initial))
{
    // Monotonic time, so that a wall-clock change cannot merge or split steps.
    auto timer = std::make_shared<QElapsedTimer>();
    timer->start();
    m_clock = [timer] { return timer->elapsed(); };
}

QString SourceBuffer::text() const { return m_text; }
int SourceBuffer::length() const { return int(m_text.size()); }
Revision SourceBuffer::revision() const { return m_revision; }
bool SourceBuffer::canUndo() const { return m_historyPosition > 0; }
bool SourceBuffer::canRedo() const { return m_historyPosition < m_history.size(); }
bool SourceBuffer::modified() const { return m_historyPosition != m_savedPosition; }

void SourceBuffer::setClock(std::function<qint64()> milliseconds)
{
    m_clock = std::move(milliseconds);
}

bool SourceBuffer::joinsLastStep(const Transaction &edit, EditKind kind, bool lineBreak, qint64 now) const
{
    if (kind == EditKind::Other || lineBreak || !m_canJoin || m_history.isEmpty()) return false;
    // An edit at the saved position starts a step, or the saved text could not be told from an edited one.
    if (m_historyPosition == m_savedPosition) return false;
    const auto &step = m_history.last();
    if (step.kind != kind || step.hasLineBreak) return false;
    if (now - step.lastTime >= groupingWindowMs) return false;
    const auto &last = step.edits.last();
    if (kind == EditKind::Typing)
        return edit.removed.isEmpty() && last.removed.isEmpty() && edit.start == last.start + last.inserted.size();
    if (!edit.inserted.isEmpty() || !last.inserted.isEmpty()) return false;
    // Backspace removes just before the previous range; Delete removes at the same start.
    return edit.start + edit.removed.size() == last.start || edit.start == last.start;
}

SourceBuffer::EditOutcome SourceBuffer::replace(SourceRange range, const QString &replacement, EditKind kind)
{
    if (!sourceGraphemeRange(m_text, range)) return EditOutcome::Rejected;
    const auto removed = m_text.mid(range.start, range.length);
    if (removed == replacement) return EditOutcome::Unchanged;
    record(range, removed, replacement, kind);
    return EditOutcome::Applied;
}

SourceBuffer::EditOutcome SourceBuffer::replace(const QList<SourceEdit> &edits)
{
    // Every range is checked against the text as it is, before anything
    // changes, so that the edits apply all together or not at all.
    int end = 0;
    bool changes = false;
    for (const SourceEdit &edit : edits) {
        if (edit.range.start < end || !sourceGraphemeRange(m_text, edit.range)) return EditOutcome::Rejected;
        end = edit.range.start + edit.range.length;
        changes = changes || QStringView(m_text).mid(edit.range.start, edit.range.length) != edit.replacement;
    }
    if (!changes) return EditOutcome::Unchanged;
    // From the last to the first, so that each range still addresses the
    // text it was given for.
    beginStep();
    for (qsizetype i = edits.size(); i-- > 0;) {
        const SourceEdit &edit = edits.at(i);
        const QString removed = m_text.mid(edit.range.start, edit.range.length);
        if (removed != edit.replacement) record(edit.range, removed, edit.replacement, EditKind::Other);
    }
    endStep();
    return EditOutcome::Applied;
}

void SourceBuffer::beginStep()
{
    if (m_stepDepth++ == 0) m_stepStarted = false;
}

void SourceBuffer::endStep()
{
    if (m_stepDepth > 0 && --m_stepDepth == 0) m_stepStarted = false;
}

void SourceBuffer::record(SourceRange range, const QString &removed, const QString &replacement, EditKind kind)
{
    const bool wasModified = modified();
    const Transaction edit{range.start, removed, replacement};
    const bool lineBreak = hasLineBreak(removed) || hasLineBreak(replacement);
    const auto now = m_clock();

    if (m_historyPosition < m_history.size()) {
        m_history.resize(m_historyPosition);
        // The saved text was among the discarded redo steps and can no longer be reached.
        if (m_savedPosition > m_historyPosition) m_savedPosition = -1;
    }
    if (m_stepDepth > 0 && m_stepStarted) {
        // Inside a step that has its first edit already.
        auto &step = m_history.last();
        step.edits.append(edit);
        step.lastTime = now;
        step.hasLineBreak = step.hasLineBreak || lineBreak;
    } else if (m_stepDepth == 0 && joinsLastStep(edit, kind, lineBreak, now)) {
        auto &step = m_history.last();
        step.edits.append(edit);
        step.lastTime = now;
    } else {
        // A step begun with beginStep() is never joined by typing afterwards.
        m_history.append({{edit}, m_stepDepth > 0 ? EditKind::Other : kind, now, lineBreak});
        ++m_historyPosition;
        m_stepStarted = m_stepDepth > 0;
    }
    m_canJoin = true;
    apply(range, replacement);
    if (modified() != wasModified) emit modifiedChanged();
}

void SourceBuffer::undo()
{
    if (m_historyPosition == 0) return;
    const bool wasModified = modified();
    m_canJoin = false;
    m_stepStarted = false;
    const auto &step = m_history.at(--m_historyPosition);
    // Replay recorded edits exactly, newest first: insertion can have joined adjacent graphemes.
    for (qsizetype i = step.edits.size(); i-- > 0;) {
        const auto &transaction = step.edits.at(i);
        apply({transaction.start, int(transaction.inserted.size())}, transaction.removed);
    }
    if (modified() != wasModified) emit modifiedChanged();
}

void SourceBuffer::redo()
{
    if (m_historyPosition == m_history.size()) return;
    const bool wasModified = modified();
    m_canJoin = false;
    m_stepStarted = false;
    const auto &step = m_history.at(m_historyPosition++);
    for (const auto &transaction : step.edits)
        apply({transaction.start, int(transaction.removed.size())}, transaction.inserted);
    if (modified() != wasModified) emit modifiedChanged();
}

qsizetype SourceBuffer::undoDepth() const { return m_historyPosition; }

void SourceBuffer::joinSince(qsizetype depth)
{
    // Whatever is joined, the next edit starts a step of its own.
    m_canJoin = false;
    if (depth < 0 || m_historyPosition - depth < 1) return;
    const bool wasModified = modified();
    Step &joined = m_history[depth];
    joined.kind = EditKind::Other;
    const qsizetype removed = m_historyPosition - depth - 1;
    for (qsizetype i = depth + 1; i < m_historyPosition; ++i) {
        const Step &step = m_history.at(i);
        joined.edits += step.edits;
        joined.lastTime = step.lastTime;
        joined.hasLineBreak = joined.hasLineBreak || step.hasLineBreak;
    }
    // Redo steps after the joined ones stay after it.
    m_history.remove(depth + 1, removed);
    if (m_savedPosition > depth && m_savedPosition < m_historyPosition) m_savedPosition = -1;
    else if (m_savedPosition >= m_historyPosition) m_savedPosition -= removed;
    m_historyPosition = depth + 1;
    if (modified() != wasModified) emit modifiedChanged();
}

void SourceBuffer::markSaved()
{
    const bool wasModified = modified();
    m_savedPosition = m_historyPosition;
    if (modified() != wasModified) emit modifiedChanged();
}

void SourceBuffer::apply(SourceRange range, const QString &replacement)
{
    m_text.replace(range.start, range.length, replacement);
    ++m_revision;
    emit sourceChanged(m_revision, range, replacement);
    if (replacement.size() != range.length) emit lengthChanged();
}

QSharedPointer<const SourceSnapshot> SourceBuffer::snapshot() const
{
    return QSharedPointer<const SourceSnapshot>::create(SourceSnapshot{revision(), m_text});
}

void SourceBuffer::continueAfter(Revision revision)
{
    m_revision = std::max(m_revision, revision);
}
