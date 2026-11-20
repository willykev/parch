#include "editorbinding.h"

#include "caretrest.h"
#include "sourceboundaries.h"
#include "tablegrids.h"

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
#include <cmath>
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

// True when every surrogate in the text is half of a pair.
bool wellFormed(QStringView text)
{
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i].isLowSurrogate()) return false;
        if (text[i].isHighSurrogate()) {
            if (i + 1 == text.size() || !text[i + 1].isLowSurrogate()) return false;
            ++i;
        }
    }
    return true;
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
        const bool stepping = b->m_tables->stepping();
        b->m_tables->setStepping(true);
        b->selectEditor(b->m_pairs.toEditor(qBound(0, anchor, length)), b->m_pairs.toEditor(qBound(0, cursor, length)));
        b->m_tables->setStepping(stepping);
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
    : QObject(parent), m_vimHost(std::make_unique<VimAdapter>(this)), m_vim(new VimMode(m_vimHost.get(), this)),
      m_tables(new TableGrids(this))
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
        // The room beside the column is the grids' too.
        connect(m_editor, &QQuickItem::xChanged, this, [this] {
            if (m_loaded) m_tables->relayout();
        }),
    };
    // The line height is a multiple of the font's size, so it follows a zoom or another font.
    if (meta->indexOfSignal("fontChanged(QFont)") >= 0) {
        m_connections.append(connect(m_editor, SIGNAL(fontChanged(QFont)), this, SLOT(applyLineHeight())));
        // Another font measures the grids' cells anew, the same height or not.
        m_connections.append(connect(m_editor, SIGNAL(fontChanged(QFont)), m_tables, SLOT(relayoutLater())));
    }
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
    m_tables->clear();
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
    m_foreign = {};
    m_topAnchor = {};
    m_cursorSource = m_anchorSource = 0;
    refreshEditable();
    m_vim->reset();
}

void EditorBinding::editorWidthChanged()
{
    if (!m_loaded) load();
    else m_tables->relayout();
}

void EditorBinding::load()
{
    if (m_loaded || !m_buffer || !m_textDocument || !m_editor) return;
    // At no width every glyph would get a line of its own, which for a long
    // document costs far more than waiting for the pane to give it a width.
    if (m_editor->width() <= 0) return;
    const QString source = m_buffer->text();
    m_editor->setFlag(QQuickItem::ItemObservesViewport, source.size() > largeTextUnits);
    // Decided before the text goes in, so that it is laid out once.
    if (const bool anywhere = hasLongWord(source, 0, source.size()); anywhere != m_wrapAnywhere) {
        m_wrapAnywhere = anywhere;
        emit wrapAnywhereChanged();
    }
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
    // The projection is the source again, whatever was written into it.
    m_foreign = {};
    m_topAnchor = {};
    selectEditor(0, 0);
    m_projecting = false;
    refreshCursor();
    refreshEditable();
    emit projectionLoaded();
    m_tables->loaded();
}

bool EditorBinding::hasLongWord(QStringView text, qsizetype from, qsizetype to)
{
    const auto breaks = [](QChar c) { return c.isSpace() || c == QChar(0x200B) || c == u'-'; };
    from = std::clamp<qsizetype>(from, 0, text.size());
    to = std::clamp<qsizetype>(to, from, text.size());
    // Back to where the run around `from` starts, but no further than needed.
    qsizetype start = from;
    while (start > 0 && from - start <= LongWord && !breaks(text.at(start - 1))) --start;
    qsizetype run = 0;
    for (qsizetype i = start; i < text.size(); ++i) {
        if (breaks(text.at(i))) {
            if (i >= to) return false;
            run = 0;
            continue;
        }
        if (++run > LongWord) return true;
    }
    return false;
}

void EditorBinding::applySourceChange(Revision revision, SourceRange range, const QString &replacement)
{
    if (!m_loaded || !m_editor || !m_textDocument) return;
    // An edit that makes a word too long to break wraps the editor anywhere
    // before the projection takes the edit, looking only around it.
    if (!m_wrapAnywhere && !replacement.isEmpty()
        && hasLongWord(m_buffer->text(), range.start, range.start + replacement.size())) {
        m_wrapAnywhere = true;
        emit wrapAnywhereChanged();
    }
    if (m_foreign.pending) {
        // The source changed while a write into the projection waited, so
        // the projection no longer shows the text the write was made in: it
        // shows the source anew, and the write is dropped. The selection
        // from before the write moves with the change, as below.
        m_foreign = {};
        const auto track = [&](int position) {
            if (position < range.start) return position;
            if (position >= range.start + range.length) return position + int(replacement.size()) - range.length;
            return range.start + int(replacement.size());
        };
        const int cursor = track(m_cursorSource), anchor = track(m_anchorSource);
        const int top = topSourcePosition();
        m_loaded = false;
        load();
        restore(cursor, anchor, top);
        return;
    }
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
    // Where this view's top line is, while the projection still shows the
    // old text: a change wholly above it, as another view's edit or a
    // replacement made all through the text, leaves what is in view in place.
    // Only a change that starts above the view is looked at further, so
    // that typing in view costs one lookup.
    QQuickItem *view = viewport();
    int top = 0;
    qreal intoTop = 0;
    if (view) {
        QRectF line = rectangleAt(m_pairs.toEditor(range.start));
        const qreal contentY = view->property("contentY").toReal();
        if (m_editor->y() + line.y() < contentY) top = topSourcePosition();
        if (top > 0) {
            line = rectangleAt(m_pairs.toEditor(top));
            intoTop = contentY - (m_editor->y() + line.y());
        }
    }
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

    // A caret in a grid's cell stays in that cell, at the same place in its
    // text, however the table is written around it.
    // This view's own undo leaves the caret where the change ends, as in any text.
    const TableGrids::Spot anchorSpot = m_undoing ? TableGrids::Spot{} : m_tables->spotOf(anchor, range);
    const TableGrids::Spot cursorSpot = m_undoing ? TableGrids::Spot{} : m_tables->spotOf(cursor, range);
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
    const QString written = source.mid(spanStart, spanEnd - spanStart);
    if (!growStart && !growEnd && m_tables->gridAt(start)) {
        // A table written anew line for line keeps its paragraphs, which
        // its grid hides, and changes only what changed in each.
        applyByLines(edit, written);
    } else {
        // Qt turns the line breaks in the inserted source into separators
        // itself, exactly as the pairs table counts them.
        edit.insertText(written);
    }
    // The grids follow the change, before the selection does.
    m_tables->sourceChanged(range, inserted);
    // Positions before the edit stay, positions after it move by its length,
    // and positions inside what it replaced go to its end; the selection keeps
    // its direction.
    const auto track = [&](int position) {
        if (position < start) return position;
        if (position >= oldEnd) return position + delta;
        return newEnd;
    };
    const int newAnchor = m_tables->positionOf(anchorSpot, range, inserted).value_or(track(anchor));
    const int newCursor = m_tables->positionOf(cursorSpot, range, inserted).value_or(track(cursor));
    selectEditor(m_pairs.toEditor(newAnchor), m_pairs.toEditor(newCursor));
    m_projecting = false;
    if ((oldLength > largeTextUnits) != (source.size() > largeTextUnits))
        m_editor->setFlag(QQuickItem::ItemObservesViewport, source.size() > largeTextUnits);
    refreshCursor();
    if (top > 0 && oldEnd <= top) {
        const QRectF line = rectangleAt(m_pairs.toEditor(top + delta));
        scrollView(view, m_editor->y() + line.y() + intoTop);
    }
}

void EditorBinding::applyByLines(QTextCursor &span, const QString &written)
{
    // What the span shows now, a paragraph separator for each line break,
    // and what replaces it, line for line; another number of lines is
    // written whole, as any edit is.
    const QString shown = span.selectedText();
    const QStringList before = shown.split(QChar::ParagraphSeparator);
    QStringList after;
    qsizetype from = 0;
    for (qsizetype i = 0; i < written.size(); ++i) {
        // The breaks Qt makes paragraphs of, as insertText does.
        const QChar c = written.at(i);
        if (c != u'\n' && c != u'\r' && c != QChar::ParagraphSeparator) continue;
        after.append(written.mid(from, i - from));
        if (c == u'\r' && i + 1 < written.size() && written.at(i + 1) == u'\n') ++i;
        from = i + 1;
    }
    after.append(written.mid(from));
    if (before.size() != after.size() || before.size() < 2) {
        span.insertText(written);
        return;
    }
    QTextCursor edit(m_textDocument);
    edit.beginEditBlock();
    int line = span.selectionStart();
    for (qsizetype i = 0; i < before.size(); ++i) {
        const QString &old = before[i], &now = after[i];
        int same = 0;
        const int shortest = int(qMin(old.size(), now.size()));
        while (same < shortest && old[same] == now[same]) ++same;
        int tail = 0;
        while (tail < shortest - same && old[old.size() - 1 - tail] == now[now.size() - 1 - tail]) ++tail;
        if (same + tail < old.size() || same + tail < now.size()) {
            edit.setPosition(line + same);
            edit.setPosition(line + int(old.size()) - tail, QTextCursor::KeepAnchor);
            edit.insertText(now.mid(same, now.size() - same - tail));
        }
        line += int(now.size()) + 1;
    }
    edit.endEditBlock();
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

bool EditorBinding::tableGrids() const { return m_tables->enabled(); }

void EditorBinding::setTableGrids(bool on)
{
    if (on == m_tables->enabled()) return;
    m_tables->setEnabled(on);
    emit tableGridsChanged();
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
    if (m_document) {
        options.newline = m_document->newline();
        const md::Options dialect = m_document->markdownOptions();
        options.footnotes = dialect.footnotes;
        options.inlineNotes = dialect.footnotes && dialect.profile == md::Profile::Writer;
        options.protectCodePipes = dialect.profile == md::Profile::Writer;
        options.mergedCells = dialect.profile == md::Profile::Writer && dialect.mergedCells;
        options.lineComments = dialect.lineComments;
    }
    return options;
}

bool EditorBinding::acts(const QString &command, int position) const
{
    if (!MarkdownEditing::isCommand(command) || !m_document || !m_loaded) return false;
    const QString text = m_buffer->text();
    int anchor = position, cursor = position;
    if (position < 0) {
        const SourceRange range = selection();
        const bool backwards = editorCursor() < editorAnchor();
        anchor = backwards ? range.start + range.length : range.start;
        cursor = backwards ? range.start : range.start + range.length;
    } else if (position > text.size()) {
        return false;
    }
    const std::optional<TextChange> change = MarkdownEditing::format(command, text, anchor, cursor, editingOptions());
    return change && !change->edits.isEmpty();
}

QString EditorBinding::undefinedNoteAt(int position) const
{
    if (!m_document || !m_document->markdownOptions().footnotes) return {};
    return MarkdownEditing::undefinedNoteAt(m_buffer->text(), position);
}

int EditorBinding::undefinedNoteNear(const QString &label, int position) const
{
    if (label.isEmpty() || !m_document || !m_document->markdownOptions().footnotes) return -1;
    const QString text = m_buffer->text();
    if (position >= 0 && position <= text.size() && MarkdownEditing::undefinedNoteAt(text, position) == label)
        return position;
    const QString reference = QStringLiteral("[^") + label + u']';
    int nearest = -1;
    for (qsizetype at = text.indexOf(reference); at >= 0; at = text.indexOf(reference, at + 1)) {
        const int inside = int(at) + 2;
        if (MarkdownEditing::undefinedNoteAt(text, inside) != label) continue;
        if (nearest < 0 || std::abs(inside - position) < std::abs(nearest - position)) nearest = inside;
    }
    return nearest;
}

bool EditorBinding::format(const QString &command)
{
    if (!MarkdownEditing::isCommand(command)) return false;
    const MarkdownEditing::Options options = editingOptions();
    return applyEditing([&](QStringView text, int anchor, int cursor) {
        return MarkdownEditing::format(command, text, anchor, cursor, options);
    });
}

bool EditorBinding::insertTable(int columns, int rows)
{
    const MarkdownEditing::Options options = editingOptions();
    const bool done = applyEditing([&](QStringView text, int, int cursor) {
        return std::optional(MarkdownEditing::insertTable(text, cursor, qBound(1, columns, 64), qBound(1, rows, 1000), options));
    });
    // A table put in here is a grid at once, the caret in its first cell.
    if (done) m_tables->adoptAt(m_cursorSource);
    return done;
}

bool EditorBinding::applyEditing(const std::function<std::optional<TextChange>(QStringView, int, int)> &operation)
{
    if (!editable() || m_projecting || m_blocked) return false;
    cancelComposition();
    // Over a block of a grid's cells, the command is made on each.
    if (const std::optional<bool> done = m_tables->formatCells(operation)) return *done;
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

bool EditorBinding::apply(const TextChange &requested)
{
    if (!editable() || m_projecting || m_blocked) return false;
    // A command's edits in a grid's cells write the table tidy around them;
    // one that would break a table's rows is refused.
    TextChange change = requested;
    if (!change.edits.isEmpty()) {
        const TableGrids::Adapted adapted = m_tables->adapt(change.edits, change.anchor, change.cursor);
        if (adapted.kind == TableGrids::Adapted::Refused) {
            emit m_tables->refused(adapted.reason);
            return false;
        }
        if (adapted.kind == TableGrids::Adapted::Changed) {
            change.edits = adapted.edits;
            change.anchor = adapted.anchor;
            change.cursor = adapted.cursor;
        }
    }
    if (!change.edits.isEmpty() && m_buffer->replace(change.edits) == SourceBuffer::EditOutcome::Rejected) return false;
    const int length = m_buffer->length();
    selectEditor(m_pairs.toEditor(qBound(0, change.anchor, length)), m_pairs.toEditor(qBound(0, change.cursor, length)));
    refreshCursor();
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
    m_tables->relayout();
}

void EditorBinding::checkNativeChange(int position, int removed, int added)
{
    if (m_projecting || !m_loaded || m_resyncPending || (removed == 0 && added == 0)) return;
    // A composition event the binding sends makes Qt redraw the preedit's
    // paragraph, reported as a change of equal length; that needs no
    // comparing on every key of an input method. A change of length then is
    // Qt's own doing, which the binding never asked for, so it is not taken
    // in: the view shows the source again.
    if (m_dispatching) {
        if (removed != added) scheduleResync();
        return;
    }
    // Whatever Qt reports while a write waits belongs to it.
    if (m_foreign.pending) {
        noteForeignWrite(position, removed, added);
        return;
    }
    // Qt reports its own formatting, the preedit among it, as a change of
    // equal length; only a change of the characters is a write past the source.
    if (removed == added) {
        const int shownLength = m_textDocument->characterCount() - 1;
        if (shownLength != m_pairs.toEditor(m_buffer->length())) {
            // Then the projection no longer follows the source at all.
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
    noteForeignWrite(position, removed, added);
}

void EditorBinding::noteForeignWrite(int position, int removed, int added)
{
    ForeignWrite &write = m_foreign;
    if (!write.pending) {
        // The selection was last followed before the write moved Qt's.
        write = {true, position, position + removed, position + added, m_anchorSource, m_cursorSource};
        // Qt is still inside its edit, which can go on with more changes, so
        // the write is taken in afterwards, before anything else that waits.
        QMetaObject::invokeMethod(this, &EditorBinding::takeForeignWrite, Qt::QueuedConnection);
        return;
    }
    // One span covers both: positions before it are where they were, and
    // positions after it are shifted by what it changed so far.
    const int end = qMax(write.newEnd, position + removed);
    write.oldEnd += end - write.newEnd;
    write.newEnd = end + added - removed;
    write.start = qMin(write.start, position);
}

void EditorBinding::takeForeignWrite()
{
    if (!m_foreign.pending) return;
    const ForeignWrite write = std::exchange(m_foreign, {});
    if (!m_loaded || !m_textDocument || !m_editor) return;
    const int oldShown = m_pairs.toEditor(m_buffer->length());
    const int newShown = m_textDocument->characterCount() - 1;
    // Qt can count the separator it keeps after the last paragraph, as when
    // clearing; the span stays inside the text, and is all of it when what it
    // changed does not add up.
    int start = qBound(0, write.start, qMin(oldShown, newShown));
    int oldEnd = qBound(start, write.oldEnd, oldShown);
    int newEnd = qBound(start, write.newEnd, newShown);
    if (newEnd - oldEnd != newShown - oldShown) {
        start = 0;
        oldEnd = oldShown;
        newEnd = newShown;
    }
    QTextCursor span(m_textDocument);
    span.setPosition(start);
    span.setPosition(newEnd, QTextCursor::KeepAnchor);
    const QString written = span.selectedText();
    const int sourceStart = m_pairs.toSource(start);
    const int sourceEnd = m_pairs.toSource(oldEnd);
    SourceRange range;
    QString replacement;
    {
        // The copy of the text is gone before the edit, or the buffer would copy all of it to change it.
        const QString source = m_buffer->text();
        const QString before = source.mid(sourceStart, sourceEnd - sourceStart);
        m_projecting = true;
        // A composition is dropped, as for any edit; nothing it showed was written.
        const bool composed = m_composing;
        const SourceRange composition = m_compositionSelection;
        cancelComposition();
        // The span shows the source again, its paragraphs with the line
        // height, which a write that cleared the text took from them; Qt
        // makes the separators of the source's line breaks as it always does.
        span.insertText(before);
        span.setPosition(start);
        span.setPosition(oldEnd, QTextCursor::KeepAnchor);
        span.mergeBlockFormat(lineFormat());
        // The selection from before the write, which the edit moves as it moves every view's.
        const int length = int(source.size());
        const int anchor = composed ? composition.start : write.anchor;
        const int cursor = composed ? composition.start + composition.length : write.cursor;
        selectEditor(m_pairs.toEditor(qBound(0, anchor, length)), m_pairs.toEditor(qBound(0, cursor, length)));
        m_projecting = false;
        refreshCursor();
        // Only what the write changed is edited: what it left as the source
        // shows it is trimmed from both ends.
        QString shown;
        shown.reserve(before.size());
        for (qsizetype i = 0; i < before.size(); ++i) {
            const QChar c = before.at(i);
            if (c == u'\r' && i + 1 < before.size() && before.at(i + 1) == u'\n') ++i;
            shown += showsAsSeparator(c) ? QChar(QChar::ParagraphSeparator) : c;
        }
        qsizetype head = 0;
        const qsizetype common = qMin(shown.size(), written.size());
        while (head < common && shown.at(head) == written.at(head)) ++head;
        qsizetype tail = 0;
        while (tail < common - head && shown.at(shown.size() - 1 - tail) == written.at(written.size() - 1 - tail))
            ++tail;
        if (head == shown.size() && head == written.size()) return;
        const int from = m_pairs.toSource(start + int(head));
        const int to = m_pairs.toSource(oldEnd - int(tail));
        // Qt shows every line break it was given as a paragraph separator;
        // the source takes the document's own.
        QString added = written.mid(head, written.size() - head - tail);
        added.replace(QChar(QChar::ParagraphSeparator), u'\n');
        added = withDocumentBreaks(added);
        // An edit inside a grapheme takes the whole grapheme, with the
        // units of it the write left as they were; one that would leave half
        // a surrogate pair goes to whole graphemes instead, as the TextEdit's
        // own insert and remove do.
        const int first = graphemeAtOrBefore(source, from);
        const int last = graphemeAtOrAfter(source, to);
        range = {first, last - first};
        replacement = source.mid(first, from - first) + added + source.mid(to, last - to);
        if (!wellFormed(replacement)) {
            range = {first, (to == from ? first : last) - first};
            replacement = added;
        }
    }
    // As the TextEdit's own methods, this leaves the caret to follow the edit;
    // a view that takes no edits now refuses it and goes on showing the source.
    replace(range, replacement, EditKind::Other, false);
}

void EditorBinding::scheduleResync()
{
    // Something wrote into the TextEdit that cannot be followed into the
    // source, so the view shows the source again. Qt is still inside its own
    // edit here, so the document is rebuilt afterwards.
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
    // While a write waits, the projection is ahead of the source and its
    // positions map to nothing yet.
    if (!m_loaded || m_foreign.pending) return;
    const int cursor = m_pairs.toSource(editorCursor());
    const int anchor = m_pairs.toSource(editorAnchor());
    if (cursor == m_cursorSource && anchor == m_anchorSource) return;
    m_cursorSource = cursor;
    m_anchorSource = anchor;
    emit cursorChanged();
    m_tables->cursorMoved();
}

void EditorBinding::editorCursorMoved()
{
    // A caret set from outside, as assistive technology sets one, is followed
    // as a click's is; one that a waiting write moved is placed once it is taken in.
    if (m_projecting || m_foreign.pending) return;
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
    m_placedCaret = -1;
    // An edit that reaches into a grid is made an edit of its cell, the
    // table written tidy around it, or refused.
    const TableGrids::Adapted adapted = m_tables->adapt(range, text);
    if (adapted.kind == TableGrids::Adapted::Refused) {
        emit m_tables->refused(adapted.reason);
        return false;
    }
    if (adapted.kind == TableGrids::Adapted::Changed) {
        // Keys typed or deleted one after another in a cell are one step to
        // undo, as in a paragraph, though each writes the table anew.
        quint64 run = 0;
        if (kind != EditKind::Other) {
            static quint64 runs = 0;
            const bool goesOn = kind == m_tableRunKind
                                && (range.start == m_tableRunCaret || range.start + range.length == m_tableRunCaret);
            if (!goesOn) m_tableRun = ++runs;
            run = m_tableRun;
        }
        if (!adapted.edits.isEmpty()
            && m_buffer->replace(adapted.edits, kind, run) == SourceBuffer::EditOutcome::Rejected)
            return false;
        m_tableRunCaret = kind != EditKind::Other ? adapted.cursor : -1;
        m_tableRunKind = kind;
        if (m_keepCaret) return true;
        m_placedCaret = adapted.cursor;
        const int caret = m_pairs.toEditor(qBound(0, adapted.cursor, m_buffer->length()));
        selectEditor(caret, caret);
        refreshCursor();
        revealCursor();
        return true;
    }
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
    const bool undoing = std::exchange(m_undoing, true);
    m_buffer->undo();
    m_undoing = undoing;
}

void EditorBinding::redo()
{
    if (!editable() || m_blocked) return;
    cancelComposition();
    const bool undoing = std::exchange(m_undoing, true);
    m_buffer->redo();
    m_undoing = undoing;
}

void EditorBinding::copy()
{
    if (!m_loaded) return;
    // A block of a grid's cells goes as cells.
    if (m_tables->copyBlock()) return;
    // The source's own text, not Qt's, which turns CRLF into LF and NBSP into spaces.
    const SourceRange range = selection();
    if (range.length > 0) QGuiApplication::clipboard()->setText(m_buffer->text().mid(range.start, range.length));
}

void EditorBinding::cut()
{
    if (!editable()) return;
    cancelComposition();
    if (m_tables->cutBlock()) return;
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
    m_preedit.clear();
    m_tables->compositionChanged();
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
    // In a grid the table was written anew around the commit, and the grids placed the caret.
    if (m_placedCaret >= 0) return true;
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
        // A grid draws the composition in its cell.
        if (preedit != m_preedit) {
            m_preedit = preedit;
            m_tables->compositionChanged();
        }
    }
    revealCursor();
}

bool EditorBinding::handleKey(QKeyEvent *key)
{
    // Keys that move the caret in a grid move it from cell to cell.
    if (m_tables->key(key)) {
        key->accept();
        return true;
    }
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
        if (event->type() == QEvent::ShortcutOverride && !m_blocked
            && m_vim->takesShortcut(static_cast<QKeyEvent *>(event))) {
            event->accept();
            return true;
        }
        // A write into the projection that waits is taken in first, so that
        // the key or the input method acts on the text it left.
        if (m_foreign.pending && (event->type() == QEvent::InputMethod || event->type() == QEvent::KeyPress))
            takeForeignWrite();
        if (event->type() == QEvent::InputMethod) {
            auto *input = static_cast<QInputMethodEvent *>(event);
            // Outside Insert mode, Vim keys take what an input method sends as keys.
            if (!m_vim->handleInputMethod(input)) handleInputMethod(input);
            return true;
        }
        if (event->type() == QEvent::InputMethodQuery && m_tables->caretInGrid()) {
            // The caret in a grid is where the grid draws it, which the input
            // method places its window by; Qt answers the rest.
            auto *query = static_cast<QInputMethodQueryEvent *>(event);
            m_dispatching = true;
            QCoreApplication::sendEvent(m_editor, query);
            m_dispatching = false;
            if (const auto caret = m_tables->rectangleFor(m_cursorSource)) {
                if (query->queries() & Qt::ImCursorRectangle) query->setValue(Qt::ImCursorRectangle, *caret);
                if (query->queries() & Qt::ImAnchorRectangle) {
                    const auto anchor = m_tables->rectangleFor(m_anchorSource);
                    query->setValue(Qt::ImAnchorRectangle, anchor ? *anchor : *caret);
                }
            }
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

void EditorBinding::scrollView(QQuickItem *view, qreal y) const
{
    // The end of the text stays exactly where Qt puts it.
    const qreal maximum = qMax<qreal>(0, view->property("contentHeight").toReal() - view->height());
    view->setProperty("contentY", std::min(std::round(qMax<qreal>(0, y)), maximum));
}

void EditorBinding::revealCursor()
{
    QQuickItem *view = viewport();
    if (!view || !m_loaded) return;
    const QRectF caret = rectangleAt(editorCursor()).translated(m_editor->position());
    const qreal top = view->property("contentY").toReal();
    const qreal height = view->height();
    qreal target = top;
    if (caret.top() < top) target = std::floor(caret.top());
    else if (caret.bottom() > top + height) target = std::ceil(caret.bottom() - height);
    if (target == top) return;
    scrollView(view, target);
}

void EditorBinding::keepCursorInView(qreal formerHeight)
{
    QQuickItem *view = viewport();
    if (!view || !m_loaded) return;
    const QRectF caret = rectangleAt(editorCursor()).translated(m_editor->position());
    const qreal top = view->property("contentY").toReal();
    if (caret.top() < top || caret.bottom() > top + formerHeight) return;
    revealCursor();
}

QRectF EditorBinding::caretRectangle() const
{
    if (!m_loaded || !m_editor) return {};
    return rectangleAt(editorCursor());
}

QRectF EditorBinding::sourceRectangle(int source) const
{
    if (!m_loaded || !m_editor) return {};
    return rectangleAt(m_pairs.toEditor(qBound(0, source, m_buffer->length())));
}

QRectF EditorBinding::rectangleAt(int editorPosition) const
{
    if (m_loaded)
        if (const auto inGrid = m_tables->rectangleFor(m_pairs.toSource(editorPosition))) return *inGrid;
    if (editorPosition == editorCursor()) return m_cursorRectangle.read(m_editor).toRectF();
    QRectF rect;
    m_positionToRectangle.invoke(m_editor, Q_RETURN_ARG(QRectF, rect), Q_ARG(int, editorPosition));
    return rect;
}

QTextDocument *EditorBinding::textDocument() const { return m_loaded ? m_textDocument.data() : nullptr; }

SourceRange EditorBinding::visibleRange(qreal around) const
{
    if (!m_loaded) return {0, 0};
    QQuickItem *view = viewport();
    // Without a view that scrolls, the whole text is in view.
    if (!view) return {0, m_buffer->length()};
    const qreal margin = qMax<qreal>(0, around) * view->height();
    const qreal top = view->property("contentY").toReal() - m_editor->y() - margin;
    int first = 0, last = 0;
    m_positionAt.invoke(m_editor, Q_RETURN_ARG(int, first), Q_ARG(qreal, 0), Q_ARG(qreal, qMax<qreal>(0, top)));
    m_positionAt.invoke(m_editor, Q_RETURN_ARG(int, last), Q_ARG(qreal, m_editor->width()),
                        Q_ARG(qreal, qMax<qreal>(0, top + view->height() + 2 * margin)));
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
    const QRectF line = rectangleAt(m_pairs.toEditor(position));
    // The first line keeps the margin above it in view.
    scrollView(view, line.y() <= 0 ? 0 : m_editor->y() + line.y());
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
