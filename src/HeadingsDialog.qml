import QtQuick
import QtQuick.Controls.Basic
import Parch

// Go to a heading of the document in front: its headings, indented by
// level, narrowed by what is typed in the field. Up and Down move through
// them from the field, Enter goes to the one marked, and Escape goes back
// to the text where it was. The list opens on the heading the caret is
// under.
ThemedDialog {
    id: dialog

    property Document document: null
    // Where the caret was as the dialog opened, as a source offset.
    property int caret: 0

    // A heading was chosen: the window puts its line at the top of the view.
    signal chosen(int position)
    // A heading an include brought in was chosen: the window opens its file there.
    signal includedChosen(string path, int line)

    function show(document, caret) {
        dialog.document = document
        dialog.caret = caret
        field.text = ""
        open()
    }

    function go(row) {
        const position = outline.positionAt(row)
        if (position < 0)
            return
        dialog.accept()
        const file = outline.fileAt(row)
        if (file.length > 0)
            dialog.includedChosen(file, outline.fileLineAt(row))
        else
            dialog.chosen(position)
    }

    function move(rows) {
        if (outline.count === 0)
            return
        list.currentIndex = Math.max(0, Math.min(outline.count - 1, list.currentIndex + rows))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
    }

    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)
    readonly property real indent: Math.round(theme.fontSize * 1.2)

    title: qsTr("Go to heading")
    preferredCharacters: 64
    preferredHeight: parent ? Math.round(parent.height * 0.7) : implicitHeight
    initialFocus: field

    OutlineModel {
        id: outline
        objectName: "headingsOutline"
        document: dialog.document
        active: dialog.visible
        filter: field.text
        // Opened, the list starts at the heading the caret is under; narrowed,
        // at the first that is left.
        onReadyChanged: if (ready && field.text.length === 0) list.currentIndex = Math.max(0, rowAt(dialog.caret))
        onFilterChanged: list.currentIndex = 0
    }

    contentItem: Item {
        implicitHeight: field.height + dialog.rowHeight * 8

        TextField {
            id: field
            objectName: "headingsFilter"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.round(theme.fontSize * 2.4)
            leftPadding: Math.round(theme.fontSize / 2)
            rightPadding: Math.round(theme.fontSize / 2)
            topPadding: 0
            bottomPadding: 0
            placeholderText: qsTr("Find a heading")
            placeholderTextColor: theme.dim
            color: theme.foreground
            selectionColor: theme.selection
            selectedTextColor: theme.foreground
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            verticalAlignment: TextInput.AlignVCenter
            cursorDelegate: FieldCaret { rest: filterCaret }
            Accessible.name: placeholderText
            Accessible.searchEdit: true

            CaretRest {
                id: filterCaret
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
                case Qt.Key_PageDown:
                    dialog.move(Math.max(1, Math.floor(list.height / dialog.rowHeight) - 1))
                    break
                case Qt.Key_PageUp:
                    dialog.move(-Math.max(1, Math.floor(list.height / dialog.rowHeight) - 1))
                    break
                case Qt.Key_Return:
                case Qt.Key_Enter:
                    dialog.go(list.currentIndex)
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
            objectName: "headingsList"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: field.bottom
            anchors.topMargin: Math.round(theme.fontSize / 2)
            anchors.bottom: parent.bottom
            clip: true
            model: outline
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            activeFocusOnTab: true
            keyNavigationEnabled: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Headings")
            Keys.onReturnPressed: dialog.go(currentIndex)
            Keys.onEnterPressed: dialog.go(currentIndex)
            Keys.onSpacePressed: dialog.go(currentIndex)

            delegate: AbstractButton {
                id: entry
                objectName: "headingRow"

                required property int index
                required property string heading
                required property int level
                required property string parentPath

                readonly property bool current: ListView.isCurrentItem

                width: list.width
                height: dialog.rowHeight
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                leftPadding: Math.round(theme.fontSize / 2) + (field.text.length > 0 ? 0 : (level - 1) * dialog.indent)
                rightPadding: Math.round(theme.fontSize / 2)
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                font.weight: level <= 2 ? Font.DemiBold : Font.Normal
                Accessible.role: Accessible.ListItem
                text: parentPath.length > 0 ? qsTr("%1 › %2").arg(parentPath).arg(heading) : heading
                Accessible.name: qsTr("%1, level %2").arg(text).arg(level)
                Accessible.selected: current
                onClicked: dialog.go(index)

                contentItem: Text {
                    text: entry.text
                    textFormat: Text.PlainText
                    font: entry.font
                    color: entry.current ? theme.bright : theme.foreground
                    elide: field.text.length > 0 ? Text.ElideMiddle : Text.ElideRight
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
            objectName: "headingsEmpty"
            visible: outline.ready && outline.count === 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: list.top
            text: field.text.length > 0 ? qsTr("No heading holds “%1”").arg(field.text) : qsTr("This document has no headings")
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.dim
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }
}
