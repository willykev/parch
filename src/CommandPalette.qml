import QtQuick
import QtQuick.Controls.Basic
import Parch

// Every command, found by typing part of its name: the field keeps the keys
// throughout, the arrows move through the matches, Enter runs the one
// chosen and Escape closes. Each match shows its keys. A command that cannot
// run now is listed, dimmed, and Enter leaves it be. Assistive technology
// hears the chosen command as the field's description.
ThemedDialog {
    id: picker

    property CommandRegistry commands: null
    // Whether a command can run now, and how it is run, as the window says.
    property var canRun: function(command) { return true }
    property var runCommand: function(command) {}

    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)

    // Opens with nothing typed and the first command chosen.
    function show() {
        field.text = ""
        list.currentIndex = 0
        open()
    }

    // Closes, then runs the command, which may give the keys elsewhere.
    function choose(row) {
        const command = matches.idAt(row)
        if (!command || !canRun(command))
            return
        close()
        Qt.callLater(picker.runCommand, command)
    }

    title: qsTr("Commands")
    initialFocus: field
    preferredCharacters: 64
    preferredHeight: implicitHeight
    // Toward the top of the window, as such palettes open, clear of the header.
    anchors.centerIn: undefined
    x: Math.round((parent ? parent.width - width : 0) / 2)
    y: parent ? Math.min(Math.round(theme.fontSize * 3), Math.max(0, parent.height - height)) : 0

    contentItem: Column {
        spacing: Math.round(theme.fontSize / 2)

        ThemedField {
            id: field
            objectName: "paletteField"
            width: parent.width
            placeholderText: qsTr("Find a command")
            Accessible.searchEdit: true
            Accessible.description: list.currentItem ? list.currentItem.spoken : qsTr("No command matches")

            onTextChanged: list.currentIndex = 0
            Keys.onUpPressed: list.currentIndex = Math.max(0, list.currentIndex - 1)
            Keys.onDownPressed: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1)
            Keys.onPressed: (event) => {
                const page = Math.max(1, Math.floor(list.height / picker.rowHeight) - 1)
                if (event.key === Qt.Key_PageDown) {
                    list.currentIndex = Math.min(list.count - 1, list.currentIndex + page)
                } else if (event.key === Qt.Key_PageUp) {
                    list.currentIndex = Math.max(0, list.currentIndex - page)
                } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
                    picker.choose(list.currentIndex)
                } else {
                    return
                }
                event.accepted = true
            }
        }

        ListView {
            id: list
            objectName: "paletteList"
            width: parent.width
            height: Math.min(contentHeight, 10 * picker.rowHeight)
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            highlightFollowsCurrentItem: true
            keyNavigationEnabled: false
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Commands")

            model: CommandFilter {
                id: matches
                objectName: "paletteMatches"
                registry: picker.commands
                text: field.text
            }

            delegate: ItemDelegate {
                id: row

                required property int index
                required property string commandId
                required property string title
                required property string category
                required property string keyText
                readonly property bool available: picker.canRun(commandId)
                // What assistive technology hears for it.
                readonly property string spoken: [title, category, keyText, available ? "" : qsTr("not available now")]
                                                 .filter((part) => part.length > 0).join(", ")

                width: ListView.view.width
                height: picker.rowHeight
                leftPadding: Math.round(theme.fontSize * 2 / 3)
                rightPadding: Math.round(theme.fontSize * 2 / 3)
                // Clicking leaves the keys in the field.
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                text: title
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                Accessible.name: spoken
                Accessible.selected: ListView.isCurrentItem
                onClicked: picker.choose(index)

                contentItem: Item {
                    Text {
                        id: name
                        anchors.left: parent.left
                        anchors.right: kind.left
                        anchors.rightMargin: Math.round(theme.fontSize / 2)
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.title
                        textFormat: Text.PlainText
                        font: row.font
                        color: !row.available ? theme.faint : row.ListView.isCurrentItem ? theme.bright : theme.foreground
                        elide: Text.ElideRight
                    }

                    Text {
                        id: kind
                        anchors.right: shortcut.left
                        anchors.rightMargin: Math.round(theme.fontSize)
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width / 4)
                        text: row.category
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizes.small
                        color: theme.dim
                        elide: Text.ElideRight
                    }

                    Text {
                        id: shortcut
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.min(implicitWidth, parent.width / 3)
                        text: row.keyText
                        textFormat: Text.PlainText
                        font: row.font
                        color: theme.dim
                        elide: Text.ElideLeft
                    }
                }

                background: Rectangle {
                    radius: theme.radius
                    color: row.ListView.isCurrentItem ? theme.selectedFill : row.hovered ? theme.hoverFill : "transparent"
                }
            }
        }

        Text {
            objectName: "paletteEmpty"
            visible: list.count === 0
            width: parent.width
            height: visible ? picker.rowHeight : 0
            text: qsTr("No command matches “%1”").arg(field.text)
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.dim
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }
}
