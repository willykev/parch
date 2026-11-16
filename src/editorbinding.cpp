#include "editorbinding.h"

#include "caretrest.h"
#include "sourceboundaries.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QFontInfo>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QQuickTextDocument>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace {

// Each of these becomes one paragraph separator when QTextCursor inserts it;
// U+FDD0 and U+FDD1 are Qt's frame markers.
bool showsAsSeparator(QChar c)
{
    return c == u'\r' || c == u'\n' || c == QChar(0x2029) || c == QChar(0xfdd0) || c == QChar(0xfdd1);
}

// What a key types, by the rule Qt's own text controls use
// (QInputControl::isAcceptableInput). Every key the TextEdit would insert has
// to be inserted here instead, or it would change the projection behind the
// source's back.
bool typesText(const QKeyEvent *key)
{
    const QString text = key->text();
    if (text.isEmpty()) return false;
    const QChar c = text.at(0);
    if (c.category() == QChar::Other_Format) return true;
    if (key->modifiers() == Qt::ControlModifier || key->modifiers() == (Qt::ShiftModifier | Qt::ControlModifier))
        return false;
    if (c.isPrint() || c.category() == QChar::Other_PrivateUse) return true;
    if (c.isHighSurrogate() && text.size() > 1 && text.at(1).isLowSurrogate()) return true;
    return c == u'\t';
}

// Above this many units Qt's TextEdit builds scene nodes only for the part in
// view (QQuickTextEditPrivate::largeTextSizeThreshold), but it decides so only
// when its text property is set. The projection is filled and edited without
// that property, so the binding makes the same decision; without it every
// frame rebuilt the nodes of the whole document, about 33 ms per frame for
// 100,000 words with the software renderer against 0.2 ms with it.
constexpr int largeTextUnits = 10000;

// The position at or before `position` where a grapheme starts.
int graphemeAtOrBefore(QStringView source, int position)
{
    return sourceGraphemeBoundary(source, position) ? position : previousSourceGrapheme(source, position);
}

// The position at or after `position` where a grapheme starts.
int graphemeAtOrAfter(QStringView source, int position)
{
    return sourceGraphemeBoundary(source, position) ? position : nextSourceGrapheme(source, position);
}

} // namespace

void CrlfPairs::reset(QStringView source)
{
    m_offsets.clear();
    m_owing = 0;
    m_shift = 0;
    for (qsizetype cr = source.indexOf(u'\r'); cr >= 0 && cr + 1 < source.size(); cr = source.indexOf(u'\r', cr + 1))
        if (source[cr + 1] == u'\n') m_offsets.push_back(int(cr));
}

qsizetype CrlfPairs::firstFrom(int position) const
{
    qsizetype low = 0, high = qsizetype(m_offsets.size());
    while (low < high) {
        const qsizetype middle = low + (high - low) / 2;
        if (at(middle) < position) low = middle + 1;
        else high = middle;
    }
    return low;
}

void CrlfPairs::settleTo(qsizetype index)
{
    if (m_shift != 0) {
        for (qsizetype i = m_owing; i < index; ++i) m_offsets[size_t(i)] += m_shift;
        for (qsizetype i = index; i < m_owing; ++i) m_offsets[size_t(i)] -= m_shift;
    }
    m_owing = index;
}

void CrlfPairs::update(QStringView source, SourceRange range, int inserted)
{
    const int start = range.start;
    const int oldEnd = range.start + range.length;
    // A pair whose CR stands from just before the edit to its last removed
    // unit loses a half or is split by the edit; the old offsets find them.
    const qsizetype first = firstFrom(start - 1);
    const qsizetype last = firstFrom(oldEnd);
    settleTo(first);
    // The edit can form pairs with a CR from just before it to its last
    // inserted unit. They are counted first so that the table is resized once.
    const qsizetype scanStart = qMax(0, start - 1);
    const qsizetype scanEnd = qMin<qsizetype>(start + inserted, source.size() - 1);
    qsizetype formed = 0;
    for (qsizetype cr = scanStart; cr < scanEnd; ++cr)
        if (source[cr] == u'\r' && source[cr + 1] == u'\n') ++formed;
    const qsizetype lost = last - first;
    const auto at = m_offsets.begin() + first;
    if (formed > lost) m_offsets.insert(at + lost, size_t(formed - lost), 0);
    else if (formed < lost) m_offsets.erase(at + formed, at + lost);
    qsizetype index = first;
    for (qsizetype cr = scanStart; cr < scanEnd; ++cr)
        if (source[cr] == u'\r' && source[cr + 1] == u'\n') m_offsets[size_t(index++)] = int(cr);
    // The new pairs are exact; every pair after them owes this edit's shift too.
    m_owing = first + formed;
    m_shift += inserted - range.length;
    if (m_owing >= qsizetype(m_offsets.size())) m_shift = 0;
}

bool CrlfPairs::startsAt(int position) const
{
    const qsizetype index = firstFrom(position);
    return index < qsizetype(m_offsets.size()) && at(index) == position;
}

int CrlfPairs::toEditor(int position) const
{
    // Every pair whose CR is before the position has given up one unit.
    return position - int(firstFrom(position));
}

int CrlfPairs::toSource(int position) const
{
    // The pair at index i shows its separator at at(i) - i, an increasing
    // sequence; every pair whose separator is before the position adds one unit.
    qsizetype low = 0, high = qsizetype(m_offsets.size());
    while (low < high) {
        const qsizetype middle = low + (high - low) / 2;
        if (at(middle) - middle < position) low = middle + 1;
        else high = middle;
    }
    return position + int(low);
}

// What Vim keys reach: the view's document, selection and edits, in source
// offsets, and the TextEdit for keys typed again.
class EditorBinding::VimAdapter : public VimHost {
public:
    explicit VimAdapter(EditorBinding *binding) : b(binding) {}

    Document *document() const override { return b->m_loaded ? b->m_document.data() : nullptr; }
    bool editable() const override { return b->editable() && !b->m_blocked; }
    bool composing() const override { return b->m_composing; }
    int cursor() const override { return b->m_loaded ? b->m_pairs.toSource(b->editorCursor()) : 0; }
    int anchor() const override { return b->m_loaded ? b->m_pairs.toSource(b->editorAnchor()) : 0; }
    void select(int anchor, int cursor) override
    {
        if (!b->m_loaded) return;
        const int length = b->m_buffer->length();
        b->selectEditor(b->m_pairs.toEditor(qBound(0, anchor, length)), b->m_pairs.toEditor(qBound(0, cursor, length)));
        b->revealCursor();
    }
    bool apply(const TextChange &change) override { return b->m_loaded && b->apply(change); }
    void type(int key, Qt::KeyboardModifiers modifiers, const QString &text) override
    {
        if (!b->m_editor) return;
        QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
        QCoreApplication::sendEvent(b->m_editor, &event);
    }
    void commit(const QString &text) override
    {
        if (!b->m_editor) return;
        QInputMethodEvent event;
        event.setCommitString(text);
        QCoreApplication::sendEvent(b->m_editor, &event);
    }
    void setInputMethodEnabled(bool enabled) override
    {
        b->m_inputMethodAllowed = enabled;
        b->applyInputMethod();
    }

private:
    EditorBinding *b;
};

EditorBinding::EditorBinding(QObject *parent)
    : QObject(parent), m_vimHost(std::make_unique<VimAdapter>(this)), m_vim(new VimMode(m_vimHost.get(), this))
{
    // Wherever the cursor moves, Vim keys follow; their own moves they know of.
    connect(this, &EditorBinding::cursorChanged, m_vim, &VimMode::selectionMoved);
}

// Qt removes a destroyed filter from the TextEdit by itself, and the views
// own their projections, so nothing needs undoing here. Vim keys go with
// the binding and reach nothing of it as they go.
EditorBinding::~EditorBinding() = default;

Document *EditorBinding::document() const { return m_document; }

void EditorBinding::setDocument(Document *document)
{
    if (m_document == document) {
        // A document that was destroyed was let go of then; this only makes sure.
        if (!document) detach();
        return;
    }
    detach();
    disconnect(m_documentGone);
    m_document = document;
    if (document) {
        m_documentGone = connect(document, &QObject::destroyed, this, [this] {
            detach();
            emit documentChanged();
        });
    }
    attach();
    emit documentChanged();
}

QQuickItem *EditorBinding::editor() const { return m_editor; }

void EditorBinding::setEditor(QQuickItem *editor)
{
    if (m_editor == editor) {
        // An editor that was destroyed was let go of then; this only makes sure.
        if (!editor) detach();
        return;
    }
    detach();
    disconnect(m_editorGone);
    m_editor = editor;
    if (editor) {
        // A destroyed TextEdit takes its text document with it: detach()
        // leaves that projection alone and drops the buffer's connection, so
        // the buffer's next change reaches nothing that has gone.
        m_editorGone = connect(editor, &QObject::destroyed, this, [this] {
            detach();
            emit editorChanged();
        });
    }
    attach();
    emit editorChanged();
}

bool EditorBinding::editable() const
{
    // A conflict pauses saving, not writing: what is typed meanwhile is kept
    // in recovery and is the text the writer's choice acts on.
    return m_loaded && m_document && m_document->state() != Document::State::ReadOnly && !m_document->isTextless();
}

void EditorBinding::attach()
{
    if (!m_document || !m_editor) return;
    // The editor's members are found by name, since TextEdit's class is not
    // public API. An editor without one of them is refused as a whole, with
    // a warning that names what is missing, rather than used half-way.
    const QMetaObject *meta = m_editor->metaObject();
    QStringList missing;
    const auto property = [&](const char *name) {
        const int index = meta->indexOfProperty(name);
        if (index < 0) missing.append(QLatin1String(name));
        return meta->property(index);
    };
    const auto method = [&](const char *signature) {
        const int index = meta->indexOfMethod(signature);
        if (index < 0) missing.append(QLatin1String(signature));
        return meta->method(index);
    };
    const auto signal = [&](const char *signature) {
        if (meta->indexOfSignal(signature) < 0) missing.append(QLatin1String(signature));
    };
    auto *quickDocument = m_editor->property("textDocument").value<QQuickTextDocument *>();
    if (!quickDocument || !quickDocument->textDocument()) missing.append(QStringLiteral("textDocument"));
    m_cursorPosition = property("cursorPosition");
    m_selectionStart = property("selectionStart");
    m_selectionEnd = property("selectionEnd");
    m_cursorRectangle = property("cursorRectangle");
    m_select = method("select(int,int)");
    m_positionAt = method("positionAt(qreal,qreal)");
    m_positionToRectangle = method("positionToRectangle(int)");
    signal("cursorPositionChanged()");
    signal("selectionStartChanged()");
    signal("selectionEndChanged()");
    if (!missing.isEmpty()) {
        qWarning("EditorBinding: the editor has no %s, so it cannot show the document",
                 qPrintable(missing.join(QStringLiteral(", "))));
        return;
    }
    m_textDocument = quickDocument->textDocument();
    m_buffer = m_document->buffer();
    // Undo belongs to the buffer and is shared by every view; the projection never undoes by itself.
    m_textDocument->setUndoRedoEnabled(false);
    m_editor->installEventFilter(this);
    m_connections = {
        connect(m_buffer, &SourceBuffer::sourceChanged, this,
                [this](Revision revision, SourceRange range, const QString &replacement) {
                    applySourceChange(revision, range, replacement);
                }),
        connect(m_document, &Document::changed, this, &EditorBinding::refreshEditable),
        connect(m_textDocument, &QTextDocument::contentsChange, this, &EditorBinding::checkNativeChange),
        // Qt makes and keeps this document itself; should it go on its own, the projection goes with it.
        connect(m_textDocument, &QObject::destroyed, this, [this] { detach(); }),
        connect(m_editor, SIGNAL(cursorPositionChanged()), this, SLOT(editorCursorMoved())),
        connect(m_editor, SIGNAL(selectionStartChanged()), this, SLOT(editorSelectionChanged())),
        connect(m_editor, SIGNAL(selectionEndChanged()), this, SLOT(editorSelectionChanged())),
        connect(m_editor, &QQuickItem::widthChanged, this, &EditorBinding::editorWidthChanged),
    };
    // The line height is a multiple of the font's size, so it follows a zoom or another font.
    if (meta->indexOfSignal("fontChanged(QFont)") >= 0)
        m_connections.append(connect(m_editor, SIGNAL(fontChanged(QFont)), this, SLOT(applyLineHeight())));
    // Being made read-only or editable again starts Qt's blinking anew.
    if (meta->indexOfSignal("readOnlyChanged(bool)") >= 0)
        m_connections.append(connect(m_editor, SIGNAL(readOnlyChanged(bool)), this, SLOT(stillQtCursor()),
                                     Qt::QueuedConnection));
    QMetaObject::invokeMethod(this, &EditorBinding::stillQtCursor, Qt::QueuedConnection);
    // Qt lets a TextEdit take input methods again as it becomes editable.
    if (meta->indexOfSignal("readOnlyChanged(bool)") >= 0)
        m_connections.append(connect(m_editor, SIGNAL(readOnlyChanged(bool)), this, SLOT(applyInputMethod())));
    load();
    // Another document, or the first: Vim keys start again in Normal mode.
    m_vim->reset();
    applyInputMethod();
}

void EditorBinding::stillQtCursor()
{
    // Qt's text control blinks its cursor on a timer of its own whenever
    // the editor has the focus, and each blink draws the whole editor again.
    // Hiding that cursor is what stops the timer, but the control starts it
    // again as it takes the focus, so it is hidden once the focus is taken.
    CaretRest::stillCursorOf(m_editor);
}

void EditorBinding::detach()
{
    // Called also while the document, the TextEdit or its text document is
    // being destroyed: whatever has gone reads as null here and is left alone.
    cancelComposition();
    for (const QMetaObject::Connection &connection : std::as_const(m_connections)) disconnect(connection);
    m_connections.clear();
    if (m_editor) m_editor->removeEventFilter(this);
    // A projection is cleared only while its TextEdit lives; one going with
    // its TextEdit is not touched, since Qt is taking that TextEdit apart.
    if (m_editor && m_textDocument && m_loaded) {
        m_projecting = true;
        m_textDocument->clear();
        m_projecting = false;
    }
    m_buffer = nullptr;
    m_textDocument = nullptr;
    m_pairs.reset({});
    m_loaded = false;
    m_topAnchor = {};
    m_cursorSource = m_anchorSource = 0;
    refreshEditable();
    m_vim->reset();
}

void EditorBinding::editorWidthChanged()
{
    if (!m_loaded) load();
}

void EditorBinding::load()
{
    if (m_loaded || !m_buffer || !m_textDocument || !m_editor) return;
    // At no width every glyph would get a line of its own, which for a long
    // document costs far more than waiting for the pane to give it a width.
    if (m_editor->width() <= 0) return;
    const QString source = m_buffer->text();
    m_editor->setFlag(QQuickItem::ItemObservesViewport, source.size() > largeTextUnits);
    m_projecting = true;
    m_textDocument->clear();
    QTextCursor cursor(m_textDocument);
    const QTextBlockFormat format = lineFormat();
    m_appliedLineHeight = int(format.lineHeight());
    // Every paragraph Qt makes from here on takes this format from the one it splits.
    cursor.setBlockFormat(format);
    cursor.insertText(source);
    m_pairs.reset(source);
    m_loaded = true;
    m_topAnchor = {};
    selectEditor(0, 0);
    m_projecting = false;
    refreshCursor();
    refreshEditable();
    emit projectionLoaded();
}

void EditorBinding::applySourceChange(Revision revision, SourceRange range, const QString &replacement)
{
    if (!m_loaded || !m_editor || !m_textDocument) return;
    // From here to the end the buffer holds the new text while the projection
    // and the pairs still describe the old one, so the TextEdit's notices in
    // between are part of following the change: neither a write past the
    // source nor a caret to report.
    m_projecting = true;
    // An edit from another view moves the text a composition here was going
    // to replace, so the composition is dropped; nothing it showed was
    // written. This view's own edits have dropped theirs already. It happens
    // before the selection is read, so that the selection the composition
    // covered is the one that follows the edit.
    cancelComposition();
    m_topAnchor.valid = false;
    const QString source = m_buffer->text();
    const int start = range.start;
    const int oldEnd = range.start + range.length;
    const int inserted = int(replacement.size());
    const int newEnd = start + inserted;
    const int delta = inserted - range.length;
    const int oldLength = int(source.size()) - delta;

    // This view's selection and the projection's span for the old source,
    // read while the projection and the pairs still describe the old source.
    const int cursor = m_pairs.toSource(editorCursor());
    const int anchor = m_pairs.toSource(editorAnchor());
    const bool startWasInsidePair = m_pairs.startsAt(start - 1);
    const bool endWasInsidePair = m_pairs.startsAt(oldEnd - 1);
    const int shownBeforeStart = start > 0 ? m_pairs.toEditor(start - 1) : 0;
    const int shownStart = m_pairs.toEditor(start);
    const int shownEnd = m_pairs.toEditor(oldEnd);
    const int shownAfterEnd = oldEnd < oldLength ? m_pairs.toEditor(oldEnd + 1) : shownEnd;

    m_pairs.update(source, range, inserted);
    emit sourceEdited(revision, range, inserted);
    // Qt shows CRLF as one separator, so where the edit joins or splits a
    // pair, before or after, the whole pair is replaced: one unit more of the
    // source, and the separator it shows.
    const bool growStart = startWasInsidePair || m_pairs.startsAt(start - 1);
    const bool growEnd = endWasInsidePair || m_pairs.startsAt(newEnd - 1);
    const int spanStart = growStart ? start - 1 : start;
    const int spanEnd = growEnd ? newEnd + 1 : newEnd;

    QTextCursor edit(m_textDocument);
    edit.setPosition(growStart ? shownBeforeStart : shownStart);
    edit.setPosition(growEnd ? shownAfterEnd : shownEnd, QTextCursor::KeepAnchor);
    // Qt turns the line breaks in the inserted source into separators itself,
    // exactly as the pairs table counts them.
    edit.insertText(source.mid(spanStart, spanEnd - spanStart));
    // Positions before the edit stay, positions after it move by its length,
    // and positions inside what it replaced go to its end; the selection keeps
    // its direction.
    const auto track = [&](int position) {
        if (position < start) return position;
        if (position >= oldEnd) return position + delta;
        return newEnd;
    };
    selectEditor(m_pairs.toEditor(track(anchor)), m_pairs.toEditor(track(cursor)));
    m_projecting = false;
    if ((oldLength > largeTextUnits) != (source.size() > largeTextUnits))
        m_editor->setFlag(QQuickItem::ItemObservesViewport, source.size() > largeTextUnits);
    refreshCursor();
}

void EditorBinding::setLineHeight(qreal lineHeight)
{
    if (lineHeight <= 0 || qFuzzyCompare(lineHeight, m_lineHeight)) return;
    m_lineHeight = lineHeight;
    applyLineHeight();
    emit lineHeightChanged();
}

void EditorBinding::setBlocked(bool blocked)
{
    if (blocked == m_blocked) return;
    m_blocked = blocked;
    // What was being composed is dropped, not written.
    if (blocked) cancelComposition();
    m_vim->setBlocked(blocked);
    emit blockedChanged();
}

void EditorBinding::setVimKeys(bool on)
{
    if (on == m_vimKeys) return;
    m_vimKeys = on;
    // Normal mode takes no input method, so what one is composing is dropped.
    if (on) cancelComposition();
    m_vim->setActive(on);
    emit vimKeysChanged();
}

void EditorBinding::applyInputMethod()
{
    if (!m_editor) return;
    const bool takes = m_inputMethodAllowed && !m_editor->property("readOnly").toBool();
    if (bool(m_editor->flags() & QQuickItem::ItemAcceptsInputMethod) == takes) return;
    m_editor->setFlag(QQuickItem::ItemAcceptsInputMethod, takes);
    if (m_editor->hasActiveFocus()) QGuiApplication::inputMethod()->update(Qt::ImEnabled);
}

void EditorBinding::setSetextHeadings(bool setext)
{
    if (setext == m_setextHeadings) return;
    m_setextHeadings = setext;
    emit setextHeadingsChanged();
}

MarkdownEditing::Options EditorBinding::editingOptions() const
{
    MarkdownEditing::Options options;
    options.setextHeadings = m_setextHeadings;
    if (m_document) options.newline = m_document->newline();
    return options;
}

bool EditorBinding::format(const QString &command)
{
    if (!MarkdownEditing::isCommand(command)) return false;
    const MarkdownEditing::Options options = editingOptions();
    return applyEditing([&](QStringView text, int anchor, int cursor) {
        return MarkdownEditing::format(command, text, anchor, cursor, options);
    });
}

bool EditorBinding::applyEditing(const std::function<std::optional<TextChange>(QStringView, int, int)> &operation)
{
    if (!editable() || m_projecting || m_blocked) return false;
    cancelComposition();
    // The selection as whole graphemes, keeping its direction.
    const SourceRange range = selection();
    const bool backwards = editorCursor() < editorAnchor();
    const int anchor = backwards ? range.start + range.length : range.start;
    const int cursor = backwards ? range.start : range.start + range.length;
    std::optional<TextChange> change;
    {
        // The copy of the text is gone before the edit, or the buffer would copy all of it to change it.
        const QString text = m_buffer->text();
        change = operation(text, anchor, cursor);
    }
    return change && apply(*change);
}

bool EditorBinding::apply(const TextChange &change)
{
    if (!editable() || m_projecting || m_blocked) return false;
    if (!change.edits.isEmpty() && m_buffer->replace(change.edits) == SourceBuffer::EditOutcome::Rejected) return false;
    const int length = m_buffer->length();
    selectEditor(m_pairs.toEditor(qBound(0, change.anchor, length)), m_pairs.toEditor(qBound(0, change.cursor, length)));
    revealCursor();
    return true;
}

QTextBlockFormat EditorBinding::lineFormat() const
{
    // A fixed height, in pixels, of the font's own size, as the font is set
    // in pixels; Qt places each line's baseline four fifths of the way down
    // it, so the room a line height adds is shared above and below the text.
    QTextBlockFormat format;
    const QFont font = m_editor ? m_editor->property("font").value<QFont>() : QFont();
    const int pixels = font.pixelSize() > 0 ? font.pixelSize() : QFontInfo(font).pixelSize();
    format.setLineHeight(qMax(1, qRound(m_lineHeight * pixels)), QTextBlockFormat::FixedHeight);
    return format;
}

void EditorBinding::applyLineHeight()
{
    // TextEdit has no line height of its own; Qt's text layout takes it from
    // each paragraph. A font change that leaves the height as it was, such as
    // another family, lays nothing out again.
    if (!m_loaded) return;
    const QTextBlockFormat format = lineFormat();
    if (int(format.lineHeight()) == m_appliedLineHeight) return;
    m_appliedLineHeight = int(format.lineHeight());
    m_projecting = true;
    QTextCursor all(m_textDocument);
    all.select(QTextCursor::Document);
    all.mergeBlockFormat(format);
    m_projecting = false;
}

void EditorBinding::checkNativeChange(int position, int removed, int added)
{
    if (m_projecting || !m_loaded || m_resyncPending || (removed == 0 && added == 0)) return;
    // A composition event the binding sends makes Qt redraw the preedit's
    // paragraph, reported as a change of equal length; that needs no
    // comparing on every key of an input method. A change of length is still
    // a write.
    if (m_dispatching && removed == added) return;
    // Qt reports its own formatting, the preedit among it, as a change of
    // equal length; only a change of the characters is a write past the source.
    if (removed == added) {
        const int shownLength = m_textDocument->characterCount() - 1;
        if (shownLength != m_pairs.toEditor(m_buffer->length())) {
            scheduleResync();
            return;
        }
        const int end = qMin(position + added, shownLength);
        if (position >= end) return;
        QTextCursor span(m_textDocument);
        span.setPosition(position);
        span.setPosition(end, QTextCursor::KeepAnchor);
        const QString source = m_buffer->text();
        const int sourceEnd = m_pairs.toSource(end);
        QString expected;
        for (int i = m_pairs.toSource(position); i < sourceEnd; ++i) {
            const QChar c = source.at(i);
            if (c == u'\r' && i + 1 < sourceEnd && source.at(i + 1) == u'\n') ++i;
            expected += showsAsSeparator(c) ? QChar(QChar::ParagraphSeparator) : c;
        }
        if (span.selectedText() == expected) return;
    }
    scheduleResync();
}

void EditorBinding::scheduleResync()
{
    // Something wrote into the TextEdit without going through the source:
    // that write cannot be trusted, so the view shows the source again. Qt is
    // still inside its own edit here, so the document is rebuilt afterwards.
    if (m_resyncPending) return;
    m_resyncPending = true;
    QTimer::singleShot(0, this, [this] {
        m_resyncPending = false;
        if (!m_loaded) return;
        const int cursor = m_cursorSource, anchor = m_anchorSource;
        const int top = topSourcePosition();
        m_loaded = false;
        load();
        restore(cursor, anchor, top);
    });
}

void EditorBinding::refreshEditable()
{
    const bool now = editable();
    if (now == m_editable) return;
    m_editable = now;
    if (!now) cancelComposition();
    emit editableChanged();
}

void EditorBinding::refreshCursor()
{
    if (!m_loaded) return;
    const int cursor = m_pairs.toSource(editorCursor());
    const int anchor = m_pairs.toSource(editorAnchor());
    if (cursor == m_cursorSource && anchor == m_anchorSource) return;
    m_cursorSource = cursor;
    m_anchorSource = anchor;
    emit cursorChanged();
}

void EditorBinding::editorCursorMoved()
{
    if (m_projecting) return;
    refreshCursor();
    // A caret moved by keys or the mouse stays in view; one the binding
    // placed, for another view's edit or a restore, leaves the scroll alone.
    if (!m_selecting) revealCursor();
}

void EditorBinding::editorSelectionChanged()
{
    if (!m_projecting) refreshCursor();
}

int EditorBinding::sourceToEditor(int position) const
{
    if (!m_loaded || position < 0 || position > m_buffer->length()) return -1;
    return m_pairs.toEditor(position);
}

int EditorBinding::editorToSource(int position) const
{
    if (!m_loaded || position < 0 || position > m_pairs.toEditor(m_buffer->length())) return -1;
    return m_pairs.toSource(position);
}

int EditorBinding::editorCursor() const { return m_cursorPosition.read(m_editor).toInt(); }

int EditorBinding::editorAnchor() const
{
    const int cursor = editorCursor();
    const int start = m_selectionStart.read(m_editor).toInt();
    return cursor == start ? m_selectionEnd.read(m_editor).toInt() : start;
}

void EditorBinding::selectEditor(int anchor, int cursor)
{
    // After an edit at the caret Qt has usually moved it to the same place already.
    if (editorCursor() == cursor && editorAnchor() == anchor) return;
    // Moving Qt's selection while it shows a preedit makes it ask the input
    // method to commit what is being composed, which would write it. The
    // composition is dropped first, so that Qt has nothing to commit.
    cancelComposition();
    const bool selecting = std::exchange(m_selecting, true);
    m_select.invoke(m_editor, Q_ARG(int, anchor), Q_ARG(int, cursor));
    m_selecting = selecting;
}

SourceRange EditorBinding::selection() const
{
    const QString source = m_buffer->text();
    const int first = m_pairs.toSource(m_selectionStart.read(m_editor).toInt());
    const int last = m_pairs.toSource(m_selectionEnd.read(m_editor).toInt());
    // Qt can hold a position inside a grapheme, for one set from code; an
    // edit there would split it, so the selection grows to whole graphemes
    // and a caret moves to the start of its grapheme.
    const int start = graphemeAtOrBefore(source, first);
    const int end = last == first ? start : graphemeAtOrAfter(source, last);
    return {start, end - start};
}

bool EditorBinding::replace(SourceRange range, const QString &text, EditKind kind, bool placeCaret)
{
    // A view never edits the source while it follows a change of it, nor while it is blocked.
    if (!editable() || m_projecting || m_blocked) return false;
    const auto outcome = m_buffer->replace(range, text, kind);
    if (outcome == SourceBuffer::EditOutcome::Rejected) return false;
    if (placeCaret) {
        // The caret ends after what was written, also when that equals what it replaced.
        const int caret = m_pairs.toEditor(range.start + int(text.size()));
        selectEditor(caret, caret);
        revealCursor();
    }
    return true;
}

void EditorBinding::insertText(const QString &text, EditKind kind)
{
    if (!editable()) return;
    cancelComposition();
    replace(selection(), text, kind);
}

void EditorBinding::deleteTo(Extent extent)
{
    if (!editable()) return;
    cancelComposition();
    SourceRange range = selection();
    if (range.length == 0) {
        const QString source = m_buffer->text();
        int other = -1;
        switch (extent) {
        case Extent::PreviousGrapheme:
            other = previousSourceGrapheme(source, range.start);
            break;
        case Extent::NextGrapheme:
            other = nextSourceGrapheme(source, range.start);
            break;
        case Extent::StartOfWord:
        case Extent::EndOfWord:
        case Extent::EndOfLine: {
            // Words and paragraph ends are where Qt's own keys find them.
            QTextCursor cursor(m_textDocument);
            cursor.setPosition(m_pairs.toEditor(range.start));
            if (extent == Extent::StartOfWord) {
                cursor.movePosition(QTextCursor::PreviousWord);
            } else if (extent == Extent::EndOfWord) {
                cursor.movePosition(QTextCursor::NextWord);
            } else {
                const QTextBlock block = cursor.block();
                const bool atEnd = cursor.position() == block.position() + block.length() - 1;
                cursor.movePosition(atEnd ? QTextCursor::NextCharacter : QTextCursor::EndOfBlock);
            }
            other = m_pairs.toSource(cursor.position());
            other = other < range.start ? graphemeAtOrBefore(source, other) : graphemeAtOrAfter(source, other);
            break;
        }
        }
        if (other < 0 || other == range.start) return;
        range = {qMin(range.start, other), qAbs(range.start - other)};
    }
    replace(range, {}, EditKind::Deleting);
}

void EditorBinding::undo()
{
    if (!editable() || m_blocked) return;
    cancelComposition();
    m_buffer->undo();
}

void EditorBinding::redo()
{
    if (!editable() || m_blocked) return;
    cancelComposition();
    m_buffer->redo();
}

void EditorBinding::copy()
{
    if (!m_loaded) return;
    // The source's own text, not Qt's, which turns CRLF into LF and NBSP into spaces.
    const SourceRange range = selection();
    if (range.length > 0) QGuiApplication::clipboard()->setText(m_buffer->text().mid(range.start, range.length));
}

void EditorBinding::cut()
{
    if (!editable()) return;
    cancelComposition();
    const SourceRange range = selection();
    if (range.length == 0) return;
    copy();
    replace(range, {}, EditKind::Other);
}

void EditorBinding::paste()
{
    if (!editable()) return;
    if (const QMimeData *mime = QGuiApplication::clipboard()->mimeData()) pasteFrom(mime);
}

QString EditorBinding::withDocumentBreaks(const QString &text) const
{
    const qsizetype first = std::find_if(text.cbegin(), text.cend(),
                                         [](QChar c) { return c == u'\r' || c == u'\n'; }) - text.cbegin();
    if (first == text.size() || !m_document) return text;
    const QString newline = m_document->newline();
    QString converted = text.left(first);
    converted.reserve(text.size() + text.size() / 8);
    for (qsizetype i = first; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (c == u'\r') {
            converted += newline;
            // CR LF is one break.
            if (i + 1 < text.size() && text.at(i + 1) == u'\n') ++i;
        } else if (c == u'\n') {
            converted += newline;
        } else {
            converted += c;
        }
    }
    return converted;
}

void EditorBinding::replaceEditorRange(int start, int end, const QString &text)
{
    if (!editable()) return;
    cancelComposition();
    const int shownLength = m_pairs.toEditor(m_buffer->length());
    const int first = m_pairs.toSource(qBound(0, qMin(start, end), shownLength));
    const int last = m_pairs.toSource(qBound(0, qMax(start, end), shownLength));
    SourceRange range;
    {
        // The copy of the text is gone before the edit, or the buffer would copy all of it to change it.
        const QString source = m_buffer->text();
        const int from = graphemeAtOrBefore(source, first);
        range = {from, (last == first ? from : graphemeAtOrAfter(source, last)) - from};
    }
    // Like the TextEdit's own methods, these leave the caret to follow the edit.
    replace(range, withDocumentBreaks(text), EditKind::Other, false);
}

void EditorBinding::appendParagraph(const QString &text)
{
    if (!editable()) return;
    cancelComposition();
    const int length = m_buffer->length();
    const QString added = withDocumentBreaks(text);
    replace({length, 0}, length > 0 ? m_document->newline() + added : added, EditKind::Other, false);
}

void EditorBinding::cancelComposition()
{
    if (!m_composing) return;
    m_composing = false;
    if (!m_editor || !m_loaded) return;
    // An empty event takes Qt's preedit away. The selection was collapsed
    // while the preedit showed, so Qt has nothing to delete with it, and the
    // selection the composition was going to replace comes back.
    QInputMethodEvent cancel;
    m_dispatching = true;
    QCoreApplication::sendEvent(m_editor, &cancel);
    m_dispatching = false;
    selectEditor(m_pairs.toEditor(m_compositionSelection.start),
                 m_pairs.toEditor(m_compositionSelection.start + m_compositionSelection.length));
}

bool EditorBinding::commitInput(const QInputMethodEvent *event, SourceRange selected)
{
    SourceRange span;
    QString replacement;
    int replaceFrom = 0;
    const QString commit = withDocumentBreaks(event->commitString());
    // The copy of the text is gone before the edit, or the buffer would copy all of it to change it.
    if (!planInput(event, commit, selected, &span, &replacement, &replaceFrom)) return false;
    if (!replace(span, replacement, EditKind::Other, false)) return false;
    const int caret = m_pairs.toEditor(replaceFrom + int(commit.size()));
    selectEditor(caret, caret);
    return true;
}

bool EditorBinding::planInput(const QInputMethodEvent *event, const QString &commit, SourceRange selected,
                              SourceRange *span, QString *replacement, int *replaceFrom) const
{
    // Qt commits by removing the selection, then replacing replacementLength
    // units from replacementStart beside the caret, counted in what it shows
    // with the selection gone. The same is worked out here in source
    // positions and made one edit, so that one undo takes it back.
    const QString source = m_buffer->text();
    const int selectedEnd = selected.start + selected.length;
    const int shownStart = m_pairs.toEditor(selected.start);
    const int shownRemoved = m_pairs.toEditor(selectedEnd) - shownStart;
    const int shownLength = m_pairs.toEditor(int(source.size())) - shownRemoved;
    const qint64 from = qint64(shownStart) + event->replacementStart();
    const qint64 to = from + event->replacementLength();
    if (event->replacementLength() < 0 || from < 0 || to > shownLength) return false;
    // Positions in the text without the selection, which equals the source up
    // to the selection and the source less the selection after it.
    const auto withoutSelection = [&](qint64 shown) {
        return shown <= shownStart ? m_pairs.toSource(int(shown))
                                   : m_pairs.toSource(int(shown) + shownRemoved) - selected.length;
    };
    const int replaceStart = withoutSelection(from);
    const int replaceTo = withoutSelection(to);
    // The source span the selection and the replaced text cover together.
    const int spanStart = qMin(replaceStart, selected.start);
    const int spanEnd = replaceTo > selected.start ? replaceTo + selected.length : selectedEnd;
    // The replaced text must not split a grapheme of the text without the
    // selection; that text is assembled around the span, whole paragraphs at
    // both ends, so that Unicode's rules see all the context they need.
    if (selected.length == 0) {
        if (!sourceGraphemeRange(source, {replaceStart, replaceTo - replaceStart})) return false;
    } else {
        const int contextStart = sourceParagraphRange(source, spanStart).start;
        const SourceRange lastParagraph = sourceParagraphRange(source, spanEnd);
        int contextEnd = lastParagraph.start + lastParagraph.length;
        // The paragraph's own line break belongs to it, both halves of a CRLF.
        if (contextEnd < source.size())
            contextEnd += (source.at(contextEnd) == u'\r' && contextEnd + 1 < source.size()
                           && source.at(contextEnd + 1) == u'\n') ? 2 : 1;
        const QString context = source.mid(contextStart, selected.start - contextStart)
            + source.mid(selectedEnd, contextEnd - selectedEnd);
        if (!sourceGraphemeRange(context, {replaceStart - contextStart, replaceTo - replaceStart})) return false;
    }
    *span = {spanStart, spanEnd - spanStart};
    *replacement = source.mid(spanStart, selected.start - spanStart) + source.mid(selectedEnd, spanEnd - selectedEnd);
    replacement->replace(replaceStart - spanStart, replaceTo - replaceStart, commit);
    *replaceFrom = replaceStart;
    return true;
}

void EditorBinding::handleInputMethod(QInputMethodEvent *event)
{
    event->accept();
    if (!editable()) {
        cancelComposition();
        return;
    }
    if (!m_composing) m_compositionSelection = selection();
    if (!event->commitString().isEmpty() || event->replacementLength() != 0) {
        const SourceRange selected = m_compositionSelection;
        // Qt's preedit goes before the source changes under it.
        cancelComposition();
        if (!commitInput(event, selected)) return;
        m_compositionSelection = selection();
    }
    QList<QInputMethodEvent::Attribute> attributes;
    for (const QInputMethodEvent::Attribute &attribute : event->attributes()) {
        if (attribute.type != QInputMethodEvent::Selection) {
            attributes.append(attribute);
            continue;
        }
        // A surrounding-text selection moves the caret after the commit, in
        // the paragraph the caret is in, as Qt's own TextEdit applies it; a
        // composition still showing ends there and starts again at the new place.
        const int shownLength = m_textDocument->characterCount() - 1;
        const int paragraph = m_textDocument->findBlock(editorCursor()).position();
        const int anchor = qBound(0, paragraph + attribute.start, shownLength);
        selectEditor(anchor, qBound(0, anchor + attribute.length, shownLength));
        m_compositionSelection = selection();
    }
    const QString preedit = event->preeditString();
    if (preedit.isEmpty()) {
        cancelComposition();
    } else {
        // Qt would delete a selection when the composition ends, so it is
        // collapsed to where the composition begins; the source range it
        // covers is kept for the commit. Later preedits of the same
        // composition leave the selection alone.
        if (!m_composing) {
            const int start = m_pairs.toEditor(m_compositionSelection.start);
            selectEditor(start, start);
        }
        QInputMethodEvent shown(preedit, attributes);
        m_dispatching = true;
        QCoreApplication::sendEvent(m_editor, &shown);
        m_dispatching = false;
        m_composing = true;
    }
    revealCursor();
}

bool EditorBinding::handleKey(QKeyEvent *key)
{
    if (key->matches(QKeySequence::Undo)) {
        undo();
    } else if (key->matches(QKeySequence::Redo)) {
        redo();
    } else if (key->matches(QKeySequence::Copy)) {
        copy();
    } else if (key->matches(QKeySequence::Cut)) {
        cut();
    } else if (key->matches(QKeySequence::Paste)) {
        paste();
    } else if (key->key() == Qt::Key_V && key->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
        pastePlainText();
    } else if (key->matches(QKeySequence::InsertLineSeparator)) {
        // A Markdown hard line break: two spaces and the document's own line break.
        const MarkdownEditing::Options options = editingOptions();
        applyEditing([&](QStringView text, int anchor, int cursor) {
            return std::optional(MarkdownEditing::hardBreak(text, anchor, cursor, options));
        });
    } else if (key->matches(QKeySequence::InsertParagraphSeparator)) {
        // A list goes on with a new item; anywhere else this is a line break
        // in the document's own convention, since Markdown gives U+2028 no meaning.
        const MarkdownEditing::Options options = editingOptions();
        if (!applyEditing([&](QStringView text, int anchor, int cursor) {
                return MarkdownEditing::newLine(text, anchor, cursor, options);
            })
            && m_document)
            insertText(m_document->newline(), EditKind::Typing);
    } else if ((key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier)
               || key->key() == Qt::Key_Backtab || (key->key() == Qt::Key_Tab && key->modifiers() == Qt::ShiftModifier)) {
        // In a table, from cell to cell; on a list item or lines selected,
        // indenting; elsewhere Tab types a tab, and Shift+Tab is Qt's.
        const bool back = key->key() == Qt::Key_Backtab || key->modifiers() & Qt::ShiftModifier;
        const MarkdownEditing::Options options = editingOptions();
        const bool done = applyEditing([&](QStringView text, int anchor, int cursor) {
            if (auto moved = MarkdownEditing::moveToCell(text, anchor, cursor, back, options)) return moved;
            return MarkdownEditing::indent(text, anchor, cursor, back, options);
        });
        if (!done) {
            if (back) return false;
            insertText(QStringLiteral("\t"), EditKind::Typing);
        }
    } else if (key->key() == Qt::Key_Backspace && !(key->modifiers() & ~Qt::ShiftModifier)) {
        deleteTo(Extent::PreviousGrapheme);
    } else if (key->matches(QKeySequence::Delete)) {
        deleteTo(Extent::NextGrapheme);
    } else if (key->matches(QKeySequence::DeleteStartOfWord)) {
        deleteTo(Extent::StartOfWord);
    } else if (key->matches(QKeySequence::DeleteEndOfWord)) {
        deleteTo(Extent::EndOfWord);
    } else if (key->matches(QKeySequence::DeleteEndOfLine)) {
        deleteTo(Extent::EndOfLine);
    } else if (key->key() == Qt::Key_Insert && key->modifiers() == Qt::NoModifier) {
        // Overwrite mode would make Qt show a different caret than the edit typing makes.
    } else if (key->key() == Qt::Key_Escape && m_composing) {
        cancelComposition();
    } else if (typesText(key)) {
        insertText(key->text(), EditKind::Typing);
    } else {
        return false;
    }
    key->accept();
    return true;
}

bool EditorBinding::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_editor) {
        switch (event->type()) {
        case QEvent::FocusIn:
            // The control starts its blinking as it takes the focus, after this.
            QMetaObject::invokeMethod(this, &EditorBinding::stillQtCursor, Qt::QueuedConnection);
            [[fallthrough]];
        case QEvent::KeyPress:
        case QEvent::InputMethod:
        case QEvent::MouseButtonPress:
            emit activity();
            break;
        default:
            break;
        }
    }
    if (watched == m_editor && !m_dispatching && m_blocked
        && (event->type() == QEvent::KeyPress || event->type() == QEvent::InputMethod)) {
        // Neither Qt's own handling nor the binding's: nothing it would type,
        // move or write happens behind a dialog.
        event->accept();
        return true;
    }
    if (watched == m_editor && !m_dispatching && m_loaded) {
        if (event->type() == QEvent::InputMethod) {
            auto *input = static_cast<QInputMethodEvent *>(event);
            // Outside Insert mode, Vim keys take what an input method sends as keys.
            if (!m_vim->handleInputMethod(input)) handleInputMethod(input);
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (m_vim->handleKey(key)) {
                key->accept();
                return true;
            }
            return handleKey(key);
        }
    }
    return QObject::eventFilter(watched, event);
}

QQuickItem *EditorBinding::viewport() const
{
    for (QQuickItem *item = m_editor ? m_editor->parentItem() : nullptr; item; item = item->parentItem())
        if (item->metaObject()->indexOfProperty("contentY") >= 0) return item;
    return nullptr;
}

void EditorBinding::revealCursor()
{
    QQuickItem *view = viewport();
    if (!view || !m_loaded) return;
    const QRectF caret = m_cursorRectangle.read(m_editor).toRectF().translated(m_editor->position());
    const qreal top = view->property("contentY").toReal();
    const qreal height = view->height();
    qreal target = top;
    if (caret.top() < top) target = caret.top();
    else if (caret.bottom() > top + height) target = caret.bottom() - height;
    if (target == top) return;
    const qreal maximum = qMax<qreal>(0, view->property("contentHeight").toReal() - height);
    view->setProperty("contentY", qBound<qreal>(0, target, maximum));
}

void EditorBinding::keepCursorInView(qreal formerHeight)
{
    QQuickItem *view = viewport();
    if (!view || !m_loaded) return;
    const QRectF caret = m_cursorRectangle.read(m_editor).toRectF().translated(m_editor->position());
    const qreal top = view->property("contentY").toReal();
    if (caret.top() < top || caret.bottom() > top + formerHeight) return;
    revealCursor();
}

QTextDocument *EditorBinding::textDocument() const { return m_loaded ? m_textDocument.data() : nullptr; }

SourceRange EditorBinding::visibleRange() const
{
    if (!m_loaded) return {0, 0};
    QQuickItem *view = viewport();
    // Without a view that scrolls, the whole text is in view.
    if (!view) return {0, m_buffer->length()};
    const qreal top = view->property("contentY").toReal() - m_editor->y();
    int first = 0, last = 0;
    m_positionAt.invoke(m_editor, Q_RETURN_ARG(int, first), Q_ARG(qreal, 0), Q_ARG(qreal, qMax<qreal>(0, top)));
    m_positionAt.invoke(m_editor, Q_RETURN_ARG(int, last), Q_ARG(qreal, m_editor->width()),
                        Q_ARG(qreal, qMax<qreal>(0, top + view->height())));
    const int start = m_pairs.toSource(qMin(first, last));
    return {start, m_pairs.toSource(qMax(first, last)) - start};
}

int EditorBinding::topSourcePosition() const
{
    QQuickItem *view = viewport();
    if (!m_loaded || !view) return 0;
    const qreal contentY = view->property("contentY").toReal();
    if (m_topAnchor.valid && m_topAnchor.contentY == contentY) return m_topAnchor.position;
    int shown = 0;
    m_positionAt.invoke(m_editor, Q_RETURN_ARG(int, shown), Q_ARG(qreal, 0),
                        Q_ARG(qreal, qMax<qreal>(0, contentY - m_editor->y())));
    // The start of that line, whichever way its text runs.
    const QTextBlock block = m_textDocument->findBlock(shown);
    if (block.isValid() && block.layout()) {
        const QTextLine line = block.layout()->lineForTextPosition(shown - block.position());
        if (line.isValid()) shown = block.position() + line.textStart();
    }
    return m_pairs.toSource(shown);
}

void EditorBinding::restore(int cursor, int anchor, int top)
{
    if (!m_loaded) return;
    const QString source = m_buffer->text();
    // Saved positions may be from older text: they are kept inside it and at grapheme starts.
    const auto fit = [&](int position) { return graphemeAtOrBefore(source, qBound(0, position, int(source.size()))); };
    // A composition keeps its own collapsed selection until it ends.
    if (!m_composing) selectEditor(m_pairs.toEditor(fit(anchor)), m_pairs.toEditor(fit(cursor)));
    QQuickItem *view = viewport();
    if (!view) return;
    const int position = fit(top);
    QRectF line;
    m_positionToRectangle.invoke(m_editor, Q_RETURN_ARG(QRectF, line), Q_ARG(int, m_pairs.toEditor(position)));
    // The first line keeps the margin above it in view.
    const qreal maximum = qMax<qreal>(0, view->property("contentHeight").toReal() - view->height());
    view->setProperty("contentY", line.y() <= 0 ? 0 : qBound<qreal>(0, m_editor->y() + line.y(), maximum));
    m_topAnchor = {true, position, view->property("contentY").toReal()};
}

void EditorBinding::select(int anchor, int cursor)
{
    if (!m_loaded || m_composing) return;
    const QString source = m_buffer->text();
    const auto fit = [&](int position) { return graphemeAtOrBefore(source, qBound(0, position, int(source.size()))); };
    selectEditor(m_pairs.toEditor(fit(anchor)), m_pairs.toEditor(fit(cursor)));
    // The binding placed the caret, so its move left the scroll alone; the
    // caret is shown here, by the least scroll that does.
    revealCursor();
}
