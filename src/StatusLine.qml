import QtQuick
import Parch

// The line along the bottom of the editor: whether the document is saved, in
// words, and why the last action could not be done, if it could not.
Rectangle {
    id: line

    property Document document
    // The state in words: the document's, or why the tab has no document.
    property string status: document ? document.statusText : ""
    // Why the last action could not be done, in a few words, and in full.
    property string problem
    property string problemDetail

    readonly property real gap: Math.round(theme.fontSize * 2 / 3)
    // A draft with nothing in it has nothing saved either, and says so
    // quietly; emptied and unsaved, or failing to save, it says so plainly.
    readonly property bool emptyDraft: document !== null && document.draft && document.buffer.length === 0
                                       && document.state === Document.Clean

    color: theme.panel

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: theme.controlBorderWidth
        color: theme.controlBorder
    }

    Row {
        id: saving
        anchors.left: parent.left
        anchors.leftMargin: line.gap
        anchors.verticalCenter: parent.verticalCenter
        spacing: Math.round(theme.fontSize / 2)

        Rectangle {
            id: mark
            objectName: "saveLight"
            anchors.verticalCenter: parent.verticalCenter
            width: Math.round(theme.fontSize / 2)
            height: width
            // A light, and so round whatever the theme's corners.
            radius: width / 2
            color: {
                // A tab without its document is one whose file is not there.
                if (!line.document)
                    return line.status.length > 0 ? theme.danger : theme.dim
                switch (line.document.state) {
                case Document.Clean:
                    return line.emptyDraft ? theme.dim : theme.good
                case Document.SaveFailed:
                case Document.Conflict:
                    return theme.danger
                case Document.ReadOnly:
                    return theme.warning
                default:
                    return theme.dim
                }
            }
        }

        Text {
            id: saveState
            objectName: "saveState"
            anchors.verticalCenter: parent.verticalCenter
            text: line.status
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: line.emptyDraft ? theme.dim : theme.foreground
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    // In a few words, which take focus from the keyboard: the whole reason
    // shows on hover and on keyboard focus, and goes to assistive technology.
    ThemedButton {
        objectName: "problem"
        visible: line.problem.length > 0
        anchors.left: saving.right
        anchors.leftMargin: line.gap
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, line.width - x - line.gap / 2)
        height: line.height - 2 * Math.round(theme.fontSize / 4)
        text: line.problem
        detail: line.problemDetail
        tipAbove: true
        textColor: theme.danger
    }
}
