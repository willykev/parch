import QtQuick
import Parch

// Check links: the document's local links, images and includes whose files
// are not there, each by its line, found off the UI thread in the text as it
// was when the check began. The list can be selected and copied.
ThemedDialog {
    id: dialog

    property Document document: null
    // The check this dialog shows, as Attachments numbers them.
    property int request: -1
    property bool checking: false
    property var missing: []
    property int checked: 0

    function check(document) {
        dialog.document = document
        dialog.missing = []
        dialog.checked = 0
        dialog.checking = true
        dialog.request = backend.attachments.checkLinks(document)
        open()
    }

    function kindText(kind) {
        return kind === "image" ? qsTr("image") : kind === "include" ? qsTr("include") : qsTr("link")
    }

    readonly property string summary: {
        if (dialog.checking)
            return qsTr("Checking links…")
        if (dialog.missing.length === 0) {
            if (dialog.checked === 0)
                return qsTr("This document has no links or images to local files.")
            return dialog.checked === 1 ? qsTr("Its one local link leads to a file that is there.")
                                        : qsTr("All %1 local links and images lead to files that are there.").arg(dialog.checked)
        }
        return qsTr("%1 of %2 local links and images lead to files that are not there.")
            .arg(dialog.missing.length).arg(dialog.checked)
    }

    readonly property string listing: {
        const lines = []
        for (const target of dialog.missing)
            lines.push(qsTr("Line %1, %2: %3").arg(target.line).arg(dialog.kindText(target.kind)).arg(target.destination))
        return lines.join("\n")
    }

    Connections {
        target: backend.attachments
        function onLinksChecked(request, missing, checked) {
            if (request !== dialog.request)
                return
            dialog.missing = missing
            dialog.checked = checked
            dialog.checking = false
        }
    }

    title: dialog.document ? qsTr("Check links in “%1”").arg(dialog.document.title) : qsTr("Check links")
    preferredCharacters: 64
    initialFocus: closeChoice

    contentItem: Column {
        spacing: Math.round(theme.fontSize / 2)

        Text {
            objectName: "linkSummary"
            width: parent.width
            text: dialog.summary
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.foreground
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        ThemedDialog.TextPane {
            objectName: "missingLinks"
            visible: dialog.missing.length > 0
            width: parent.width
            height: Math.min(10, dialog.missing.length + 1) * Math.round(theme.fontSize * 1.5) + 2 * inset
            caption: qsTr("Missing")
            text: dialog.listing
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: closeChoice
            objectName: "closeChoice"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    }
}
