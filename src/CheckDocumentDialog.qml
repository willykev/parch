import QtQuick
import QtQuick.Controls.Basic
import Parch

// Check document: everything wrong in the document in front, a line each as
// file:line:column: message, found off the UI thread in the text as it was
// when the check began. Up and Down move through the list, and Enter (or a
// click) opens the place: in the document, or in the file it includes that
// the problem is in. The summary says how many, and what could not be
// checked for want of a helper.
ThemedDialog {
    id: dialog

    property Document document: null
    readonly property bool checking: checker.checking
    readonly property var problems: checker.problems
    readonly property string summary: checker.summary
    // Every line, as the list shows them.
    readonly property string listing: dialog.problems.map(problem => problem.text).join("\n")
    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)
    // The application's backend, which the checker is handed: inside it,
    // the name is its own property's.
    readonly property Backend application: backend

    function check(document) {
        dialog.document = document
        checker.check(document)
        list.currentIndex = 0
        open()
    }

    function go(row) {
        if (row < 0 || row >= dialog.problems.length)
            return
        dialog.close()
        checker.open(row)
    }

    function severityText(severity) {
        return severity === "error" ? qsTr("Error") : severity === "info" ? qsTr("Note") : qsTr("Warning")
    }

    DocumentChecker {
        id: checker
        objectName: "documentChecker"
        backend: dialog.application
        // Once the problems are there, the keys go to them, on the first.
        onChecked: {
            if (checking)
                return
            list.currentIndex = 0
            if (checker.problems.length > 0 && closeChoice.activeFocus)
                list.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    title: dialog.document ? qsTr("Check “%1”").arg(dialog.document.title) : qsTr("Check document")
    preferredCharacters: 76
    preferredHeight: parent ? Math.round(parent.height * 0.7) : implicitHeight
    // On the problems when they are there by then, else on Close until they come.
    initialFocus: !checker.checking && dialog.problems.length > 0 ? list : closeChoice
    onClosed: checker.cancel()

    contentItem: Item {
        implicitHeight: summaryText.height + dialog.rowHeight * 6

        Text {
            id: summaryText
            objectName: "checkSummary"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            text: dialog.summary
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.foreground
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        ListView {
            id: list
            objectName: "checkProblems"
            visible: dialog.problems.length > 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: summaryText.bottom
            anchors.topMargin: Math.round(theme.fontSize / 2)
            anchors.bottom: parent.bottom
            clip: true
            model: dialog.problems
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            activeFocusOnTab: true
            keyNavigationEnabled: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Problems")
            Keys.onReturnPressed: dialog.go(currentIndex)
            Keys.onEnterPressed: dialog.go(currentIndex)
            Keys.onSpacePressed: dialog.go(currentIndex)

            delegate: AbstractButton {
                id: entry
                objectName: "problemRow"

                required property int index
                required property var modelData

                readonly property bool current: ListView.isCurrentItem

                width: list.width
                height: dialog.rowHeight
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                leftPadding: Math.round(theme.fontSize / 2)
                rightPadding: Math.round(theme.fontSize / 2)
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                text: modelData.text
                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1: %2").arg(dialog.severityText(modelData.severity)).arg(modelData.text)
                Accessible.selected: current
                onClicked: dialog.go(index)

                contentItem: Row {
                    spacing: Math.round(theme.fontSize / 2)

                    // A mark of how much it matters: the danger colour for
                    // an error or a warning, dimmed for a note.
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: Math.round(theme.fontSize / 2)
                        height: width
                        radius: width / 2
                        color: entry.modelData.severity === "info" ? theme.dim : theme.danger
                    }

                    Text {
                        width: entry.availableWidth - Math.round(theme.fontSize)
                        height: entry.availableHeight
                        text: entry.text
                        textFormat: Text.PlainText
                        font: entry.font
                        color: entry.current ? theme.bright : theme.foreground
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                background: Rectangle {
                    radius: theme.radius
                    color: entry.down ? theme.pressedFill : entry.current || entry.hovered ? theme.hoverFill : "transparent"
                    border.width: entry.current && list.activeFocus ? Math.max(1, theme.controlBorderWidth) : 0
                    border.color: theme.accent
                }
            }
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: closeChoice
            objectName: "closeChoice"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    }
}
