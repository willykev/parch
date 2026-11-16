import QtQuick
import Parch
import "icons.js" as Icons

// The line along the bottom of the editor: with Vim keys, their mode first,
// and the command line being typed, the keys of a command not yet complete
// or what the last command said; whether the document is saved, in words,
// and why the last action could not be done, if it could not; and on the
// right how many words the document has, or the selection, and how long
// they take to read. The reason keeps its room first. Between them, while
// the text and its preview are side by side, whether they scroll together.
Rectangle {
    id: line

    property Document document
    // The text and the preview are side by side, and scroll together.
    property bool syncShown: false
    property bool syncOn: true
    signal syncToggled()
    // The state in words: the document's, or why the tab has no document.
    property string status: document ? document.statusText : ""
    // Why the last action could not be done, in a few words, and in full.
    property string problem
    property string problemDetail
    // The view's selection, as source offsets, whose words are counted when it has any.
    property int selectionStart: 0
    property int selectionEnd: 0
    // The editor's Vim keys, while it takes them.
    property VimMode vim: null
    readonly property bool vimShown: vim !== null && vim.active

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
        id: vimArea
        objectName: "vimArea"
        visible: line.vimShown
        anchors.left: parent.left
        anchors.leftMargin: line.gap
        anchors.verticalCenter: parent.verticalCenter
        spacing: Math.round(theme.fontSize / 2)

        Text {
            id: vimMode
            objectName: "vimMode"
            anchors.verticalCenter: parent.verticalCenter
            text: line.vim ? line.vim.modeName : ""
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            font.weight: Font.DemiBold
            color: theme.accent
            Accessible.role: Accessible.StaticText
            Accessible.name: qsTr("Vim mode: %1").arg(text)
        }

        // At most a third of the line, the end of a long command in view.
        Text {
            id: vimCommand
            objectName: "vimCommand"
            readonly property bool said: line.vim !== null && line.vim.commandLine.length === 0
                                         && line.vim.message.length > 0
            anchors.verticalCenter: parent.verticalCenter
            visible: text.length > 0
            width: Math.min(implicitWidth, Math.max(0, line.width / 3))
            text: !line.vim ? "" : line.vim.commandLine.length > 0 ? line.vim.commandLine
                                   : line.vim.message.length > 0 ? line.vim.message : line.vim.pending
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: said ? theme.warning : theme.foreground
            elide: Text.ElideLeft
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    Row {
        id: saving
        anchors.left: vimArea.visible ? vimArea.right : parent.left
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

    // In the middle while there is room there, and clear of what stands on
    // either side of it; left out where there is no room for it.
    ThemedButton {
        id: syncButton
        objectName: "scrollSyncButton"
        readonly property real after: (problemButton.visible ? problemButton.x + problemButton.width
                                                              : saving.x + saving.width) + line.gap
        readonly property real before: (count.visible ? count.x : line.width) - line.gap
        visible: line.syncShown && before - after >= implicitWidth
        x: Math.max(after, Math.min(before - width, Math.round((line.width - width) / 2)))
        anchors.verticalCenter: parent.verticalCenter
        height: line.height - 2 * Math.round(theme.fontSize / 4)
        iconPath: Icons.link
        iconSize: Math.round(theme.fontSize * 1.1)
        labelled: true
        text: line.syncOn ? qsTr("Scroll sync") : qsTr("Scroll sync off")
        detail: line.syncOn ? qsTr("The text and the preview scroll together. Press to let each scroll by itself.")
                            : qsTr("The text and the preview each scroll by themselves. Press to scroll them together.")
        textColor: line.syncOn ? theme.foreground : theme.dim
        tipAbove: true
        // It is on or off, as assistive technology is told; which of the two
        // is the setting's to say, so it is bound again after a press, which
        // changes it by itself.
        checkable: true
        checked: line.syncOn
        onClicked: {
            line.syncToggled()
            checked = Qt.binding(() => line.syncOn)
        }
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
