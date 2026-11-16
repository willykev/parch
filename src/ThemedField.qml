import QtQuick
import QtQuick.Controls.Basic
import Parch

// A one-line text field in a dialog: a fine edge that takes the accent
// while the field has the keys, and a caret that rests as the editor's does.
// Its placeholder is its accessible name unless it is given another.
TextField {
    id: field

    implicitHeight: Math.round(theme.fontSize * 2.4)
    leftPadding: Math.round(theme.fontSize * 2 / 3)
    rightPadding: Math.round(theme.fontSize * 2 / 3)
    topPadding: 0
    bottomPadding: 0
    placeholderTextColor: theme.dim
    color: theme.foreground
    selectionColor: theme.selection
    selectedTextColor: theme.foreground
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize
    verticalAlignment: TextInput.AlignVCenter
    cursorDelegate: FieldCaret { rest: fieldCaret }
    Accessible.name: placeholderText

    CaretRest {
        id: fieldCaret
        objectName: "caretRest"
        target: field
        steady: theme.reduceMotion
    }

    background: Rectangle {
        radius: theme.radius
        color: field.hovered && !field.activeFocus ? theme.hoverFill : theme.background
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: field.activeFocus ? theme.accent : theme.controlBorder
    }
}
