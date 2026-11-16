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
    enum class EditOutcome { Rejected, Unchanged, Applied };
    EditOutcome replace(SourceRange range, const QString &replacement, EditKind kind = EditKind::Other);
    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
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
    bool joinsLastStep(const Transaction &edit, EditKind kind, bool lineBreak, qint64 now) const;
    void apply(SourceRange range, const QString &replacement);

    QString m_text;
    Revision m_revision = 0;
    QVector<Step> m_history;
    qsizetype m_historyPosition = 0;
    // The history position on disk, or -1 once an edit discarded that position.
    qsizetype m_savedPosition = 0;
    // An undo or redo ends grouping, since the last step is no longer the latest edit.
    bool m_canJoin = false;
    std::function<qint64()> m_clock;
};
