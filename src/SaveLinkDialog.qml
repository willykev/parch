import QtQuick
import Parch

ThemedDialog {
    id: dialog

    property var request: null
    property var waiting: []

    function ask(document, name, target, canFollow) {
        waiting.push({ document, name, target, canFollow })
        if (!request)
            showNext()
    }

    function showNext() {
        if (!request && waiting.length > 0) {
            request = waiting.shift()
            open()
        }
    }

    function answer(choice) {
        const current = request
        request = null
        close()
        if (current)
            backend.answerSaveLink(current.document, current.name, current.target, choice)
    }

    onClosed: {
        if (request)
            answer("cancel")
        Qt.callLater(showNext)
    }

    title: qsTr("Save at a symbolic link")
    preferredCharacters: 60
    initialFocus: cancelChoice

    contentItem: Text {
        objectName: "saveLinkQuestion"
        text: dialog.request ? qsTr("“%1” links to “%2”. Replacing the link puts this document at “%1”.")
                               .arg(backend.shownText(dialog.request.name)).arg(backend.shownText(dialog.request.target)) : ""
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
            objectName: "saveLinkCancel"
            text: qsTr("Cancel")
            onClicked: dialog.answer("cancel")
        }
        ThemedDialog.Choice {
            objectName: "saveLinkReplace"
            text: qsTr("Replace the link")
            onClicked: dialog.answer("replace")
        }
        ThemedDialog.Choice {
            objectName: "saveLinkFollow"
            visible: !!dialog.request && dialog.request.canFollow
            text: qsTr("Write to the target")
            onClicked: dialog.answer("follow")
        }
    }
}
