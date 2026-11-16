import QtQuick
import QtQuick.Controls.Basic
import Parch

// Asks where in the library a document or folder moves to: the library's
// folders, indented as the library shows them and narrowed by what is
// typed in the field, leaving out the folder it is in and, for a folder,
// itself and what it holds. Up and Down move from the field, and Enter or
// Move here moves it.
ThemedDialog {
    id: dialog

    // What moves.
    property string path
    readonly property string name: path.substring(path.lastIndexOf("/") + 1)
    readonly property string folder: path.substring(0, path.lastIndexOf("/"))

    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)
    readonly property real indent: Math.round(theme.fontSize * 1.2)

    // The folder chosen.
    signal chosen(string folder)

    // The folders it can go to, as the organizer listed them.
    readonly property var choices: {
        const kept = []
        const typed = field.text.toLowerCase()
        for (const entry of backend.organizer.folders) {
            if (entry.path === dialog.folder || entry.path === dialog.path || entry.path.startsWith(dialog.path + "/"))
                continue
            if (typed.length > 0 && entry.label.toLowerCase().indexOf(typed) < 0)
                continue
            kept.push(entry)
        }
        return kept
    }

    function ask(path) {
        dialog.path = path
        field.text = ""
        list.currentIndex = 0
        backend.organizer.listFolders()
        open()
    }

    function choose(index) {
        if (index < 0 || index >= choices.length)
            return
        const folder = choices[index].path
        dialog.accept()
        dialog.chosen(folder)
    }

    function move(rows) {
        if (choices.length === 0)
            return
        list.currentIndex = Math.max(0, Math.min(choices.length - 1, list.currentIndex + rows))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
    }

    title: qsTr("Move “%1” to…").arg(name)
    preferredCharacters: 56
    preferredHeight: parent ? Math.round(parent.height * 0.7) : implicitHeight
    initialFocus: field

    contentItem: Item {
        implicitHeight: field.height + dialog.rowHeight * 8

        TextField {
            id: field
            objectName: "moveFilter"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.round(theme.fontSize * 2.4)
            leftPadding: Math.round(theme.fontSize / 2)
            rightPadding: Math.round(theme.fontSize / 2)
            topPadding: 0
            bottomPadding: 0
            placeholderText: qsTr("Find a folder")
            placeholderTextColor: theme.dim
            color: theme.foreground
            selectionColor: theme.selection
            selectedTextColor: theme.foreground
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            verticalAlignment: TextInput.AlignVCenter
            cursorDelegate: FieldCaret { rest: fieldCaret }
            Accessible.name: placeholderText
            Accessible.searchEdit: true
            onTextChanged: list.currentIndex = 0

            CaretRest {
                id: fieldCaret
                objectName: "caretRest"
                target: field
                steady: theme.reduceMotion
            }

            Keys.onPressed: (event) => {
                switch (event.key) {
                case Qt.Key_Down:
                    dialog.move(1)
                    break
                case Qt.Key_Up:
                    dialog.move(-1)
                    break
                case Qt.Key_Return:
                case Qt.Key_Enter:
                    dialog.choose(list.currentIndex)
                    break
                default:
                    return
                }
                event.accepted = true
            }

            background: Rectangle {
                radius: theme.radius
                color: theme.background
                border.width: Math.max(1, theme.controlBorderWidth)
                border.color: field.activeFocus ? theme.accent : theme.controlBorder
            }
        }

        ListView {
            id: list
            objectName: "moveFolders"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: field.bottom
            anchors.topMargin: Math.round(theme.fontSize / 2)
            anchors.bottom: parent.bottom
            clip: true
            model: dialog.choices
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            activeFocusOnTab: count > 0
            keyNavigationEnabled: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Folders")
            Keys.onReturnPressed: dialog.choose(currentIndex)
            Keys.onEnterPressed: dialog.choose(currentIndex)
            Keys.onSpacePressed: dialog.choose(currentIndex)

            delegate: AbstractButton {
                id: entry
                objectName: "moveFolder"

                required property int index
                required property var modelData

                readonly property bool current: ListView.isCurrentItem

                width: list.width
                height: dialog.rowHeight
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                leftPadding: Math.round(theme.fontSize / 2) + (field.text.length > 0 ? 0 : modelData.depth * dialog.indent)
                rightPadding: Math.round(theme.fontSize / 2)
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.label
                Accessible.selected: current
                onClicked: dialog.choose(index)

                contentItem: Text {
                    // Narrowed, a folder is named with the folders above it.
                    text: field.text.length > 0 ? entry.modelData.label : entry.modelData.name
                    textFormat: Text.PlainText
                    font: entry.font
                    color: entry.current ? theme.bright : theme.foreground
                    elide: Text.ElideMiddle
                    verticalAlignment: Text.AlignVCenter
                }

                background: Rectangle {
                    radius: theme.radius
                    color: entry.down ? theme.pressedFill : entry.current || entry.hovered ? theme.hoverFill : "transparent"
                    border.width: entry.current && list.activeFocus ? Math.max(1, theme.controlBorderWidth) : 0
                    border.color: theme.accent
                }
            }
        }

        Text {
            visible: list.count === 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: list.top
            text: backend.organizer.busy ? qsTr("Listing the library's folders…") : qsTr("No other folder of the library")
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.dim
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
            objectName: "moveHere"
            text: qsTr("Move here")
            enabled: list.currentIndex >= 0 && list.currentIndex < dialog.choices.length
            onClicked: dialog.choose(list.currentIndex)
        }
    }
}
