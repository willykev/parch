#pragma once

#include "document.h"
#include "sourcebuffer.h"
#include "sourcetypes.h"

#include <QMetaMethod>
#include <QMetaProperty>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QStringView>
#include <vector>

class QInputMethodEvent;
class QKeyEvent;
class QTextBlockFormat;
class QTextDocument;

// Where the CRLF pairs of a source are. Qt shows CRLF as one paragraph
// separator and every other character, line breaks included, as one
// character, so these pairs are the only places where positions in the
// source and in its Qt projection differ. A source without carriage returns
// keeps an empty table and maps one to one.
//
// The offsets of each pair's CR are kept sorted. An edit shifts every pair
// after it, so that shift is kept owing from one index on and settled only as
// far as the next edit elsewhere needs: typing in one place costs the length
// of the edit, not the number of pairs after it.
class CrlfPairs {
public:
    void reset(QStringView source);
    // Follows an edit that replaced `range` of the old source with `inserted`
    // units; `source` is the text after the edit.
    void update(QStringView source, SourceRange range, int inserted);
    // True when a pair's CR is at this offset, so the next position is inside it.
    bool startsAt(int position) const;
    // Source position to projection position. The position between CR and LF
    // maps to the pair's separator, like the one before the CR.
    int toEditor(int position) const;
    // Projection position to source position, never one inside a pair.
    int toSource(int position) const;

private:
    int at(qsizetype index) const { return m_offsets[size_t(index)] + (index >= m_owing ? m_shift : 0); }
    // The first index whose CR is at or after `position`.
    qsizetype firstFrom(int position) const;
    void settleTo(qsizetype index);

    std::vector<int> m_offsets;
    // Entries from this index on still owe m_shift.
    qsizetype m_owing = 0;
    int m_shift = 0;
};

// Keeps one view's TextEdit showing one document. The document's SourceBuffer
// owns the text, its revisions and undo; the TextEdit's QTextDocument is only
// a projection of it, edited by one delta per source change. Every input the
// TextEdit would apply itself, from keys, input methods, the clipboard or its
// own editing methods, is turned into a SourceBuffer edit at grapheme
// boundaries instead, and comes back to this and every other view as a source
// change. Cursor, selection and scroll stay with the view. Qt's own cursor
// is kept from blinking, since each of its blinks draws the whole editor
// again; the view draws a caret of its own. The document and
// the TextEdit may each be destroyed before the binding; it then lets go of
// the other and acts as a binding without one.
class EditorBinding : public QObject {
    Q_OBJECT
    Q_PROPERTY(Document *document READ document WRITE setDocument NOTIFY documentChanged)
    Q_PROPERTY(QQuickItem *editor READ editor WRITE setEditor NOTIFY editorChanged)
    Q_PROPERTY(int cursorSource READ cursorSource NOTIFY cursorChanged)
    Q_PROPERTY(int anchorSource READ anchorSource NOTIFY cursorChanged)
    // False for a read-only document, whose bytes are kept as they are: it
    // takes no edits, though it can be selected and copied. A document in
    // conflict is still written in; only its saving waits.
    Q_PROPERTY(bool editable READ editable NOTIFY editableChanged)
    // Line height as a multiple of the editor font's pixel size, as CSS sets
    // one: every paragraph's lines sit round(lineHeight × pixel size) pixels
    // apart, whatever the font's own line spacing, and again so whenever the
    // editor's font changes.
    Q_PROPERTY(qreal lineHeight READ lineHeight WRITE setLineHeight NOTIFY lineHeightChanged)
    // While true, as while the window shows a dialog, the view takes no keys,
    // input method text or edits of its own; the source can still change
    // from elsewhere, and the view follows it.
    Q_PROPERTY(bool blocked READ blocked WRITE setBlocked NOTIFY blockedChanged)

public:
    explicit EditorBinding(QObject *parent = nullptr);
    ~EditorBinding() override;

    Document *document() const;
    void setDocument(Document *document);
    QQuickItem *editor() const;
    void setEditor(QQuickItem *editor);
    int cursorSource() const { return m_cursorSource; }
    int anchorSource() const { return m_anchorSource; }
    bool editable() const;
    qreal lineHeight() const { return m_lineHeight; }
    void setLineHeight(qreal lineHeight);
    bool blocked() const { return m_blocked; }
    void setBlocked(bool blocked);

    // UTF-16 positions; -1 for a position outside the text.
    Q_INVOKABLE int sourceToEditor(int utf16) const;
    Q_INVOKABLE int editorToSource(int position) const;

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void copy();
    Q_INVOKABLE void cut();
    Q_INVOKABLE void paste();
    // The TextEdit's own insert, remove and clear, in its positions.
    Q_INVOKABLE void replaceEditorRange(int start, int end, const QString &text);
    // The TextEdit's own append: a new paragraph at the end.
    Q_INVOKABLE void appendParagraph(const QString &text);

    // The source offset of the first visible line: the first whose text
    // reaches below the top of the view, since the room a line height adds
    // around a line's text belongs to no line.
    Q_INVOKABLE int topSourcePosition() const;
    // Selects from anchor to cursor and scrolls the line holding `top` to the
    // top of the view. All three are source offsets.
    Q_INVOKABLE void restore(int cursor, int anchor, int top);
    // The view's height has changed from `formerHeight`: a caret that was in
    // view is kept in view, by the least scroll that shows it, as when typing
    // moves it; otherwise the view keeps its top line.
    Q_INVOKABLE void keepCursorInView(qreal formerHeight);

signals:
    void documentChanged();
    void editorChanged();
    void cursorChanged();
    // A key, an input method, a click or the focus reached the editor: the
    // writer is here, whatever it changes.
    void activity();
    void editableChanged();
    void lineHeightChanged();
    void blockedChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void editorCursorMoved();
    void applyLineHeight();
    void stillQtCursor();
    void editorSelectionChanged();
    void editorWidthChanged();

private:
    void attach();
    void detach();
    void load();
    void applySourceChange(SourceRange range, const QString &replacement);
    // The format every paragraph takes: lines lineHeight × the font's pixel size apart.
    QTextBlockFormat lineFormat() const;
    // Notices a change of the TextEdit's text that did not come from the source.
    void checkNativeChange(int position, int removed, int added);
    void scheduleResync();
    void refreshEditable();
    // Follows the TextEdit's caret and selection into source offsets.
    void refreshCursor();

    bool handleKey(QKeyEvent *key);
    void handleInputMethod(QInputMethodEvent *event);
    bool commitInput(const QInputMethodEvent *event, SourceRange selected);
    // The source edit an input method commit of `commit` makes, or false when it would split a grapheme.
    bool planInput(const QInputMethodEvent *event, const QString &commit, SourceRange selected, SourceRange *span,
                   QString *replacement, int *replaceFrom) const;
    void insertText(const QString &text, EditKind kind);
    // Text from outside the source, as pasted, inserted or committed, with
    // every line break in it, CRLF, a lone CR or LF, made the document's own.
    // Text with no break, as typing gives, comes back as it is.
    QString withDocumentBreaks(const QString &text) const;
    // How far a deletion key reaches when nothing is selected.
    enum class Extent { PreviousGrapheme, NextGrapheme, StartOfWord, EndOfWord, EndOfLine };
    void deleteTo(Extent extent);
    // Edits the source from this view; with placeCaret the caret ends after the new text.
    bool replace(SourceRange range, const QString &text, EditKind kind, bool placeCaret = true);
    void cancelComposition();

    // The selection in source offsets, widened to whole graphemes.
    SourceRange selection() const;
    int editorCursor() const;
    int editorAnchor() const;
    // Selects in the TextEdit without scrolling to the caret.
    void selectEditor(int anchor, int cursor);
    QQuickItem *viewport() const;
    void revealCursor();

    QPointer<Document> m_document;
    QPointer<SourceBuffer> m_buffer;
    QPointer<QQuickItem> m_editor;
    QPointer<QTextDocument> m_textDocument;
    // What attach() connected, undone together by detach().
    QList<QMetaObject::Connection> m_connections;
    // Made when the document and the editor are set, so that either may be
    // destroyed first and the binding lets go of the other when it is.
    QMetaObject::Connection m_documentGone, m_editorGone;
    CrlfPairs m_pairs;
    QMetaProperty m_cursorPosition, m_selectionStart, m_selectionEnd, m_cursorRectangle;
    QMetaMethod m_select, m_positionAt, m_positionToRectangle;
    qreal m_lineHeight = 1.0;
    // The distance between lines that the paragraphs have, in pixels.
    int m_appliedLineHeight = 0;
    int m_cursorSource = 0;
    int m_anchorSource = 0;
    bool m_loaded = false;
    bool m_editable = false;
    bool m_blocked = false;
    // The projection is being changed to follow the source.
    bool m_projecting = false;
    // An event this binding made is going to the TextEdit unfiltered.
    bool m_dispatching = false;
    // The binding is placing the selection, so the view does not scroll to it.
    bool m_selecting = false;
    bool m_resyncPending = false;
    bool m_composing = false;
    // The source range a composition will replace, kept while Qt shows its preedit.
    SourceRange m_compositionSelection{0, 0};
    // The anchor restore() last set, kept while the view stays where restore()
    // put it, so that repeated width changes do not creep to earlier lines.
    struct TopAnchor {
        bool valid = false;
        int position = 0;
        qreal contentY = 0;
    };
    TopAnchor m_topAnchor;
};
