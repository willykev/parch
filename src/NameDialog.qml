import QtQuick
import QtQuick.Controls.Basic
import Parch

// Asks for a name: for a new document or folder, or for one renamed. What
// is wrong with a name is said beneath the field as it is typed, and the
// choice waits until the name is one a file can have. Enter chooses, and
// Escape or Cancel leaves everything as it was.
ThemedDialog {
    id: dialog

    // What the choice says, such as Rename or Make.
    property string action
    // What the name is for, said beneath the field.
    property string hint
    // Why the name typed cannot be used; empty when it can.
    readonly property string problem: field.text.length === 0 ? "" : backend.organizer.nameProblem(field.text)

    // The name chosen.
    signal chosen(string name)

    // Opens with a name, its part before the last dot selected, so that
    // typing replaces the name and keeps its ending.
    function ask(title, action, name, hint) {
        dialog.title = title
        dialog.action = action
        dialog.hint = hint || ""
        field.text = name
        open()
        const dot = name.lastIndexOf(".")
        field.select(0, dot > 0 ? dot : name.length)
    }

    function choose() {
        if (field.text.trim().length === 0 || dialog.problem.length > 0)
            return
        const name = field.text
        dialog.accept()
        dialog.chosen(name)
    }

    preferredCharacters: 48
    initialFocus: field

    contentItem: Column {
        spacing: Math.round(theme.fontSize / 2)

        TextField {
            id: field
            objectName: "nameField"
            width: parent.width
            height: Math.round(theme.fontSize * 2.4)
            leftPadding: Math.round(theme.fontSize / 2)
            rightPadding: Math.round(theme.fontSize / 2)
            topPadding: 0
            bottomPadding: 0
            placeholderText: qsTr("Name")
            placeholderTextColor: theme.dim
            color: theme.foreground
            selectionColor: theme.selection
            selectedTextColor: theme.foreground
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            verticalAlignment: TextInput.AlignVCenter
            selectByMouse: true
            cursorDelegate: FieldCaret { rest: fieldCaret }
            Accessible.name: dialog.title.length > 0 ? dialog.title : placeholderText
            Accessible.description: dialog.problem.length > 0 ? dialog.problem : dialog.hint
            Keys.onReturnPressed: dialog.choose()
            Keys.onEnterPressed: dialog.choose()

            CaretRest {
                id: fieldCaret
                objectName: "caretRest"
                target: field
                steady: theme.reduceMotion
            }

            background: Rectangle {
                radius: theme.radius
                color: theme.background
                border.width: Math.max(1, theme.controlBorderWidth)
                border.color: dialog.problem.length > 0 ? theme.danger : field.activeFocus ? theme.accent : theme.controlBorder
            }
        }

        Text {
            objectName: "nameProblem"
            width: parent.width
            text: dialog.problem.length > 0 ? dialog.problem : dialog.hint
            visible: text.length > 0
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.small
            color: dialog.problem.length > 0 ? theme.danger : theme.dim
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            text: qsTr("Cancel")
            onClicked: dialog.reject()
        }
        ThemedDialog.Choice {
            objectName: "nameChoose"
            text: dialog.action
            enabled: field.text.trim().length > 0 && dialog.problem.length === 0
            onClicked: dialog.choose()
        }
    }
}
