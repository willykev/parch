import QtQuick
import QtQuick.Controls.Basic
import Parch
import "icons.js" as Icons

// Find, and replace, in the document in front: a bar above the text, in the
// layout and never over it. The field finds as it is typed, the match
// nearest the caret selected in the text; Enter and Shift+Enter go to the
// next and previous match, as F3 and Shift+F3 do from anywhere in the
// window. Case, whole words and regular expressions are toggles, and a
// pattern that is no regular expression says why beneath. With replace
// shown, Enter in its field replaces the match selected and goes to the
// next, and Replace all replaces every one; each is one undo step. Escape
// anywhere in the bar closes it, and the window gives the keys back to the
// text.
Rectangle {
    id: bar

    // What finds, which the bar drives and reads.
    required property DocumentFind find
    // The replace row is shown.
    property bool replacing: false
    // The keys that open the bar and step through it, for the tooltips.
    property var keys: ({})

    property int replacedCount: 0

    // Escape or the close button: the window closes the bar and gives the
    // keys back to the text.
    signal closeRequested()

    readonly property real gap: Math.round(theme.fontSize / 2)
    readonly property real rowHeight: Math.round(theme.fontSize * 2.4)
    readonly property real iconSize: Math.round(theme.fontSize * 1.15)
    readonly property real buttonSize: rowHeight
    readonly property string replacedText: qsTr("%1 replaced").arg(Number(replacedCount).toLocaleString(Qt.locale(), "f", 0))
    readonly property string statusHint: !visible && replaceSummary.running ? replacedText : ""

    // What the count says: where the selection is among the matches, how
    // many there are, or that there are none yet.
    readonly property string countText: {
        if (replaceSummary.running)
            return replacedText
        if (find.pattern.length === 0 || find.error.length > 0)
            return ""
        if (find.searching)
            return qsTr("Searching…")
        if (find.count === 0)
            return qsTr("No matches")
        const total = Number(find.count).toLocaleString(Qt.locale(), "f", 0) + (find.truncated ? "+" : "")
        if (find.current >= 0)
            return qsTr("%1 of %2").arg(Number(find.current + 1).toLocaleString(Qt.locale(), "f", 0)).arg(total)
        return find.count === 1 ? qsTr("1 match") : qsTr("%1 matches").arg(total)
    }

    Connections {
        target: bar.find
        function onReplacedAll(count) {
            bar.replacedCount = count
            replaceSummary.restart()
        }
        function onQueryChanged() { replaceSummary.stop() }
        function onDocumentChanged() { replaceSummary.stop() }
        function onActiveChanged() { replaceSummary.stop() }
    }
    QuietTimer {
        id: replaceSummary
        interval: 4000
    }

    // The keys go to the find field, with what it holds selected.
    function focusFind() {
        findField.forceActiveFocus(Qt.TabFocusReason)
        findField.selectAll()
    }

    function focusReplace() {
        replaceField.forceActiveFocus(Qt.TabFocusReason)
        replaceField.selectAll()
    }

    // The field's text, as when the bar opens on a selection.
    function setPattern(text) {
        findField.text = text
    }

    implicitHeight: visible ? column.implicitHeight + 2 * gap + line.height : 0
    color: theme.panel
    Accessible.role: Accessible.ToolBar
    Accessible.name: replacing ? qsTr("Find and replace") : qsTr("Find")

    Keys.onEscapePressed: bar.closeRequested()

    // A toggle among the bar's controls: a short label, framed and in the
    // accent while it is on, named in full for assistive technology.
    component Toggle: ThemedButton {
        id: toggle

        property string name

        checkable: true
        framed: checked
        textColor: checked ? theme.accent : theme.foreground
        implicitWidth: bar.buttonSize
        implicitHeight: bar.buttonSize
        padding: 0
        detail: name
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        Accessible.name: name
        Accessible.checkable: true
        Accessible.checked: checked
    }

    // A field of the bar, with the window's caret.
    component Field: TextField {
        id: field

        height: bar.rowHeight
        leftPadding: bar.gap
        topPadding: 0
        bottomPadding: 0
        placeholderTextColor: theme.dim
        color: theme.foreground
        selectionColor: theme.selection
        selectedTextColor: theme.foreground
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        verticalAlignment: TextInput.AlignVCenter
        selectByMouse: true
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
            color: theme.background
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: field.activeFocus ? theme.accent : theme.controlBorder
        }
    }

    Column {
        id: column
        x: bar.gap
        y: bar.gap
        width: bar.width - 2 * bar.gap
        spacing: bar.gap

        Item {
            width: column.width
            height: bar.rowHeight

            Field {
                id: findField
                objectName: "findField"
                anchors.left: parent.left
                anchors.right: findControls.left
                anchors.rightMargin: bar.gap
                anchors.verticalCenter: parent.verticalCenter
                rightPadding: countLabel.visible ? countLabel.width + 2 * bar.gap : bar.gap
                placeholderText: qsTr("Find")
                Accessible.searchEdit: true
                Accessible.description: bar.find.error.length > 0 ? bar.find.error : bar.countText
                onTextChanged: {
                    bar.find.pattern = text
                    bar.find.seek()
                }
                Keys.onReturnPressed: (event) => (event.modifiers & Qt.ShiftModifier) ? bar.find.previous() : bar.find.next()
                Keys.onEnterPressed: (event) => (event.modifiers & Qt.ShiftModifier) ? bar.find.previous() : bar.find.next()

                Text {
                    id: countLabel
                    objectName: "findCount"
                    visible: text.length > 0
                    anchors.right: parent.right
                    anchors.rightMargin: bar.gap
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, findField.width / 2)
                    text: bar.countText
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizes.small
                    color: bar.find.count === 0 && !bar.find.searching && !replaceSummary.running ? theme.danger : theme.dim
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }
            }

            Row {
                id: findControls
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter

                Toggle {
                    objectName: "findCase"
                    text: qsTr("Aa")
                    name: qsTr("Match case")
                    checked: bar.find.caseSensitive
                    onToggled: bar.find.caseSensitive = checked
                }
                Toggle {
                    objectName: "findWholeWord"
                    text: qsTr("W")
                    name: qsTr("Whole words")
                    checked: bar.find.wholeWord
                    onToggled: bar.find.wholeWord = checked
                }
                Toggle {
                    objectName: "findRegex"
                    text: qsTr(".*")
                    name: qsTr("Regular expression")
                    checked: bar.find.regex
                    onToggled: bar.find.regex = checked
                }
                ThemedButton {
                    objectName: "findPrevious"
                    width: bar.buttonSize
                    height: bar.buttonSize
                    iconPath: Icons.chevronUp
                    iconSize: bar.iconSize
                    text: qsTr("Previous match")
                    keys: bar.keys.findPrevious || ""
                    enabled: bar.find.count > 0
                    onClicked: bar.find.previous()
                }
                ThemedButton {
                    objectName: "findNext"
                    width: bar.buttonSize
                    height: bar.buttonSize
                    iconPath: Icons.chevronDown
                    iconSize: bar.iconSize
                    text: qsTr("Next match")
                    keys: bar.keys.findNext || ""
                    enabled: bar.find.count > 0
                    onClicked: bar.find.next()
                }
                ThemedButton {
                    objectName: "findClose"
                    width: bar.buttonSize
                    height: bar.buttonSize
                    iconPath: Icons.close
                    iconSize: bar.iconSize
                    text: qsTr("Close find")
                    onClicked: bar.closeRequested()
                }
            }
        }

        Item {
            visible: bar.replacing
            width: column.width
            height: bar.rowHeight

            Field {
                id: replaceField
                objectName: "replaceField"
                anchors.left: parent.left
                anchors.right: replaceControls.left
                anchors.rightMargin: bar.gap
                anchors.verticalCenter: parent.verticalCenter
                rightPadding: bar.gap
                placeholderText: qsTr("Replace with")
                enabled: bar.find.canReplace
                Accessible.description: bar.find.regex ? qsTr("$1 to $99 are the groups the pattern captured") : ""
                Keys.onReturnPressed: bar.find.replace(text)
                Keys.onEnterPressed: bar.find.replace(text)
            }

            Row {
                id: replaceControls
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Math.round(bar.gap / 2)

                ThemedButton {
                    objectName: "replaceOne"
                    height: bar.buttonSize
                    framed: true
                    horizontalPadding: bar.gap
                    text: qsTr("Replace")
                    detail: qsTr("Replace the selected match and go to the next")
                    enabled: bar.find.canReplace && bar.find.count > 0
                    onClicked: bar.find.replace(replaceField.text)
                }
                ThemedButton {
                    objectName: "replaceAll"
                    height: bar.buttonSize
                    framed: true
                    horizontalPadding: bar.gap
                    text: qsTr("All")
                    detail: qsTr("Replace every match")
                    Accessible.name: qsTr("Replace all")
                    enabled: bar.find.canReplace && bar.find.count > 0
                    onClicked: bar.find.replaceAll(replaceField.text)
                }
            }
        }

        // Why the pattern is no regular expression.
        Text {
            objectName: "findError"
            visible: bar.find.error.length > 0
            width: column.width
            text: bar.find.error
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.small
            color: theme.danger
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    Rectangle {
        id: line
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: theme.controlBorderWidth
        color: theme.controlBorder
    }
}
