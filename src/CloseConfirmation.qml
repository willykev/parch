import QtQuick
import Parch

// Asked before a tab closes whose text is in neither its file nor anywhere
// else the writer can see: its save failed, or its file changed or went
// meanwhile. A failed save can be tried again, or saved somewhere else, and
// the tab closes once the text is there; a conflict can be reviewed first.
// Closing without saving keeps the text in history and gives up the rest.
//
// Where nothing would keep the text, it says so plainly and offers to
// discard it instead: a draft that was never saved, which nothing on disk
// leads back to, or history that is paused. When history turns out to be
// paused only once it is asked to keep the text, the window asks again,
// with only Discard text and Cancel. Cancel is the safe choice and has focus.
ThemedDialog {
    id: dialog

    required property WindowContext context
    property Document document: null
    // Asked again: nothing will keep the text, so it can only be discarded.
    property bool discarding: false
    readonly property bool conflict: document !== null && document.state === Document.Conflict
    // Whether history can be counted on to keep the text closed without saving.
    readonly property bool historyKeeps: document !== null && document.everSaved && !backend.historyPaused
    readonly property string reason: {
        if (!dialog.document)
            return ""
        const error = backend.shownText(dialog.document.lastError)
        if (dialog.conflict)
            return error.length > 0 ? error : qsTr("The file has changed on disk")
        return qsTr("It could not be saved: %1").arg(error.length > 0 ? error : qsTr("the reason is not known"))
    }

    // Review was chosen: the window shows the conflict.
    signal review(Document document)

    function ask(document, discarding) {
        dialog.document = document
        dialog.discarding = discarding
        open()
    }

    // Each choice acts on the window's document in front, which this is.
    function act() {
        backend.activate(dialog.context)
    }

    title: !dialog.document ? ""
           : dialog.discarding ? qsTr("Discard “%1”?").arg(dialog.document.title)
           : qsTr("Close “%1”?").arg(dialog.document.title)
    preferredCharacters: 60
    initialFocus: cancel

    // Saved meanwhile, or no longer in front, it has nothing left to ask.
    Connections {
        target: dialog.document
        function onChanged() {
            const state = dialog.document.state
            if (dialog.opened && state !== Document.SaveFailed && state !== Document.Conflict)
                dialog.reject()
        }
    }

    contentItem: Text {
        objectName: "closeReason"
        text: {
            if (!dialog.document)
                return ""
            if (dialog.discarding)
                return qsTr("History is paused, so it cannot keep this text. Discarding it closes the tab and keeps the text nowhere.")
            if (!dialog.document.everSaved)
                return qsTr("%1. This draft has never been saved, so closing it without saving discards this text.")
                    .arg(dialog.reason)
            if (!dialog.historyKeeps)
                return qsTr("%1. History is paused, so closing without saving discards this text.").arg(dialog.reason)
            if (dialog.conflict)
                return qsTr("%1. This text is not saved over it until you choose; closing without saving keeps it in history only.")
                    .arg(dialog.reason)
            return qsTr("%1. Closing without saving keeps this text in history only.").arg(dialog.reason)
        }
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        color: theme.foreground
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            visible: !dialog.discarding && !dialog.conflict
            text: qsTr("Try again")
            Accessible.description: qsTr("Save again, and close once it is saved.")
            onClicked: {
                dialog.act()
                dialog.accept()
                backend.closeTryingAgain()
            }
        }
        ThemedDialog.Choice {
            visible: !dialog.discarding && !dialog.conflict
            text: qsTr("Save as…")
            Accessible.description: qsTr("Choose another place to save it, and close once it is saved there.")
            onClicked: {
                dialog.act()
                dialog.accept()
                backend.closeSavingAs()
            }
        }
        ThemedDialog.Choice {
            visible: !dialog.discarding && dialog.conflict
            text: qsTr("Review")
            Accessible.description: qsTr("Compare this text with the file and choose which to keep.")
            onClicked: {
                const document = dialog.document
                dialog.accept()
                dialog.review(document)
            }
        }
        ThemedDialog.Choice {
            visible: !dialog.discarding && dialog.historyKeeps
            text: qsTr("Close without saving")
            textColor: theme.danger
            Accessible.description: qsTr("Keep this text in history only, and close.")
            onClicked: {
                dialog.act()
                dialog.accept()
                backend.closeWithoutSaving()
            }
        }
        ThemedDialog.Choice {
            visible: dialog.discarding || !dialog.historyKeeps
            text: qsTr("Discard text")
            textColor: theme.danger
            Accessible.description: qsTr("Close, and keep this text nowhere.")
            onClicked: {
                dialog.act()
                dialog.accept()
                backend.discardText()
            }
        }
        ThemedDialog.Choice {
            id: cancel
            text: qsTr("Cancel")
            onClicked: dialog.reject()
        }
    }
}
