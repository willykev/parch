#include "editoraccessible.h"

#include "editorbinding.h"

#include <QtQuick/private/qquicktextedit_p.h>
#include <mutex>

namespace {
// The binding whose editor a TextEdit is, as the factory finds it.
const char *const bindingProperty = "_parchEditorBinding";

QAccessibleInterface *editorFactory(const QString &, QObject *object)
{
    auto *editor = qobject_cast<QQuickTextEdit *>(object);
    if (!editor) return nullptr;
    auto *binding = qobject_cast<EditorBinding *>(editor->property(bindingProperty).value<QObject *>());
    if (!binding) return nullptr;
    return new EditorAccessible(editor, binding);
}
} // namespace

EditorAccessible::EditorAccessible(QQuickTextEdit *editor, EditorBinding *binding)
    : QAccessibleQuickTextEdit(editor), m_binding(binding)
{
}

void EditorAccessible::serve(QQuickItem *editor, EditorBinding *binding)
{
    if (!qobject_cast<QQuickTextEdit *>(editor)) return;
    // Consulted before Qt Quick's own factory, which was installed earlier.
    static std::once_flag installed;
    std::call_once(installed, [] { QAccessible::installFactory(editorFactory); });
    editor->setProperty(bindingProperty, QVariant::fromValue<QObject *>(binding));
    if (!QAccessible::isActive()) return;
    // An interface made for the editor before it had its binding makes way.
    QAccessibleInterface *made = QAccessible::queryAccessibleInterface(editor);
    if (made && !dynamic_cast<EditorAccessible *>(made))
        QAccessible::deleteAccessibleInterface(QAccessible::uniqueId(made));
}

bool EditorAccessible::bound() const
{
    // Only while the binding still projects into this editor; otherwise the
    // TextEdit is Qt's own again.
    return m_binding && m_binding->editor() == object();
}

int EditorAccessible::sourceOf(int offset) const
{
    return m_binding->editorToSource(qBound(0, offset, characterCount()));
}

void EditorAccessible::setText(QAccessible::Text kind, const QString &text)
{
    if (kind != QAccessible::Value || !bound()) QAccessibleQuickTextEdit::setText(kind, text);
    else m_binding->replaceEditorRange(0, characterCount(), text);
}

void EditorAccessible::insertText(int offset, const QString &text)
{
    if (!bound()) QAccessibleQuickTextEdit::insertText(offset, text);
    else m_binding->replaceEditorRange(offset, offset, text);
}

void EditorAccessible::deleteText(int startOffset, int endOffset)
{
    if (!bound()) QAccessibleQuickTextEdit::deleteText(startOffset, endOffset);
    else m_binding->replaceEditorRange(startOffset, endOffset, QString());
}

void EditorAccessible::replaceText(int startOffset, int endOffset, const QString &text)
{
    if (!bound()) QAccessibleQuickTextEdit::replaceText(startOffset, endOffset, text);
    else m_binding->replaceEditorRange(startOffset, endOffset, text);
}

void EditorAccessible::setCursorPosition(int position)
{
    if (!bound()) {
        QAccessibleQuickTextEdit::setCursorPosition(position);
        return;
    }
    const int source = sourceOf(position);
    if (source >= 0) m_binding->select(source, source);
}

void EditorAccessible::setSelection(int selectionIndex, int startOffset, int endOffset)
{
    if (!bound()) {
        QAccessibleQuickTextEdit::setSelection(selectionIndex, startOffset, endOffset);
        return;
    }
    // The editor has one selection.
    if (selectionIndex != 0) return;
    const int anchor = sourceOf(startOffset), cursor = sourceOf(endOffset);
    if (anchor >= 0 && cursor >= 0) m_binding->select(anchor, cursor);
}

void EditorAccessible::addSelection(int startOffset, int endOffset)
{
    setSelection(0, startOffset, endOffset);
}

void EditorAccessible::removeSelection(int selectionIndex)
{
    if (!bound()) {
        QAccessibleQuickTextEdit::removeSelection(selectionIndex);
        return;
    }
    if (selectionIndex != 0) return;
    const int caret = m_binding->cursorSource();
    m_binding->select(caret, caret);
}
