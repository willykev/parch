import QtQuick
import Parch

// A link requiring confirmation shows its destination above the choices.
ThemedDialog {
    id: dialog

    // The address to open, or the path of the file to show.
    property string target: ""
    // How the question shows it.
    property string shown: ""
    property string scheme: ""
    // A file on this computer, which is shown in its folder.
    property bool local: false

    property string origin: ""
    property bool userInfo: false

    function ask(target, shown, scheme, local, origin, userInfo) {
        dialog.origin = origin
        dialog.userInfo = userInfo
        details.contentY = 0
        dialog.target = target
        dialog.shown = shown
        dialog.scheme = scheme
        dialog.local = local
        open()
    }

    // Closes first, so that what the answer leads to finds the window as it is.
    function answer() {
        close()
        if (dialog.local)
            backend.showInFolder(dialog.target)
        else
            backend.openAsked(dialog.target)
    }

    title: dialog.local ? qsTr("Show this file in its folder?") : qsTr("Open a “%1:” link?").arg(dialog.scheme)
    preferredCharacters: 60
    initialFocus: cancelChoice

    contentItem: Column {
        spacing: dialog.gap

        Text {
            objectName: "linkOrigin"
            visible: !dialog.local
            width: parent.width
            text: dialog.origin
            textFormat: Text.PlainText
            font: dialog.font
            color: theme.foreground
            wrapMode: Text.WrapAnywhere
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Text {
            objectName: "linkExplanation"
            width: parent.width
            text: dialog.local
                  ? qsTr("A link never opens a file other than a Markdown document: the file manager shows it, and you open it from there if you mean to.")
                  : dialog.userInfo ? qsTr("This address includes user information before the host name.")
                  : dialog.scheme === "mailto" ? qsTr("This link includes extra instructions for your mail program.")
                  : dialog.scheme === "http" || dialog.scheme === "https" ? qsTr("Check the host name before opening this address.")
                  : qsTr("Links that start with “%1:” go to the program registered for them.").arg(dialog.scheme)
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.small
            color: theme.dim
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Flickable {
            id: details
            width: parent.width
            height: Math.min(contentHeight, dialog.parent ? dialog.parent.height * 0.32 : contentHeight)
            contentHeight: question.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Text {
                id: question
                objectName: "linkQuestion"
                width: details.width
                text: dialog.local
                      ? qsTr("%1 is a file on this computer that parch does not open itself. Show it in its folder?").arg(dialog.shown)
                      : qsTr("Open %1 with the program registered for it?").arg(dialog.shown)
                textFormat: Text.PlainText
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                color: theme.foreground
                // An address has few spaces to wrap at.
                wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: cancelChoice
            objectName: "linkCancelChoice"
            text: qsTr("Cancel")
            Accessible.description: qsTr("Follow nothing.")
            onClicked: dialog.close()
        }
        ThemedDialog.Choice {
            objectName: "linkShowChoice"
            visible: dialog.local
            text: qsTr("Show in folder")
            Accessible.description: qsTr("Show the file, selected, in the file manager.")
            onClicked: dialog.answer()
        }
        ThemedDialog.Choice {
            objectName: "linkOpenChoice"
            visible: !dialog.local
            text: qsTr("Open")
            Accessible.description: qsTr("Open the address with the program registered for it.")
            onClicked: dialog.answer()
        }
    }
}
