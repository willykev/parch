#pragma once

#include "attachments.h"
#include "document.h"
#include "markdownediting.h"
#include "sourcebuffer.h"
#include "sourcetypes.h"
#include "tablegrids.h"
// The binding's Vim keys are a property QML reads, so the type must be complete here.
#include "vimmode.h"

#include <QMetaMethod>
#include <QMetaProperty>
#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QString>
#include <QStringView>
#include <QUrl>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Insertion;
class QInputMethodEvent;
class QKeyEvent;

class QMimeData;
class QTextBlockFormat;
class QTextCursor;
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
// change. A write that reaches the projection some other way, as assistive
// technology makes through Qt's own interface for the TextEdit, is taken
// back once Qt has finished it and made the same kind of edit, or refused
// when the view takes no edits. Cursor, selection and scroll stay with the
// view; a caret or selection set from outside is followed as a click's is.
// Qt's own cursor is kept from blinking, since each of its blinks draws the
// whole editor again; the view draws a caret of its own. The document and
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
    // The text holds a word longer than LongWord characters, which Qt
    // could only break into lines in time that grows with its square: the
    // editor then wraps at any character, in time that grows with the line.
    // Found as the text is shown and as edits make such a word; it stays
    // until the text is shown again.
    Q_PROPERTY(bool wrapAnywhere READ wrapAnywhere NOTIFY wrapAnywhereChanged)
    // Line height as a multiple of the editor font's pixel size, as CSS sets
    // one: every paragraph's lines sit round(lineHeight × pixel size) pixels
    // apart, whatever the font's own line spacing, and again so whenever the
    // editor's font changes.
    Q_PROPERTY(qreal lineHeight READ lineHeight WRITE setLineHeight NOTIFY lineHeightChanged)
    // While true, as while the window shows a dialog, the view takes no keys,
    // input method text or edits of its own; the source can still change
    // from elsewhere, and the view follows it.
    Q_PROPERTY(bool blocked READ blocked WRITE setBlocked NOTIFY blockedChanged)
    // Where pasted images and dropped files are placed; without it, files
    // are not attached, and images in pasted HTML keep only their alt text.
    Q_PROPERTY(Attachments *attachments READ attachments WRITE setAttachments NOTIFY attachmentsChanged)
    // The heading commands underline headings of levels one and two rather
    // than marking them with #.
    Q_PROPERTY(bool setextHeadings READ setextHeadings WRITE setSetextHeadings NOTIFY setextHeadingsChanged)
    // The view takes Vim keys, in the modes `vim` keeps; every edit they
    // make still goes through the source. Off, keys type as always.
    Q_PROPERTY(bool vimKeys READ vimKeys WRITE setVimKeys NOTIFY vimKeysChanged)
    Q_PROPERTY(VimMode *vim READ vim CONSTANT)
    // The view's tables drawn as grids, and what they make of its editing.
    Q_PROPERTY(TableGrids *tables READ tables CONSTANT)
    // The view shows its tables as grids; off, as their Markdown.
    Q_PROPERTY(bool tableGrids READ tableGrids WRITE setTableGrids NOTIFY tableGridsChanged)

public:
    // What the writer chose for a dropped Markdown file.
    enum DropChoice { Cancel, InsertLink, Include };
    Q_ENUM(DropChoice)

    explicit EditorBinding(QObject *parent = nullptr);
    ~EditorBinding() override;

    Document *document() const;
    void setDocument(Document *document);
    QQuickItem *editor() const;
    void setEditor(QQuickItem *editor);
    int cursorSource() const { return m_cursorSource; }
    int anchorSource() const { return m_anchorSource; }
    bool editable() const;
    bool wrapAnywhere() const { return m_wrapAnywhere; }
    static constexpr qsizetype LongWord = 10000;
    // Whether the text has a run of more than LongWord characters with no
    // place to break, looking only from `from` to `to`, and past them until
    // the run around them ends.
    static bool hasLongWord(QStringView text, qsizetype from, qsizetype to);
    qreal lineHeight() const { return m_lineHeight; }
    void setLineHeight(qreal lineHeight);
    bool blocked() const { return m_blocked; }
    void setBlocked(bool blocked);
    bool setextHeadings() const { return m_setextHeadings; }
    void setSetextHeadings(bool setext);
    bool vimKeys() const { return m_vimKeys; }
    void setVimKeys(bool on);
    VimMode *vim() const { return m_vim; }
    TableGrids *tables() const { return m_tables; }
    bool tableGrids() const;
    void setTableGrids(bool on);

    // Runs one of MarkdownEditing's formatting commands on this view's
    // selection, as one undo step, and selects what the command leaves
    // selected. False when the command is unknown, changes nothing, or the
    // view takes no edits now.
    Q_INVOKABLE bool format(const QString &command);
    // Puts in a new table of empty cells after the caret's line, `rows`
    // counting the header, and the caret in its first cell, as one step.
    Q_INVOKABLE bool insertTable(int columns, int rows);
    // Whether a formatting command would change the text with the
    // selection as it is now, or with the caret at a source position; for
    // commands that act only on what is at the caret.
    Q_INVOKABLE bool acts(const QString &command, int position = -1) const;
    // The label of a note reference at a source position that nothing
    // defines, in a document whose dialect has notes; empty for none.
    Q_INVOKABLE QString undefinedNoteAt(int position) const;
    // Where the note reference with this label that nothing defines is now:
    // at the position, or else the one nearest to it, as when the text has
    // changed since the position was taken; -1 when there is none.
    Q_INVOKABLE int undefinedNoteNear(const QString &label, int position) const;

    // UTF-16 positions; -1 for a position outside the text.
    Q_INVOKABLE int sourceToEditor(int utf16) const;
    Q_INVOKABLE int editorToSource(int position) const;

    // The projection this view shows, once it shows the document.
    QTextDocument *textDocument() const;
    // The source from the first line in view to the last, which reach past
    // the top and bottom of the view; empty while nothing is shown. With
    // `around`, from that many view heights above the view to as many below.
    SourceRange visibleRange(qreal around = 0) const;

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void copy();
    Q_INVOKABLE void cut();
    // Pastes what the clipboard holds as Markdown: files copied in a file
    // manager are attached as when dropped, HTML is converted, an image is
    // saved in the assets, and anything else is its plain text.
    Q_INVOKABLE void paste();

    // Paste and drop, in editorpaste.cpp.
    Attachments *attachments() const;
    void setAttachments(Attachments *attachments);
    // The clipboard's plain text, never converted, as Ctrl+Shift+V pastes it.
    Q_INVOKABLE void pastePlainText();
    // Takes what a drop brings, at a point in the editor's coordinates:
    // files are attached, HTML converted, links and text put in. False,
    // and nothing happens, when the document takes no edits or the drop
    // brings nothing to take.
    Q_INVOKABLE bool dropAt(qreal x, qreal y, const QList<QUrl> &urls, const QString &html, const QString &text);
    // The answers to includeAsked() and copyAsked(); a DropChoice for the first.
    Q_INVOKABLE void answerInclude(int choice);
    Q_INVOKABLE void answerCopy(bool copy);
    // Where the caret is drawn, in the editor's coordinates: in a grid's
    // cell as the grid draws it.
    Q_INVOKABLE QRectF caretRectangle() const;
    // What an input method is composing, shown where the composition
    // starts, a source position; empty while it composes nothing.
    QString preedit() const { return m_composing ? m_preedit : QString(); }
    int preeditAt() const { return m_compositionSelection.start; }
    // Where a caret at a source position would be drawn, in the editor's
    // coordinates, a grid's cells and rows included.
    QRectF sourceRectangle(int source) const;
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
    // Selects from anchor to cursor, source offsets kept inside the text and
    // at grapheme boundaries, and scrolls only as far as shows the caret.
    Q_INVOKABLE void select(int anchor, int cursor);
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
    void wrapAnywhereChanged();
    void lineHeightChanged();
    void blockedChanged();
    void attachmentsChanged();
    // A dropped Markdown document can be linked to or included; a draft
    // includes nothing until it is named. Answered with answerInclude().
    void includeAsked(const QString &name, bool canInclude);
    // Dropped files larger than the large-file size are to be copied.
    // Answered with answerCopy().
    void copyAsked(const QStringList &names, qint64 bytes);
    void setextHeadingsChanged();
    // The source changed, at `revision`, from `range` to `inserted` units.
    // Sent once positions map to the new source and before the projection
    // shows it, so that what decorates the projection follows the change
    // before Qt lays the changed paragraphs out.
    void sourceEdited(Revision revision, SourceRange range, int inserted);
    // The projection was filled with the whole source anew, as when it is
    // first shown or shown again after a write that did not come from it.
    void projectionLoaded();
    void vimKeysChanged();
    void tableGridsChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void editorCursorMoved();
    void applyLineHeight();
    void stillQtCursor();
    void editorSelectionChanged();
    void editorWidthChanged();
    void applyInputMethod();

private:
    friend class TableGrids;
    friend class TableGrid;
    // Where a caret at a projection position is drawn, in the editor's
    // coordinates: in a grid as the grid draws it, elsewhere as Qt lays it out.
    QRectF rectangleAt(int editorPosition) const;
    void attach();
    void detach();
    void load();
    void applySourceChange(Revision revision, SourceRange range, const QString &replacement);
    // Writes a span of the projection anew, line for line, where the lines
    // are as many as before; otherwise whole.
    void applyByLines(QTextCursor &span, const QString &written);
    // The format every paragraph takes: lines lineHeight × the font's pixel size apart.
    QTextBlockFormat lineFormat() const;
    // Notices a change of the TextEdit's text that did not come from the source.
    void checkNativeChange(int position, int removed, int added);
    // Gathers a write into the projection that did not come from the source,
    // to be taken in by takeForeignWrite() once Qt has finished it.
    void noteForeignWrite(int position, int removed, int added);
    // The projection shows the source again where a write changed it, and
    // what the write left there becomes one source edit, as the TextEdit's
    // own insert or remove makes one; a view that takes no edits keeps the
    // source as it is.
    void takeForeignWrite();
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
    // Paste and drop, in editorpaste.cpp.
    void pasteFrom(const QMimeData *mime);
    // Converts HTML and puts it at the range; false when it shows nothing.
    // A large page, or one with images, is converted on a worker and goes
    // in when ready, as `plain` when its Markdown shows nothing or would
    // grow far past it.
    bool insertHtml(const QString &html, SourceRange range, const QString &plain = {});
    void pasteImage(const QMimeData *mime, const QString &alt, SourceRange range);
    // Text an insertion puts in later goes in as this view's own edit, so
    // that a cell takes it as its text and no table is broken.
    void writeThrough(Insertion *insertion);
    // An image with no alt text, `length` long, came into the document at
    // `at`: a caret that followed it in, in this view, goes between its
    // brackets, for the alt text to be written there.
    void inviteAltText(const Document *document, int at, int length);
    void attachFiles(const QStringList &paths, SourceRange range);
    struct DropRequest;
    void continueDrop(const std::shared_ptr<DropRequest> &request);
    void placeDrop(const std::shared_ptr<DropRequest> &request);
    // How far a deletion key reaches when nothing is selected.
    enum class Extent { PreviousGrapheme, NextGrapheme, StartOfWord, EndOfWord, EndOfLine };
    void deleteTo(Extent extent);
    // Edits the source from this view; with placeCaret the caret ends after the new text.
    bool replace(SourceRange range, const QString &text, EditKind kind, bool placeCaret = true);
    // Applies a command's edits as one step and selects as it says.
    bool apply(const TextChange &change);
    // An editing key's change, run on this view's selection; false when the key does nothing special here.
    bool applyEditing(const std::function<std::optional<TextChange>(QStringView, int, int)> &operation);
    MarkdownEditing::Options editingOptions() const;
    void cancelComposition();

    // The selection in source offsets, widened to whole graphemes.
    SourceRange selection() const;
    int editorCursor() const;
    int editorAnchor() const;
    // Selects in the TextEdit without scrolling to the caret.
    void selectEditor(int anchor, int cursor);
    QQuickItem *viewport() const;
    // Scrolls the view to y, in whole pixels within its range. Qt's Flickable
    // rounds a position between pixels as soon as the height of what it
    // shows changes, as it does at every new width of the column: the view
    // would move by itself between the new width and the line put back at
    // its top, and the preview beside it would follow there and back.
    void scrollView(QQuickItem *view, qreal y) const;
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
    bool m_wrapAnywhere = false;
    bool m_editable = false;
    bool m_blocked = false;
    bool m_setextHeadings = false;
    // The projection is being changed to follow the source.
    bool m_projecting = false;
    // An event this binding made is going to the TextEdit unfiltered.
    bool m_dispatching = false;
    // The binding is placing the selection, so the view does not scroll to it.
    bool m_selecting = false;
    bool m_resyncPending = false;
    // A write into the projection that did not come from the source, waiting
    // for takeForeignWrite(): Qt is still inside it when it reports it, and
    // one write, as a replacement, can come as a removal and an insertion.
    // From `start` it covers the projection up to `oldEnd` before the write
    // and up to `newEnd` after it.
    struct ForeignWrite {
        bool pending = false;
        int start = 0;
        int oldEnd = 0;
        int newEnd = 0;
        // This view's selection before the write, in source offsets.
        int anchor = 0;
        int cursor = 0;
    };
    ForeignWrite m_foreign;
    bool m_composing = false;
    // What the input method shows while it composes.
    QString m_preedit;
    // An edit made for something that finished later, as a paste converted
    // off the window's thread, leaves the caret where it is.
    bool m_keepCaret = false;
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
    QPointer<Attachments> m_attachments;
    // The drop waiting for its files to be looked at or for an answer.
    std::shared_ptr<DropRequest> m_drop;
    // What Vim keys reach of this binding, and the keys themselves, which
    // reach nothing as they go.
    class VimAdapter;
    std::unique_ptr<VimAdapter> m_vimHost;
    VimMode *m_vim = nullptr;
    bool m_vimKeys = false;
    // Whether the TextEdit takes input method text: Vim keys allow it only in Insert mode.
    bool m_inputMethodAllowed = true;
    TableGrids *m_tables = nullptr;
    // Where the last edit through replace() left the caret, as the grids
    // placed it, in source offsets; -1 when the edit placed nothing.
    int m_placedCaret = -1;
    // The run of typing or deleting in a grid's cell this view is making,
    // and where its last edit left the caret: the next key there joins it.
    quint64 m_tableRun = 0;
    // This view's undo or redo is being applied: its caret goes where the change ends.
    bool m_undoing = false;
    int m_tableRunCaret = -1;
    EditKind m_tableRunKind = EditKind::Other;
};
