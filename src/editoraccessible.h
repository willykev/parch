#pragma once

#include <QAccessible>
#include <QPointer>
#include <QString>
#include <QtQuick/private/qaccessiblequicktextedit_p.h>

class EditorBinding;
class QQuickItem;

// What assistive technology reaches the editor's text through: Qt's own
// interface for a TextEdit, whose edits, caret and selection go through the
// view's EditorBinding instead, so that an edit made this way, as AT-SPI's
// insert_text, is an edit of the source, one step to undo, and never a
// write into the projection that the binding would only take back.
class EditorAccessible : public QAccessibleQuickTextEdit {
public:
    EditorAccessible(QQuickTextEdit *editor, EditorBinding *binding);

    void setText(QAccessible::Text kind, const QString &text) override;
    void insertText(int offset, const QString &text) override;
    void deleteText(int startOffset, int endOffset) override;
    void replaceText(int startOffset, int endOffset, const QString &text) override;
    void setCursorPosition(int position) override;
    void setSelection(int selectionIndex, int startOffset, int endOffset) override;
    void addSelection(int startOffset, int endOffset) override;
    void removeSelection(int selectionIndex) override;

    // Makes the editor of this binding reached through an EditorAccessible
    // from now on, in place of an interface made for it before.
    static void serve(QQuickItem *editor, EditorBinding *binding);

private:
    bool bound() const;
    // A place in the shown text as a source offset; -1 for none.
    int sourceOf(int offset) const;

    QPointer<EditorBinding> m_binding;
};
