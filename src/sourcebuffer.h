#pragma once

#include "sourcetypes.h"

#include <QObject>
#include <QSharedPointer>
#include <QVector>
#include <functional>

// How an edit was made, which decides whether it joins the previous undo step.
enum class EditKind { Typing, Deleting, Other };

// The single authoritative text of a document. It owns the text, a monotonic
// revision, edit transactions and undo; views and files are built on it.
class SourceBuffer : public QObject {
    Q_OBJECT
    Q_PROPERTY(int length READ length NOTIFY lengthChanged)
public:
    explicit SourceBuffer(QString initial = {}, QObject *parent = nullptr);
    // Also for QML, which reads the whole text only to show it elsewhere,
    // as a dialog that compares or previews it does.
    Q_INVOKABLE QString text() const;
    int length() const;
    Revision revision() const;
    // Deferred: asked for by a listener while undo or redo applies a step,
    // and applied, as a step of its own, once that step is whole. Its range
    // addresses the text as it was when asked for and is carried over every
    // change made after that; an edit whose text one of those changes
    // touched is dropped. Undo and redo asked for meanwhile do nothing.
    enum class EditOutcome { Rejected, Unchanged, Applied, Deferred };
    EditOutcome replace(SourceRange range, const QString &replacement, EditKind kind = EditKind::Other);
    // Several replacements as one undo step, as a command that edits in more
    // than one place makes them. Each range addresses the text as it is now;
    // they are sorted and do not overlap. Either all are applied or, should
    // one of them split a grapheme or the ranges be out of order, none.
    EditOutcome replace(const QList<SourceEdit> &edits);
    // Every edit from beginStep() to the matching endStep() undoes and redoes
    // as one step, whatever its kind; steps begun inside another join it.
    void beginStep();
    void endStep();
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    // How many steps undo can take back now: a mark for joinSince().
    qsizetype undoDepth() const;
    // Makes every step taken since undoDepth() returned `depth` one step, as
    // a Vim command or a stay in Insert mode is one change, and lets no later
    // edit join it. Nothing is joined once undo has gone back past `depth`.
    // A save among the joined steps no longer matches a step of its own, so
    // the text then counts as modified.
    void joinSince(qsizetype depth);
    // The current history position is what is on disk.
    void markSaved();
    // True when the history position differs from the saved one.
    bool modified() const;
    QSharedPointer<const SourceSnapshot> snapshot() const;
    // Counts on from a revision an earlier buffer of the same document
    // reached, as one restored after a restart does, so that revisions
    // recorded before and after the restart compare in the order of the
    // edits. Only for a buffer whose revision nothing has read yet.
    void continueAfter(Revision revision);
    // Replaces the monotonic clock so that tests need not sleep.
    void setClock(std::function<qint64()> milliseconds);

signals:
    // The range addresses the source before this mutation; the revision is after it.
    void sourceChanged(Revision revision, SourceRange range, const QString &replacement);
    void lengthChanged();
    void modifiedChanged();

private:
    struct Transaction {
        int start;
        QString removed;
        QString inserted;
    };
    // One undo step: the transactions that undo and redo together.
    struct Step {
        QVector<Transaction> edits;
        EditKind kind;
        qint64 lastTime;
        bool hasLineBreak;
    };
    // An edit asked for while a step was being applied, waiting for it to end.
    struct Waiting {
        QList<SourceEdit> edits;
        EditKind kind;
        // Asked for as several replacements at once, which apply together.
        bool together;
        // Text one of them addresses was changed after it was asked for.
        bool dropped = false;
    };
    bool joinsLastStep(const Transaction &edit, EditKind kind, bool lineBreak, qint64 now) const;
    // Records an edit already checked and applies it.
    void record(SourceRange range, const QString &removed, const QString &replacement, EditKind kind);
    void apply(SourceRange range, const QString &replacement);
    // Undoes or redoes a step, and then applies the edits asked for while it did.
    void replay(bool backwards);
    void wait(const QList<SourceEdit> &edits, EditKind kind, bool together);

    QString m_text;
    Revision m_revision = 0;
    QVector<Step> m_history;
    qsizetype m_historyPosition = 0;
    // The history position on disk, or -1 once an edit discarded that position.
    qsizetype m_savedPosition = 0;
    // An undo or redo ends grouping, since the last step is no longer the latest edit.
    bool m_canJoin = false;
    // How deep beginStep() calls are, and whether the step they make has its first edit yet.
    int m_stepDepth = 0;
    bool m_stepStarted = false;
    // Undo or redo is applying a step, and the edits asked for meanwhile.
    bool m_replaying = false;
    QList<Waiting> m_waiting;
    std::function<qint64()> m_clock;
};
