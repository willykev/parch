#pragma once

// The binding and the preview are properties QML sets, so their types must
// be complete here.
#include "editorbinding.h"
#include "preview.h"

#include <QObject>
#include <QPointer>
#include <QQuickItem>
#include <QTimer>

// Keeps an editor and the preview beside it at the same place in the text.
// Places are matched by the source, never by how far down either view is:
// the block of the source at the top of one view is found, and the other
// view is scrolled so that the same block is at its top, as far into the
// block as the first view is. At either end of one view the other is at
// its end too.
//
// The view that is scrolled leads, and the other follows it. A view that is
// following does not lead back: what it moves by, and the moves it makes
// while it settles, are not taken for scrolling of its own until the
// leader has rested for HoldMs. When the preview's text changes, the view
// that led last leads again.
class ScrollSync : public QObject {
    Q_OBJECT
    Q_PROPERTY(Preview *preview READ preview WRITE setPreview NOTIFY changed)
    Q_PROPERTY(EditorBinding *binding READ binding WRITE setBinding NOTIFY changed)
    // The Flickable the editor scrolls in.
    Q_PROPERTY(QQuickItem *editorViewport READ editorViewport WRITE setEditorViewport NOTIFY changed)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY changed)

public:
    static constexpr int HoldMs = 160;

    explicit ScrollSync(QObject *parent = nullptr);

    Preview *preview() const { return m_preview; }
    void setPreview(Preview *preview);
    EditorBinding *binding() const { return m_binding; }
    void setBinding(EditorBinding *binding);
    QQuickItem *editorViewport() const { return m_editorViewport; }
    void setEditorViewport(QQuickItem *viewport);
    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    // Scrolls the preview to where the editor is, and the editor to where
    // the preview is, whatever led last.
    Q_INVOKABLE void followEditor();
    Q_INVOKABLE void followPreview();
    // How many times each view was scrolled to follow the other.
    int previewFollowed() const { return m_previewFollowed; }
    int editorFollowed() const { return m_editorFollowed; }

signals:
    void changed();

private slots:
    void editorMoved();
    void previewMoved();

private:
    enum class Leader { None, Editor, Preview };
    bool ready() const;
    // Where a source position is in the editor's Flickable.
    qreal editorHeightOf(int source) const;
    void watchPreviewViewport();
    void previewUpdated();

    QPointer<Preview> m_preview;
    QPointer<EditorBinding> m_binding;
    QPointer<QQuickItem> m_editorViewport;
    QPointer<QQuickItem> m_previewViewport;
    bool m_enabled = false;
    bool m_applying = false;
    Leader m_leader = Leader::None;
    QTimer m_hold;
    int m_previewFollowed = 0;
    int m_editorFollowed = 0;
};
