#include "scrollsync.h"

#include "sourcebuffer.h"

#include <QRectF>
#include <algorithm>
#include <cmath>
#include <limits>

ScrollSync::ScrollSync(QObject *parent) : QObject(parent)
{
    m_hold.setSingleShot(true);
    m_hold.setInterval(HoldMs);
}

void ScrollSync::setPreview(Preview *preview)
{
    if (preview == m_preview) return;
    if (m_preview) disconnect(m_preview, nullptr, this, nullptr);
    m_preview = preview;
    if (preview) {
        connect(preview, &Preview::viewportChanged, this, &ScrollSync::watchPreviewViewport);
        connect(preview, &Preview::updated, this, &ScrollSync::previewUpdated);
        connect(preview, &Preview::laidOut, this, &ScrollSync::previewUpdated);
    }
    watchPreviewViewport();
    emit changed();
}

void ScrollSync::watchPreviewViewport()
{
    QQuickItem *viewport = m_preview ? m_preview->viewport() : nullptr;
    if (viewport == m_previewViewport) return;
    if (m_previewViewport) disconnect(m_previewViewport, nullptr, this, nullptr);
    m_previewViewport = viewport;
    if (viewport) connect(viewport, SIGNAL(contentYChanged()), this, SLOT(previewMoved()));
}

void ScrollSync::setBinding(EditorBinding *binding)
{
    if (binding == m_binding) return;
    m_binding = binding;
    emit changed();
}

void ScrollSync::setEditorViewport(QQuickItem *viewport)
{
    if (viewport == m_editorViewport) return;
    if (m_editorViewport) disconnect(m_editorViewport, nullptr, this, nullptr);
    m_editorViewport = viewport;
    if (viewport) connect(viewport, SIGNAL(contentYChanged()), this, SLOT(editorMoved()));
    emit changed();
}

void ScrollSync::setEnabled(bool enabled)
{
    if (enabled == m_enabled) return;
    m_enabled = enabled;
    emit changed();
    if (enabled) {
        m_leader = Leader::Editor;
        followEditor();
    }
}

bool ScrollSync::ready() const
{
    return m_preview && m_binding && m_binding->editor() && m_binding->document() && m_editorViewport && m_previewViewport
           && m_preview->surface() && !m_preview->working();
}

qreal ScrollSync::editorHeightOf(int source) const
{
    QQuickItem *editor = m_binding->editor();
    const int length = m_binding->document()->buffer()->length();
    const int position = m_binding->sourceToEditor(std::clamp(source, 0, length));
    QRectF line;
    QMetaObject::invokeMethod(editor, "positionToRectangle", Q_RETURN_ARG(QRectF, line), Q_ARG(int, std::max(0, position)));
    return editor->y() + line.y();
}

namespace {
qreal most(QQuickItem *viewport)
{
    return std::max<qreal>(0, viewport->property("contentHeight").toReal() - viewport->height());
}
} // namespace

void ScrollSync::followEditor()
{
    if (!m_enabled || !ready()) return;
    const qreal contentY = m_editorViewport->property("contentY").toReal();
    const qreal editorMost = most(m_editorViewport), previewMost = most(m_previewViewport);
    qreal target = 0;
    if (contentY <= 0.5) {
        target = 0;
    } else if (editorMost > 0 && contentY >= editorMost - 0.5) {
        target = previewMost;
    } else {
        const Preview::Span span = m_preview->spanOfSource(m_binding->topSourcePosition());
        if (!span.valid) return;
        const qreal from = editorHeightOf(span.sourceStart);
        const qreal to = span.sourceEnd == std::numeric_limits<int>::max()
                             ? m_editorViewport->property("contentHeight").toReal()
                             : editorHeightOf(span.sourceEnd);
        const qreal part = to > from ? std::clamp((contentY - from) / (to - from), qreal(0), qreal(1)) : 0;
        // From the surface's coordinates to the Flickable's.
        const qreal offset = m_previewViewport->property("contentY").toReal() - m_preview->viewTop();
        target = std::clamp(std::round(span.top + part * (span.bottom - span.top) + offset), qreal(0), previewMost);
    }
    if (std::abs(target - m_previewViewport->property("contentY").toReal()) < 1) return;
    m_applying = true;
    m_previewViewport->setProperty("contentY", target);
    m_applying = false;
    ++m_previewFollowed;
}

void ScrollSync::followPreview()
{
    if (!m_enabled || !ready()) return;
    const qreal contentY = m_previewViewport->property("contentY").toReal();
    const qreal editorMost = most(m_editorViewport), previewMost = most(m_previewViewport);
    qreal target = 0;
    if (contentY <= 0.5) {
        target = 0;
    } else if (previewMost > 0 && contentY >= previewMost - 0.5) {
        target = editorMost;
    } else {
        const qreal top = m_preview->viewTop();
        const Preview::Span span = m_preview->spanAtHeight(top);
        if (!span.valid) return;
        const qreal part = span.bottom > span.top ? std::clamp((top - span.top) / (span.bottom - span.top), qreal(0), qreal(1)) : 0;
        const qreal from = editorHeightOf(span.sourceStart);
        const qreal to = span.sourceEnd == std::numeric_limits<int>::max()
                             ? m_editorViewport->property("contentHeight").toReal()
                             : editorHeightOf(span.sourceEnd);
        target = std::clamp(std::round(from + part * std::max<qreal>(0, to - from)), qreal(0), editorMost);
    }
    if (std::abs(target - m_editorViewport->property("contentY").toReal()) < 1) return;
    m_applying = true;
    m_editorViewport->setProperty("contentY", target);
    m_applying = false;
    ++m_editorFollowed;
}

void ScrollSync::editorMoved()
{
    if (!m_enabled || m_applying) return;
    // While the preview leads, what the editor does is following it.
    if (m_leader == Leader::Preview && m_hold.isActive()) return;
    m_leader = Leader::Editor;
    m_hold.start();
    followEditor();
}

void ScrollSync::previewMoved()
{
    if (!m_enabled || m_applying) return;
    if (m_leader == Leader::Editor && m_hold.isActive()) return;
    m_leader = Leader::Preview;
    m_hold.start();
    followPreview();
}

// The preview shows other text, or the same text laid out otherwise: the
// view that led last leads again, so that the other is where it belongs.
void ScrollSync::previewUpdated()
{
    if (!m_enabled) return;
    if (m_leader == Leader::Preview) {
        followPreview();
    } else {
        m_leader = Leader::Editor;
        m_hold.start();
        followEditor();
    }
}
