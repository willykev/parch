import QtQuick
import Parch

// Asked when the file an export would write is a symbolic link. parch never
// writes through a link, since what it leads to may be any file, such as a
// shell's start-up file: the export goes to another file, chosen in the file
// dialog, or, only when the writer says so, replaces the link itself, and
// what the link leads to stays as it is. Cancel is the safe choice and has
// focus; Escape and a dialog closed any other way cancel too.
ThemedDialog {
    id: dialog

    property string output: ""
    property string target: ""
    property string name: ""
    // The question has its answer, so that closing does not answer it again.
    property bool answered: true

    function ask(output, target) {
        dialog.output = output
        dialog.target = target
        dialog.name = output.substring(output.lastIndexOf("/") + 1)
        dialog.answered = false
        open()
    }

    // Closes first, so that the file dialog an answer opens finds the window as it is.
    function answer(choice) {
        dialog.answered = true
        close()
        if (choice === "elsewhere")
            backend.exports.exportElsewhere()
        else if (choice === "replace")
            backend.exports.replaceLink()
        else
            backend.exports.keepLink()
    }

    onClosed: if (!dialog.answered) dialog.answer("cancel")

    title: qsTr("“%1” is a link").arg(dialog.name)
    preferredCharacters: 60
    initialFocus: cancelChoice

    contentItem: Text {
        objectName: "exportLinkQuestion"
        text: qsTr("%1 is a link to %2. parch never writes through a link: export to another file, or replace the link itself with the export, which leaves %2 as it is.")
              .replace(/%[12]/g, function(slot) { return slot === "%1" ? dialog.output : dialog.target })
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        color: theme.foreground
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: cancelChoice
            objectName: "exportLinkCancelChoice"
            text: qsTr("Cancel")
            Accessible.description: qsTr("Export nothing.")
            onClicked: dialog.answer("cancel")
        }
        ThemedDialog.Choice {
            objectName: "exportLinkReplaceChoice"
            text: qsTr("Replace the link")
            Accessible.description: qsTr("Write the export in place of the link, leaving the file it leads to as it is.")
            onClicked: dialog.answer("replace")
        }
        ThemedDialog.Choice {
            objectName: "exportLinkElsewhereChoice"
            text: qsTr("Export as…")
            Accessible.description: qsTr("Choose another file to export to.")
            onClicked: dialog.answer("elsewhere")
        }
    }
}
