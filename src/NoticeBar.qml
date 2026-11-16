import QtQuick
import Parch

// Above the editor, in the layout and never over the text: a row for each
// thing about the document in front that asks for the writer's attention,
// with what can be done about it. A row about the document's state goes only
// once its cause is resolved: a failed save, a conflict, a read-only file, a
// failed recovery write, paused history. A notice with no state behind it,
// such as a file dialog that failed, has Dismiss. The bar never takes the
// keys by itself: a click on it leaves them in the text, and only Tab and
// F6 reach its buttons. Messages name parch's own folders by what they are
// for and the home folder as ~, on screen and to assistive technology,
// which hears the whole message.
Rectangle {
    id: bar

    property Document document
    property bool historyPaused: false
    // Paused because named versions fill history's space, rather than
    // because the last version did not fit.
    property bool historyFull: false
    // Why recovery could not take the document's text; empty when it can.
    property string recoveryFailure
    // A notice from the last action, in a few words and in full.
    property string notice
    property string noticeDetail
    // What the notice tells of, as Backend.NoticeTone: a failure is marked
    // as danger, one whose harm was headed off as a warning, and news calmly.
    property int noticeTone: Backend.Failure
    // What the notice tells of can be undone, with Undo beside Dismiss.
    property bool noticeUndoable: false

    signal retrySave()
    // A file that could not be read is read again.
    signal readAgain()
    // A failed save, or text whose file cannot be read, goes somewhere else
    // instead, as the conflict dialog's Save as… does.
    signal saveAs()
    signal reviewConflict()
    signal retryRecovery()
    signal reviewHistory()
    signal dismiss()
    signal undo()
    // A file that is not UTF-8 is read in another encoding, and one read so
    // is written as UTF-8, each once the writer chooses.
    signal reopenAs()
    signal confirmEncoding()

    // The rows shown, by what each is about, most pressing first.
    readonly property var kinds: {
        const shown = []
        if (bar.document) {
            switch (bar.document.state) {
            case Document.Conflict:
                shown.push("conflict")
                break
            case Document.SaveFailed:
                shown.push("saveFailed")
                break
            case Document.ReadOnly:
                // parch's own read-only documents, such as its guide, are as they should be.
                if (!bar.document.scratch)
                    shown.push("readOnly")
                break
            }
            if (bar.document.convertedFrom.length > 0 && bar.document.state !== Document.Conflict)
                shown.push("converted")
            if (bar.recoveryFailure.length > 0)
                shown.push("recovery")
            if (bar.historyPaused)
                shown.push("history")
        }
        if (bar.notice.length > 0)
            shown.push("app")
        return shown
    }

    // The document waits because its file is there but cannot be read.
    readonly property bool unreadable: document !== null && document.fileUnreadable
    readonly property real gap: Math.round(theme.fontSize * 2 / 3)
    readonly property real characterWidth: zero.advanceWidth
    // A row has a button for the keys to go to.
    readonly property bool reachable: visible && kinds.some(kind => actionOf(kind).length > 0)

    // What a row's button says, by what the row is about; none for a row
    // with nothing to be done about it.
    function actionOf(kind) {
        switch (kind) {
        case "conflict":
            return bar.unreadable ? qsTr("Retry") : qsTr("Review")
        case "saveFailed":
        case "recovery":
            return qsTr("Retry")
        case "history":
            return qsTr("Review history")
        case "app":
            return qsTr("Dismiss")
        case "readOnly":
            return qsTr("Reopen as…")
        case "converted":
            return qsTr("Save as UTF-8…")
        default:
            return ""
        }
    }

    // Gives the keys to the first row's button, as F6 does; false when no row has one.
    function focusFirst() {
        for (let i = 0; i < repeater.count; ++i) {
            const row = repeater.itemAt(i)
            if (row && row.takeKeys())
                return true
        }
        return false
    }

    visible: kinds.length > 0
    implicitHeight: visible ? rows.implicitHeight + line.height : 0
    color: theme.panel

    TextMetrics {
        id: zero
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: "0"
    }

    Column {
        id: rows
        width: parent.width

        Repeater {
            id: repeater
            model: bar.kinds

            delegate: Item {
                id: row

                required property string modelData
                readonly property string kind: modelData
                readonly property bool missing: bar.document !== null && bar.document.fileMissing
                // As it happened, and as the writer reads it.
                readonly property string full: {
                    const error = bar.document ? bar.document.lastError : ""
                    switch (kind) {
                    case "conflict":
                        // A name something else took is said whole.
                        if (bar.document.nameTaken)
                            return error
                        if (bar.unreadable)
                            return qsTr("%1. Nothing is saved over it until it can be read.").arg(error)
                        return qsTr("%1. Nothing is saved over it until you choose.")
                            .arg(error.length > 0 ? error
                                 : missing ? qsTr("The file no longer exists")
                                 : qsTr("The file has changed on disk"))
                    case "saveFailed":
                        return error.length > 0 ? qsTr("Not saved: %1").arg(error) : qsTr("Not saved")
                    case "readOnly":
                        return qsTr("Read-only: this file is not valid UTF-8 text, so its bytes are kept as they are and it is never saved. Reopen it in the encoding it was written in to edit it.")
                    case "converted":
                        return qsTr("Read as %1. Nothing is saved until you choose to write it as UTF-8; its original bytes are kept in history first.")
                            .arg(bar.document.convertedFrom)
                    case "recovery":
                        return qsTr("Recovery could not keep this text: %1").arg(bar.recoveryFailure)
                    case "history":
                        return bar.historyFull
                            ? qsTr("History is paused: named versions fill the space it may use. Saving goes on.")
                            : qsTr("History is paused: the last version did not fit in the space it may use. Saving goes on.")
                    default:
                        return bar.notice
                    }
                }
                readonly property string message: backend.shownText(full)
                readonly property string action: bar.actionOf(kind)
                readonly property string fullDetail: kind === "app" ? bar.noticeDetail : ""
                readonly property string detail: backend.shownText(fullDetail)
                // What the row says beside its buttons needs room for a few
                // words; with less, the buttons go beneath it.
                readonly property real markRoom: mark.width + bar.gap
                readonly property real buttonGap: Math.round(theme.fontSize / 2)
                readonly property real actionsWidth: actions.visible
                    ? button.implicitWidth + (saveAsButton.visible ? buttonGap + saveAsButton.implicitWidth : 0)
                      + (undoButton.visible ? buttonGap + undoButton.implicitWidth : 0) : 0
                readonly property bool stacked: actions.visible
                    && width - 2 * bar.gap - markRoom - actionsWidth - bar.gap < 24 * bar.characterWidth

                function takeKeys() {
                    if (!actions.visible)
                        return false
                    const first = undoButton.visible ? undoButton : button
                    first.forceActiveFocus(Qt.TabFocusReason)
                    return true
                }

                objectName: kind + "Notice"
                width: rows.width
                implicitHeight: stacked ? words.height + Math.round(theme.fontSize / 4) + button.height + 2 * inset
                                        : Math.max(words.height, actions.visible ? button.height : 0) + 2 * inset
                readonly property real inset: Math.round(theme.fontSize / 3)
                Accessible.role: Accessible.AlertMessage
                Accessible.name: message
                Accessible.description: detail.length > 0 ? message + "\n" + detail : message

                Rectangle {
                    id: mark
                    objectName: "noticeMark"
                    x: bar.gap
                    y: words.y + Math.round((firstLine.height - height) / 2)
                    width: Math.round(theme.fontSize / 2)
                    height: width
                    // A light, as the status line's, and so round whatever the theme's corners.
                    radius: width / 2
                    // Danger is for failures, and warning for those whose harm
                    // was headed off. News is marked in the dim role, a mix of
                    // the text and the background, since some themes' accent is
                    // as red as their danger.
                    color: row.kind === "readOnly" || row.kind === "history" || row.kind === "converted"
                           || (row.kind === "app" && bar.noticeTone === Backend.Warning) ? theme.warning
                         : row.kind === "app" && bar.noticeTone === Backend.News ? theme.dim : theme.danger
                }

                // The height of one line of the message, to centre the mark on its first line.
                Text {
                    id: firstLine
                    visible: false
                    text: "0"
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                }

                Column {
                    id: words
                    x: mark.x + row.markRoom
                    y: row.stacked ? row.inset : Math.round((row.implicitHeight - height) / 2)
                    width: row.stacked ? row.width - x - bar.gap
                                       : row.width - x - bar.gap - (actions.visible ? row.actionsWidth + bar.gap : 0)

                    Text {
                        objectName: "noticeText"
                        width: parent.width
                        text: row.message
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.fontSize
                        color: theme.foreground
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        Accessible.ignored: true
                    }

                    Text {
                        visible: row.detail.length > 0
                        width: parent.width
                        text: row.detail
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizes.small
                        color: theme.dim
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                        Accessible.ignored: true
                    }
                }

                // The row's action, and for a failed save or a file that cannot
                // be read Save as… beside it, at the row's end; each gives way
                // to the other in a narrow row.
                Row {
                    id: actions
                    visible: row.action.length > 0
                    x: row.width - bar.gap - width
                    y: row.stacked ? words.y + words.height + Math.round(theme.fontSize / 4)
                                   : Math.round((row.implicitHeight - height) / 2)
                    spacing: row.buttonGap
                    readonly property int buttons: 1 + (saveAsButton.visible ? 1 : 0) + (undoButton.visible ? 1 : 0)
                    readonly property real room: (row.width - 2 * bar.gap - (buttons - 1) * spacing) / buttons

                    // Undo comes first, as what the notice offers.
                    ThemedButton {
                        id: undoButton
                        objectName: "noticeUndo"
                        visible: row.kind === "app" && bar.noticeUndoable
                        width: Math.min(implicitWidth, actions.room)
                        framed: true
                        focusPolicy: Qt.TabFocus
                        text: qsTr("Undo")
                        Accessible.description: row.message
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        onClicked: bar.undo()
                    }

                    ThemedButton {
                        id: button
                        objectName: row.kind === "app" ? "noticeDismiss" : "noticeAction"
                        width: Math.min(implicitWidth, actions.room)
                        framed: true
                        // Reached by Tab and F6, but a click leaves the keys in the text.
                        focusPolicy: Qt.TabFocus
                        text: row.action
                        Accessible.description: row.message
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        onClicked: {
                            switch (row.kind) {
                            case "conflict":
                                if (bar.unreadable)
                                    bar.readAgain()
                                else
                                    bar.reviewConflict()
                                break
                            case "saveFailed":
                                bar.retrySave()
                                break
                            case "recovery":
                                bar.retryRecovery()
                                break
                            case "history":
                                bar.reviewHistory()
                                break
                            case "app":
                                bar.dismiss()
                                break
                            case "readOnly":
                                bar.reopenAs()
                                break
                            case "converted":
                                bar.confirmEncoding()
                                break
                            }
                        }
                    }

                    ThemedButton {
                        id: saveAsButton
                        objectName: "noticeSaveAs"
                        visible: row.kind === "saveFailed" || (row.kind === "conflict" && bar.unreadable)
                        width: Math.min(implicitWidth, actions.room)
                        framed: true
                        focusPolicy: Qt.TabFocus
                        text: qsTr("Save as…")
                        Accessible.description: qsTr("Choose another place to save this text.")
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        onClicked: bar.saveAs()
                    }
                }

                Rectangle {
                    visible: row.kind !== bar.kinds[bar.kinds.length - 1]
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: words.x
                    height: theme.controlBorderWidth
                    color: theme.controlBorder
                }
            }
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
