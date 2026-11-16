import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Templates as T
import Parch

// Every command and its keys, to look through and to change. A command is
// found by typing part of its name; the arrows move through the list, and
// Enter or Change records new keys for the command chosen: the next keys
// pressed, or Escape to leave its keys as they were. Keys that type text,
// or that the editor keeps for editing, are refused with the reason; keys
// another command has are offered to swap. Remove leaves a command without
// keys, Reset gives it its own back, and Reset all every command its own.
ThemedDialog {
    id: dialog

    property CommandRegistry commands: null

    // The command being given new keys, while the keys are recorded.
    property string recording
    // The keys pressed, while the command that has them is asked about.
    property string pressed
    property var taken: null
    // What happened, or what to do next, said for everyone to read.
    property string message

    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)
    // Read again whenever the matches change, which a call alone would not notice.
    readonly property string current: matches.count >= 0 ? matches.idAt(list.currentIndex) : ""
    readonly property string currentTitle: current ? commands.title(current) : ""

    function startRecording() {
        if (!current)
            return
        recording = current
        pressed = ""
        taken = null
        message = qsTr("Press the new keys for %1, or Escape to keep its keys").arg(currentTitle)
        recorder.forceActiveFocus(Qt.TabFocusReason)
    }

    function stopRecording(said) {
        recording = ""
        pressed = ""
        taken = null
        message = said
        list.forceActiveFocus(Qt.TabFocusReason)
    }

    // The keys pressed while recording, taken or refused with the reason.
    function record(sequence) {
        const check = commands.check(recording, sequence)
        const title = commands.title(recording)
        if (check.allowed) {
            commands.bind(recording, sequence)
            stopRecording(qsTr("%1 is now %2").arg(title).arg(check.keys))
        } else if (check.problem === "typing") {
            message = qsTr("%1 types text. Use Ctrl, Alt or Super with it, or a function key.").arg(check.keys)
        } else if (check.problem === "reserved") {
            message = qsTr("%1 is kept for editing text. Try other keys.").arg(check.keys)
        } else if (check.problem === "taken") {
            pressed = sequence
            taken = check
            message = qsTr("%1 is already %2. Swap their keys?").arg(check.keys).arg(check.takenByTitle)
            swapButton.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    function swap() {
        const title = commands.title(recording)
        const keys = taken.keys
        commands.swap(recording, pressed)
        stopRecording(qsTr("%1 is now %2").arg(title).arg(keys))
    }

    function show() {
        field.text = ""
        list.currentIndex = 0
        recording = ""
        message = ""
        open()
    }

    title: qsTr("Keyboard shortcuts")
    initialFocus: field
    preferredCharacters: 72
    preferredHeight: Math.round(theme.fontSize * 36)
    // Escape while recording leaves the keys alone, and the dialog open.
    closePolicy: recording ? T.Popup.NoAutoClose : T.Popup.CloseOnEscape
    onClosed: recording = ""

    contentItem: Item {
        implicitHeight: field.height + list.implicitHeight + bar.height

        ThemedField {
            id: field
            objectName: "shortcutsField"
            anchors.left: parent.left
            anchors.right: parent.right
            enabled: !dialog.recording
            placeholderText: qsTr("Find a command")
            Accessible.searchEdit: true
            onTextChanged: list.currentIndex = 0
            Keys.onDownPressed: list.forceActiveFocus(Qt.TabFocusReason)
        }

        ListView {
            id: list
            objectName: "shortcutsList"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: field.bottom
            anchors.topMargin: Math.round(theme.fontSize / 2)
            anchors.bottom: bar.top
            anchors.bottomMargin: Math.round(theme.fontSize / 2)
            clip: true
            enabled: !dialog.recording
            activeFocusOnTab: true
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Commands and their keys")
            Accessible.description: currentItem ? currentItem.spoken : ""
            Keys.onReturnPressed: dialog.startRecording()
            Keys.onEnterPressed: dialog.startRecording()

            model: CommandFilter {
                id: matches
                objectName: "shortcutsMatches"
                registry: dialog.commands
                text: field.text
            }

            delegate: Rectangle {
                id: row

                required property int index
                required property string commandId
                required property string title
                required property string category
                required property string keyText
                required property bool customized
                readonly property bool isCurrent: ListView.isCurrentItem
                readonly property string spoken: [title, keyText.length > 0 ? keyText : qsTr("no keys"),
                                                  customized ? qsTr("changed") : ""].filter((part) => part.length > 0).join(", ")

                width: ListView.view.width
                height: dialog.rowHeight
                radius: theme.radius
                color: isCurrent ? theme.selectedFill : "transparent"
                border.width: isCurrent && list.activeFocus ? Math.max(1, theme.controlBorderWidth) : 0
                border.color: theme.accent
                Accessible.role: Accessible.ListItem
                Accessible.name: spoken
                Accessible.selected: isCurrent

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        list.currentIndex = row.index
                        list.forceActiveFocus(Qt.TabFocusReason)
                    }
                    onDoubleClicked: dialog.startRecording()
                }

                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: Math.round(theme.fontSize * 2 / 3)
                    anchors.right: kind.left
                    anchors.rightMargin: Math.round(theme.fontSize / 2)
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.title
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: row.isCurrent ? theme.bright : theme.foreground
                    elide: Text.ElideRight
                }

                Text {
                    id: kind
                    anchors.right: shortcut.left
                    anchors.rightMargin: Math.round(theme.fontSize)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, row.width / 5)
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
                    anchors.rightMargin: Math.round(theme.fontSize * 2 / 3)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, row.width / 3)
                    text: row.keyText.length > 0 ? row.keyText : "—"
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    // The writer's own keys stand out from the defaults.
                    color: row.customized ? theme.accent : theme.dim
                    elide: Text.ElideLeft
                }
            }

            ScrollBar.vertical: ThemedDialog.Scroller { Accessible.name: qsTr("Scroll the commands") }
        }

        Column {
            id: bar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            spacing: Math.round(theme.fontSize / 2)

            Text {
                id: said
                objectName: "shortcutsMessage"
                width: parent.width
                visible: dialog.message.length > 0
                text: dialog.message
                textFormat: Text.PlainText
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                color: dialog.recording ? theme.bright : theme.dim
                wrapMode: Text.Wrap
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            // Takes the keys pressed while recording, all of them, Tab included.
            Item {
                id: recorder
                objectName: "shortcutsRecorder"
                width: 1
                height: 1
                Accessible.role: Accessible.StaticText
                Accessible.name: dialog.message
                // Only while it records; the message says the same meanwhile.
                Accessible.ignored: !dialog.recording
                Keys.onPressed: (event) => {
                    event.accepted = true
                    if (!dialog.recording || dialog.taken)
                        return
                    if (event.key === Qt.Key_Escape && event.modifiers === Qt.NoModifier) {
                        dialog.stopRecording(qsTr("%1 keeps its keys").arg(dialog.commands.title(dialog.recording)))
                        return
                    }
                    const sequence = dialog.commands.sequenceOf(event.key, event.modifiers)
                    if (sequence.length > 0)
                        dialog.record(sequence)
                }
            }

            Flow {
                width: parent.width
                spacing: Math.round(theme.fontSize / 2)
                visible: dialog.taken !== null

                ThemedDialog.Choice {
                    id: swapButton
                    objectName: "swapKeys"
                    text: qsTr("Swap")
                    onClicked: dialog.swap()
                }
                ThemedDialog.Choice {
                    objectName: "keepKeys"
                    text: qsTr("Cancel")
                    onClicked: dialog.stopRecording(qsTr("%1 keeps its keys").arg(dialog.commands.title(dialog.recording)))
                }
            }

            Flow {
                width: parent.width
                spacing: Math.round(theme.fontSize / 2)
                visible: !dialog.recording

                ThemedDialog.Choice {
                    objectName: "changeKeys"
                    text: qsTr("Change…")
                    enabled: dialog.current.length > 0
                    onClicked: dialog.startRecording()
                }
                ThemedDialog.Choice {
                    objectName: "removeKeys"
                    text: qsTr("Remove")
                    enabled: dialog.current.length > 0 && dialog.commands.keys[dialog.current] !== undefined
                    onClicked: {
                        const title = dialog.currentTitle
                        dialog.commands.unbind(dialog.current)
                        dialog.message = qsTr("%1 has no keys").arg(title)
                    }
                }
                ThemedDialog.Choice {
                    objectName: "resetKeys"
                    text: qsTr("Reset")
                    enabled: dialog.current.length > 0
                    onClicked: {
                        const title = dialog.currentTitle
                        dialog.commands.reset(dialog.current)
                        dialog.message = qsTr("%1 has its own keys again").arg(title)
                    }
                }
                ThemedDialog.Choice {
                    objectName: "resetAllKeys"
                    text: qsTr("Reset all")
                    onClicked: {
                        dialog.commands.resetAll()
                        dialog.message = qsTr("Every command has its own keys again")
                    }
                }
                ThemedDialog.Choice {
                    objectName: "closeShortcuts"
                    text: qsTr("Close")
                    onClicked: dialog.close()
                }
            }
        }
    }
}
