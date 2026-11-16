#pragma once

#include "document.h"

#include <QFutureWatcher>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringView>
#include <QThreadPool>
#include <QTimer>

// The words of a document, or of the selection when there is one, and how
// long they take to read at WordsPerMinute. What is counted is the text a
// reader sees, as the Markdown engine gives it (md::Document::plainText):
// link destinations, task boxes, comments, front matter, raw markup and
// Markdown punctuation are no words; code and notes are read. A selection
// counts what of that text is written within it. Words are found by
// Unicode's word boundaries (QTextBoundaryFinder), so letters joined by
// apostrophes are one. Chinese and Japanese, written without spaces, count
// each Han, Hiragana and Katakana character as a word.
//
// Edits are counted a moment after typing pauses. A short document is
// counted at once; a long one on a worker thread of its own, from a copy of
// the text, and its count is shown only while the text and selection are
// still those it was made for, so a stale count never replaces a newer one.
class WordCounter : public QObject {
    Q_OBJECT
    Q_PROPERTY(Document *document READ document WRITE setDocument NOTIFY documentChanged)
    // The selection, as source offsets in either order; equal for none.
    Q_PROPERTY(int selectionStart READ selectionStart WRITE setSelectionStart NOTIFY selectionChanged)
    Q_PROPERTY(int selectionEnd READ selectionEnd WRITE setSelectionEnd NOTIFY selectionChanged)
    Q_PROPERTY(int words READ words NOTIFY counted)
    Q_PROPERTY(int minutes READ minutes NOTIFY counted)
    // The count is of the selection rather than the whole document.
    Q_PROPERTY(bool ofSelection READ ofSelection NOTIFY counted)
    // In a few words for the status line, and in full for assistive technology.
    Q_PROPERTY(QString summary READ summary NOTIFY counted)
    Q_PROPERTY(QString spoken READ spoken NOTIFY counted)

public:
    static constexpr int WordsPerMinute = 200;
    // Longer documents are read and counted on the worker.
    static constexpr int ImmediateUnits = 20000;
    // How long typing pauses before a count.
    static constexpr int DelayMs = 120;

    explicit WordCounter(QObject *parent = nullptr);
    ~WordCounter() override;

    Document *document() const { return m_document; }
    void setDocument(Document *document);
    int selectionStart() const { return m_selectionStart; }
    void setSelectionStart(int position);
    int selectionEnd() const { return m_selectionEnd; }
    void setSelectionEnd(int position);
    int words() const { return m_words; }
    int minutes() const;
    bool ofSelection() const { return m_ofSelection; }
    QString summary() const;
    QString spoken() const;
    // A count waits on the worker.
    bool counting() const { return m_watcher.isRunning(); }
    // The worker's pool, which a test can hold up.
    QThreadPool *pool() { return &m_pool; }

    // The words of a Markdown text, or of what is written from `from` to
    // `to` in it.
    static int count(QStringView markdown);
    static int count(const QString &markdown, int from, int to);
    // The words of plain text.
    static int countWords(QStringView text);

signals:
    void documentChanged();
    void selectionChanged();
    void counted();

private:
    void schedule(int delay);
    void start();
    void take(int words, bool ofSelection);

    QPointer<Document> m_document;
    QMetaObject::Connection m_edits;
    int m_selectionStart = 0;
    int m_selectionEnd = 0;
    int m_words = 0;
    bool m_ofSelection = false;
    // Every change of the text or the selection makes earlier counts stale.
    quint64 m_generation = 0;
    quint64 m_running = 0;
    bool m_runningSelection = false;
    QTimer m_timer;
    QThreadPool m_pool;
    QFutureWatcher<int> m_watcher;
};
