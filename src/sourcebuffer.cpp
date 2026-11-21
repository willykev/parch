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

bool SourceBuffer::joinsLastStep(const Transaction &edit, EditKind kind, bool lineBreak, qint64 now, quint64 run) const
{
    if (run != 0) {
        // A run's edits join wherever they are, and line breaks and all.
        if (kind == EditKind::Other || !m_canJoin || m_history.isEmpty()) return false;
        if (m_historyPosition == m_savedPosition) return false;
        const auto &step = m_history.last();
        return step.run == run && step.kind == kind && now - step.lastTime < groupingWindowMs;
    }
    if (kind == EditKind::Other || lineBreak || !m_canJoin || m_history.isEmpty()) return false;
    // An edit at the saved position starts a step, or the saved text could not be told from an edited one.
    if (m_historyPosition == m_savedPosition) return false;
    const auto &step = m_history.last();
    // A run's step is its own: typing elsewhere, as in another view, starts another.
    if (step.run != 0) return false;
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
    if (m_replaying) {
        wait({{range, replacement}}, kind, false);
        return EditOutcome::Deferred;
    }
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
    if (m_replaying) {
        wait(edits, EditKind::Other, true);
        return EditOutcome::Deferred;
    }
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

SourceBuffer::EditOutcome SourceBuffer::replaceAtRevision(Revision revision, const QList<SourceEdit> &edits)
{
    if (m_replaying || m_stepDepth > 0 || revision != m_revision) return EditOutcome::Rejected;
    return replace(edits);
}

SourceBuffer::EditOutcome SourceBuffer::replace(const QList<SourceEdit> &edits, EditKind kind, quint64 run)
{
    if (run == 0 || kind == EditKind::Other) return replace(edits);
    int end = 0;
    bool changes = false;
    for (const SourceEdit &edit : edits) {
        if (edit.range.start < end || !sourceGraphemeRange(m_text, edit.range)) return EditOutcome::Rejected;
        end = edit.range.start + edit.range.length;
        changes = changes || QStringView(m_text).mid(edit.range.start, edit.range.length) != edit.replacement;
    }
    if (!changes) return EditOutcome::Unchanged;
    if (m_replaying) {
        wait(edits, kind, true, run);
        return EditOutcome::Deferred;
    }
    // The first edit joins the run's step or starts it; the rest of these join the first.
    bool first = true;
    for (qsizetype i = edits.size(); i-- > 0;) {
        const SourceEdit &edit = edits.at(i);
        const QString removed = m_text.mid(edit.range.start, edit.range.length);
        if (removed == edit.replacement) continue;
        if (first) {
            record(edit.range, removed, edit.replacement, kind, run);
            first = false;
            beginStep();
            m_stepStarted = true;
        } else {
            record(edit.range, removed, edit.replacement, kind, run);
        }
    }
    if (!first) endStep();
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

void SourceBuffer::record(SourceRange range, const QString &removed, const QString &replacement, EditKind kind,
                          quint64 run)
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
    } else if (m_stepDepth == 0 && joinsLastStep(edit, kind, lineBreak, now, run)) {
        auto &step = m_history.last();
        step.lastTime = now;
        step.hasLineBreak = step.hasLineBreak || lineBreak;
        // An edit of a run over what the run's last edit wrote is made one
        // with it: removed, what the two removed from the text before them;
        // inserted, what they leave.
        Transaction &last = step.edits.last();
        const qsizetype end1 = last.start + last.inserted.size();
        const qsizetype end2 = edit.start + edit.removed.size();
        if (run != 0 && edit.start <= end1 && end2 >= last.start) {
            const auto part = [](const QString &text, qsizetype from, qsizetype to) {
                from = qBound<qsizetype>(0, from, text.size());
                to = qBound<qsizetype>(from, to, text.size());
                return text.mid(from, to - from);
            };
            const QString removedBoth = part(edit.removed, 0, last.start - edit.start) + last.removed
                                        + part(edit.removed, end1 - edit.start, edit.removed.size());
            const QString insertedBoth = part(last.inserted, 0, edit.start - last.start) + edit.inserted
                                         + part(last.inserted, end2 - last.start, last.inserted.size());
            last = {qMin(last.start, edit.start), removedBoth, insertedBoth};
        } else {
            step.edits.append(edit);
        }
    } else {
        // A step begun with beginStep() is never joined by typing afterwards.
        m_history.append({{edit}, m_stepDepth > 0 && run == 0 ? EditKind::Other : kind, now, lineBreak, run});
        ++m_historyPosition;
        m_stepStarted = m_stepDepth > 0;
    }
    m_canJoin = true;
    apply(range, replacement);
    if (modified() != wasModified) emit modifiedChanged();
}

void SourceBuffer::undo()
{
    if (m_historyPosition == 0 || m_replaying) return;
    replay(true);
}

void SourceBuffer::redo()
{
    if (m_historyPosition == m_history.size() || m_replaying) return;
    replay(false);
}

void SourceBuffer::replay(bool backwards)
{
    const bool wasModified = modified();
    m_canJoin = false;
    m_stepStarted = false;
    // A copy, and every edit asked for meanwhile waits for the step to be
    // whole: a listener that edits as each change lands must neither take
    // the step out of history nor move where its later changes go.
    const QVector<Transaction> edits = m_history.at(backwards ? --m_historyPosition : m_historyPosition++).edits;
    m_replaying = true;
    if (backwards) {
        // Recorded edits replay exactly, newest first: insertion can have joined adjacent graphemes.
        for (qsizetype i = edits.size(); i-- > 0;) {
            const auto &transaction = edits.at(i);
            apply({transaction.start, int(transaction.inserted.size())}, transaction.removed);
        }
    } else {
        for (const auto &transaction : edits)
            apply({transaction.start, int(transaction.removed.size())}, transaction.inserted);
    }
    m_replaying = false;
    if (modified() != wasModified) emit modifiedChanged();
    while (!m_waiting.isEmpty()) {
        const Waiting waiting = m_waiting.takeFirst();
        if (waiting.dropped) continue;
        if (waiting.together) replace(waiting.edits, waiting.kind, waiting.run);
        else replace(waiting.edits.first().range, waiting.edits.first().replacement, waiting.kind);
    }
}

void SourceBuffer::wait(const QList<SourceEdit> &edits, EditKind kind, bool together, quint64 run)
{
    m_waiting.append({edits, kind, together, false, run});
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

void SourceBuffer::forgetUndo()
{
    if (m_replaying || m_stepDepth > 0) return;
    const bool wasModified = modified();
    const bool saved = !wasModified;
    m_history.clear();
    m_historyPosition = 0;
    m_savedPosition = saved ? 0 : -1;
    m_canJoin = false;
    if (modified() != wasModified) emit modifiedChanged();
}

void SourceBuffer::apply(SourceRange range, const QString &replacement)
{
    // Edits waiting for a step address the text as it was when they were
    // asked for, so each change made since carries them along: one before
    // them moves them, one after leaves them, and one that touches their
    // text drops them.
    const int changeEnd = range.start + range.length;
    const int shift = int(replacement.size()) - range.length;
    for (Waiting &waiting : m_waiting) {
        for (SourceEdit &edit : waiting.edits) {
            const int end = edit.range.start + edit.range.length;
            if (end <= range.start) continue;
            if (edit.range.start >= changeEnd) edit.range.start += shift;
            else waiting.dropped = true;
        }
    }
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
