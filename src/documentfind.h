#pragma once

#include "sourcetypes.h"
#include "textsearch.h"
// The document is a property QML reads, so its type must be complete here.
#include "document.h"
#include "editorbinding.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <atomic>
#include <memory>
#include <optional>

// Find and replace in one document, for a window's find bar. Every match of
// the query is found off the UI thread, in a snapshot of the text, and taken
// only while the text is still at the snapshot's revision; an edit meanwhile
// moves the matches it does not touch, drops those it does, and looks again
// a little later. Nothing is looked for while the bar is not active, so that
// typing pays for nothing.
//
// The view says where its selection is, and the find says which match that
// is; moving to a match asks the view to select it. A move asked for while
// the matches of a new query are still being found waits for them. Replace
// and Replace all edit the document's SourceBuffer, each as one undo step.
// Every match's range stays readable, so that a view can mark them all.
class DocumentFind : public QObject {
    Q_OBJECT
    Q_PROPERTY(Document *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(QString pattern READ pattern WRITE setPattern NOTIFY queryChanged)
    Q_PROPERTY(bool caseSensitive READ caseSensitive WRITE setCaseSensitive NOTIFY queryChanged)
    Q_PROPERTY(bool wholeWord READ wholeWord WRITE setWholeWord NOTIFY queryChanged)
    Q_PROPERTY(bool regex READ regex WRITE setRegex NOTIFY queryChanged)
    // Why the pattern cannot be looked for, when it is not a valid regular
    // expression; empty otherwise.
    Q_PROPERTY(QString error READ error NOTIFY resultsChanged)
    Q_PROPERTY(int count READ count NOTIFY resultsChanged)
    // There are more matches than MaxMatches, which are all that are kept.
    Q_PROPERTY(bool truncated READ truncated NOTIFY resultsChanged)
    // The matches of the query as it is now are still being found.
    Q_PROPERTY(bool searching READ searching NOTIFY resultsChanged)
    // Which match the view's selection is, counted from 0; -1 for none.
    Q_PROPERTY(int current READ current NOTIFY currentChanged)
    // The document takes edits: neither read-only nor without text of its own.
    Q_PROPERTY(bool canReplace READ canReplace NOTIFY canReplaceChanged)
    // The window's text view, whose tables shown as grids find only their
    // cells' text, and are written tidy when a match in them is replaced.
    Q_PROPERTY(EditorBinding *binding READ binding WRITE setBinding NOTIFY bindingChanged)

public:
    // How long the text must rest after an edit before it is looked in again.
    static constexpr int EditSettleMs = 150;

    explicit DocumentFind(QObject *parent = nullptr);
    ~DocumentFind() override;

    Document *document() const;
    void setDocument(Document *document);
    bool active() const;
    void setActive(bool active);
    QString pattern() const;
    void setPattern(const QString &pattern);
    bool caseSensitive() const;
    void setCaseSensitive(bool on);
    bool wholeWord() const;
    void setWholeWord(bool on);
    bool regex() const;
    void setRegex(bool on);
    TextQuery query() const;
    QString error() const;
    int count() const;
    bool truncated() const;
    bool searching() const;
    int current() const;
    bool canReplace() const;
    EditorBinding *binding() const { return m_binding; }
    void setBinding(EditorBinding *binding);
    // Every match, in order, in the document's source as it is now.
    QList<SourceRange> matches() const;
    // The same for QML, as {start, length}.
    Q_INVOKABLE QVariantList ranges() const;

    // Where the view's selection is, as source offsets.
    Q_INVOKABLE void setSelection(int anchor, int cursor);
    // Selects the next match after the selection, or the one before it,
    // round the ends.
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    // Selects the first match at or after the start of the selection, as
    // the find bar does while the pattern is typed.
    Q_INVOKABLE void seek();
    // Replaces the match the selection is, and selects the next; a selection
    // that is no match moves to the next match first, as Find next does.
    Q_INVOKABLE void replace(const QString &replacement);
    // Replaces every match, as one edit that one undo reverts. The matches
    // are found again off the UI thread first, in the text as it is then.
    Q_INVOKABLE void replaceAll(const QString &replacement);

signals:
    void documentChanged();
    void activeChanged();
    void queryChanged();
    void resultsChanged();
    void currentChanged();
    void canReplaceChanged();
    void bindingChanged();
    // The view is to select from anchor to cursor, source offsets, and show it.
    void selectRequested(int anchor, int cursor);
    // Replace all replaced this many matches.
    void replacedAll(int count);

private:
    enum class Pending { None, Next, Previous, Seek, Replace };
    struct Found {
        QList<SourceRange> matches;
        bool truncated = false;
        Revision revision = 0;
    };
    struct Replaced {
        TextSearch::Edit edit;
        QList<TextSearch::Edit> edits;
        int count = 0;
        Revision revision = 0;
    };
    struct ReplaceRequest {
        QPointer<Document> document;
        QRegularExpression expression;
        QString replacement;
        bool regex;
        mutable std::atomic<bool> canceled{false};
    };

    static Found findJob(QSharedPointer<const SourceSnapshot> snapshot, QRegularExpression expression,
                         std::shared_ptr<std::atomic<bool>> cancel);
    // With grids, the worker keeps the individual matches for cell editing.
    static Replaced replaceJob(QSharedPointer<const SourceSnapshot> snapshot,
                               const std::shared_ptr<const ReplaceRequest> &request, bool each);

    void queryEdited();
    void schedule(int delayMs);
    void search();
    void take(const Found &found);
    // A search that failed: it says why, and the next query searches again.
    void failed(const QString &reason);
    void sourceChanged(Revision revision, SourceRange range, const QString &replacement);
    void refreshCurrent();
    void refreshCanReplace();
    // Finds the matches again in the grid's table that holds a position,
    // in place of those the edit that wrote it anew took with it.
    void findInTable(int position);
    void runPending();
    void step(bool forward);
    void selectMatch(int index);
    void replaceCurrent(const QString &replacement);
    bool replaceInGrids() const;
    void cancelReplaceAll();
    void startReplaceAll(const std::shared_ptr<const ReplaceRequest> &request);
    void takeReplacements(const std::shared_ptr<const ReplaceRequest> &request, const Replaced &result, bool each);
    // The matches are those of the query as it is now, in the text as it is
    // now or moved along with its edits since.
    bool settled() const;

    // The matches a grid of the view shows as no text of a cell, which are left out.
    QList<SourceRange> inCellText(const QList<SourceRange> &matches) const;

    QPointer<Document> m_document;
    QPointer<EditorBinding> m_binding;
    QList<QMetaObject::Connection> m_connections;
    bool m_active = false;
    TextQuery m_query;
    std::optional<QRegularExpression> m_expression;
    QString m_error;
    QList<SourceRange> m_matches;
    bool m_truncated = false;
    // The matches belong to the query as it is now, though the text may have
    // moved on since they were found.
    bool m_forQuery = false;
    int m_current = -1;
    int m_anchor = 0;
    int m_cursor = 0;
    bool m_canReplace = false;
    Pending m_pending = Pending::None;
    QString m_pendingReplacement;
    QTimer m_timer;
    // One search at a time; a newer one cancels the one that runs.
    quint64 m_generation = 0;
    bool m_running = false;
    bool m_searchAgain = false;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    std::shared_ptr<const ReplaceRequest> m_replaceRequest;
    QString m_replaceError;
    QThreadPool m_pool;
};
