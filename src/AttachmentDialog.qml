import QtQuick
import Parch

// Asked while files dropped on the text are on their way in: whether a
// dropped Markdown document is linked to or included where it was dropped,
// and whether files over the large-file size are copied into the assets.
// Cancel is the safe choice and has focus; Escape and a dialog closed any
// other way cancel too, so every question gets exactly one answer.
ThemedDialog {
    id: dialog

    // "include" or "copy".
    property string question: ""
    property string name: ""
    property bool canInclude: true
    property var names: []
    property real bytes: 0
    // The question has its answer, so that closing does not answer it again.
    property bool answered: true

    // A DropChoice of EditorBinding.
    signal includeChosen(int choice)
    signal copyChosen(bool copy)

    function askInclude(name, canInclude) {
        dialog.question = "include"
        dialog.name = name
        dialog.canInclude = canInclude
        dialog.answered = false
        open()
    }

    function askCopy(names, bytes) {
        dialog.question = "copy"
        dialog.names = names
        dialog.bytes = bytes
        dialog.answered = false
        open()
    }

    // Closes first, so that a question the answer leads to opens afresh.
    function answer(value) {
        const asked = dialog.question
        dialog.answered = true
        close()
        if (asked === "include")
            dialog.includeChosen(value)
        else
            dialog.copyChosen(value)
    }

    function cancel() {
        answer(dialog.question === "include" ? EditorBinding.Cancel : false)
    }

    onClosed: if (!dialog.answered) dialog.cancel()

    // Sizes as people read them, in units of 1024 bytes.
    function sizeText(bytes) {
        const units = [qsTr("%1 bytes"), qsTr("%1 KiB"), qsTr("%1 MiB"), qsTr("%1 GiB"), qsTr("%1 TiB")]
        let unit = 0
        while (bytes >= 1024 && unit < units.length - 1) {
            bytes /= 1024
            ++unit
        }
        return units[unit].arg(unit === 0 ? bytes : bytes.toFixed(1))
    }

    title: dialog.question === "include" ? qsTr("Add “%1”").arg(dialog.name) : qsTr("Copy large files?")
    preferredCharacters: 60
    initialFocus: cancelChoice

    contentItem: Text {
        objectName: "attachmentQuestion"
        text: {
            if (dialog.question === "include") {
                return dialog.canInclude
                    ? qsTr("Insert a link to it, or include its text here when the document is shown and exported.")
                    : qsTr("Insert a link to it. A draft includes other documents only once it is saved with a name.")
            }
            return qsTr("%1 (%2) will be copied into this document's assets folder.")
                .arg(dialog.names.join(", ")).arg(dialog.sizeText(dialog.bytes))
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
            objectName: "insertLinkChoice"
            visible: dialog.question === "include"
            text: qsTr("Insert link")
            Accessible.description: qsTr("Link to the document where it is dropped.")
            onClicked: dialog.answer(EditorBinding.InsertLink)
        }
        ThemedDialog.Choice {
            objectName: "includeChoice"
            visible: dialog.question === "include" && dialog.canInclude
            text: qsTr("Include")
            Accessible.description: qsTr("Show the document's text where it is dropped.")
            onClicked: dialog.answer(EditorBinding.Include)
        }
        ThemedDialog.Choice {
            objectName: "copyChoice"
            visible: dialog.question === "copy"
            text: qsTr("Copy")
            Accessible.description: qsTr("Copy the files into the assets folder and link to them.")
            onClicked: dialog.answer(true)
        }
        ThemedDialog.Choice {
            id: cancelChoice
            objectName: "cancelChoice"
            text: qsTr("Cancel")
            Accessible.description: qsTr("Add nothing.")
            onClicked: dialog.cancel()
        }
    }
}
