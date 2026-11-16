import QtQuick
import Parch

// The line along the bottom of the editor: whether the document is saved, in
// words, and why the last action could not be done, if it could not; and on
// the right how many words the document has, or the selection, and how long
// they take to read. The reason keeps its room first.
Rectangle {
    id: line

    property Document document
    // The state in words: the document's, or why the tab has no document.
    property string status: document ? document.statusText : ""
    // Why the last action could not be done, in a few words, and in full.
    property string problem
    property string problemDetail
    // The view's selection, as source offsets, whose words are counted when it has any.
    property int selectionStart: 0
    property int selectionEnd: 0

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
        id: problemButton
        objectName: "problem"
        visible: line.problem.length > 0
        anchors.left: saving.right
        anchors.leftMargin: line.gap
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, (count.visible ? count.x - line.gap : line.width - line.gap / 2) - x)
        height: line.height - 2 * Math.round(theme.fontSize / 4)
        text: line.problem
        detail: line.problemDetail
        tipAbove: true
        textColor: theme.danger
    }

    WordCounter {
        id: counter
        objectName: "wordCounter"
        document: line.document
        selectionStart: line.selectionStart
        selectionEnd: line.selectionEnd
    }

    // Shown where there is room beside the state and the reason, cut short
    // where there is only a little.
    Text {
        id: count
        objectName: "wordCount"
        readonly property real room: line.width - saving.x - saving.width - 2 * line.gap
                                     - (problemButton.visible ? problemButton.implicitWidth + line.gap : 0)
        visible: line.document !== null && room >= 6 * zero.advanceWidth
        anchors.right: parent.right
        anchors.rightMargin: line.gap
        anchors.verticalCenter: parent.verticalCenter
        width: Math.min(implicitWidth, Math.max(0, room))
        text: counter.summary
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        color: theme.dim
        elide: Text.ElideLeft
        Accessible.role: Accessible.StaticText
        Accessible.name: counter.spoken
    }

    TextMetrics {
        id: zero
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: "0"
    }
}
