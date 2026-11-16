#pragma once

#include "editordecorations.h"
#include "spellchecker.h"

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <vector>

// The misspelled words of one view, as a decoration layer of wavy underlines
// in the given colour. The text is checked on the spell checker's worker,
// never as it is typed: once the view shows it, and again once typing has
// paused for SettleMs. Each check reads a copy of the text, finds what is
// prose there as the Markdown engine reads it (spellingProse), and checks it
// a paragraph at a time, the paragraphs in view first, handing each
// paragraph's words over as it goes.
// A paragraph edited since the copy was taken is not taken; it is checked
// again. A paragraph whose text and prose are unchanged since an earlier
// check is not checked again. A word added to the dictionary or ignored
// loses its underline at once, everywhere.
class SpellingLayer : public QObject {
    Q_OBJECT
    Q_PROPERTY(EditorDecorations *decorations READ decorations WRITE setDecorations NOTIFY decorationsChanged)
    Q_PROPERTY(SpellChecker *checker READ checker WRITE setChecker NOTIFY checkerChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    // A check is waiting to start or running.
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

public:
    static constexpr int SettleMs = 300;
    // The decoration layer's name.
    static QString layerName() { return QStringLiteral("spelling"); }

    explicit SpellingLayer(QObject *parent = nullptr);
    ~SpellingLayer() override;

    EditorDecorations *decorations() const { return m_decorations; }
    void setDecorations(EditorDecorations *decorations);
    SpellChecker *checker() const { return m_checker; }
    void setChecker(SpellChecker *checker);
    QColor color() const { return m_color; }
    void setColor(const QColor &color);
    bool checking() const { return m_running || m_settle.isActive(); }

    // The misspelled word at a source position, its end included, as
    // {start, end, word}; empty when there is none.
    Q_INVOKABLE QVariantMap misspellingAt(int position) const;
    // Puts `replacement` in place of `word` at [start, end), as one step to
    // undo, if the word is still there.
    Q_INVOKABLE bool replaceWord(int start, int end, const QString &word, const QString &replacement);
    // Starts a check now, rather than once typing pauses.
    void checkNow();

signals:
    void decorationsChanged();
    void checkerChanged();
    void colorChanged();
    void checkingChanged();

public:
    // What a check found in one paragraph: its misspelled words.
    struct Found {
        SourceRange paragraph;
        std::vector<DecorationSpan> words;
    };

private:
    struct Cache;

    bool active() const;
    void defineLayer();
    void connectBinding();
    // Checks again when the document in view is read in another dialect.
    void watchDialect();
    void refresh();
    void schedule(int delayMs);
    void start();
    void cancel();
    void take(quint64 check, Revision revision, const std::vector<Found> &found, bool last);
    void accepted(const QString &word);

    QPointer<EditorDecorations> m_decorations;
    QPointer<SpellChecker> m_checker;
    QPointer<EditorBinding> m_binding;
    QList<QMetaObject::Connection> m_decorationConnections, m_bindingConnections, m_checkerConnections;
    QMetaObject::Connection m_dialect;
    QColor m_color;
    QTimer m_settle;
    quint64 m_check = 0;
    std::shared_ptr<std::atomic_bool> m_cancelled;
    std::shared_ptr<Cache> m_cache;
    bool m_running = false;
    // A paragraph's words were not taken, since it was edited meanwhile.
    bool m_stale = false;
};
