import QtQuick
import Parch

// Asks before a document or folder of the library goes to the trash, saying
// what goes with a folder and what becomes of a document that is open.
// Cancel is the safe choice and has focus.
ThemedDialog {
    id: dialog

    // What the organizer asks about, as its trashing says.
    readonly property var trashing: backend.organizer.trashing

    title: trashing.name ? qsTr("Move “%1” to the trash?").arg(trashing.name) : ""
    preferredCharacters: 56
    initialFocus: cancel

    contentItem: Text {
        objectName: "trashDetail"
        text: {
            const about = dialog.trashing
            if (!about.name)
                return ""
            let what
            if (!about.folder)
                what = qsTr("It goes to the trash, from where it can be put back.")
            else if (about.documents === 0)
                what = qsTr("The folder holds no documents. It goes to the trash, from where it can be put back.")
            else if (about.documents === 1)
                what = qsTr("The folder and the document in it go to the trash, from where they can be put back.")
            else
                what = qsTr("The folder and the %1 documents in it go to the trash, from where they can be put back.").arg(about.documents)
            if (about.open > 0)
                what += " " + qsTr("What is open of it stays in its tab, with its text, until the tab is closed.")
            return what
        }
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        color: theme.foreground
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    onRejected: backend.organizer.cancelTrash()

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: cancel
            text: qsTr("Cancel")
            onClicked: dialog.reject()
        }
        ThemedDialog.Choice {
            objectName: "trashConfirm"
            text: qsTr("Move to trash")
            textColor: theme.danger
            onClicked: {
                dialog.accept()
                backend.organizer.confirmTrash()
            }
        }
    }
}
