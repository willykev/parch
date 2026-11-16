import QtQuick
import QtQuick.Controls.Basic
import Parch

// The document's versions: the text as it is now, then each version kept in
// history, newest first, with when it was taken and its name. The one chosen is
// shown read-only beside the list, or under it in a narrow window, read
// off the UI thread. A version can be named, restored as one edit that a
// single undo takes back, or deleted after asking; the text as it is now
// can be named, as a new version, but not restored or deleted. Everything
// here acts on the document the dialog shows, whichever is in front, and a
// read answered after a newer one was asked is not shown.
ThemedDialog {
    id: dialog

    property Document document: null
    // The versions in history, newest first, as the backend reads them.
    property var versions: []
    readonly property var rows: [{ "id": "", "taken": null, "when": "", "name": "", "reason": "" }].concat(versions)
    property alias currentIndex: list.currentIndex
    // The row chosen, for what shows it; code that runs as the choice
    // changes asks chosen(), which never lags behind it.
    readonly property var current: rows[Math.max(0, Math.min(currentIndex, rows.length - 1))]
    // The row chosen as the buttons take it. Versions read afresh replace
    // the list's rows, and the list passes by its first row on its way back
    // to the one chosen; meanwhile this stays, so that no button goes off
    // and on, which would drop a press that was under way. Delete follows
    // it, and Restore, whose preview stays as it is.
    property int chosenIndex: 0
    property bool rereading: false
    readonly property bool currentText: chosenIndex <= 0
    // The text the preview shows, or why it cannot.
    property string previewText
    property string previewProblem
    property bool loading: false
    readonly property bool stacked: availableWidth < 2 * 30 * characterWidth + gap
    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)
    // The reads waited for; an answer to any other is out of date.
    property int versionsRequest: 0
    property int previewRequest: 0

    function chosen() {
        return dialog.rows[Math.max(0, Math.min(list.currentIndex, dialog.rows.length - 1))]
    }

    function review(document) {
        dialog.document = document
        versions = []
        list.currentIndex = 0
        reload()
        showCurrent()
        open()
    }

    function reload() {
        if (dialog.document)
            versionsRequest = backend.review.readVersions(dialog.document)
    }

    // A version as the preview's header and the delete prompt name it: when
    // it was taken, why history kept it by itself, and its name.
    function describe(row) {
        if (!row.id)
            return qsTr("Current text")
        const when = row.reason ? qsTr("%1 · %2").arg(row.when).arg(row.reason) : row.when
        return row.name ? qsTr("%1, %2").arg(when).arg(row.name) : when
    }

    // The row chosen, in the preview: the text as it is now from memory, a
    // version as history reads it.
    function showCurrent() {
        if (!dialog.document)
            return
        const row = chosen()
        if (!row.id) {
            // No read is waited for any more.
            previewRequest = 0
            previewText = dialog.document.buffer.text()
            previewProblem = ""
            loading = false
            return
        }
        previewText = ""
        previewProblem = ""
        loading = true
        previewRequest = backend.review.readVersion(dialog.document, row.id)
    }

    title: dialog.document ? qsTr("Versions of “%1”").arg(dialog.document.title) : ""
    preferredCharacters: 104
    preferredHeight: 34 * Math.round(theme.fontSize * 1.5)
    initialFocus: list

    // Its prompts go with it.
    onClosed: {
        namePrompt.close()
        deletePrompt.close()
    }

    Connections {
        target: backend.review
        function onVersionsRead(request, documentId, read) {
            if (request !== dialog.versionsRequest)
                return
            // The same version stays chosen where it can, or the row in its place.
            const was = dialog.chosen().id
            const index = list.currentIndex
            dialog.rereading = true
            dialog.versions = read
            let found = -1
            for (let i = 0; i < read.length; ++i)
                if (read[i].id === was)
                    found = i + 1
            list.currentIndex = was ? (found >= 0 ? found : Math.min(index, read.length)) : 0
            dialog.rereading = false
            dialog.chosenIndex = list.currentIndex
            // The version still chosen keeps its preview, which is not read
            // again, so that Restore stays as it was, and a press under way
            // with it.
            if (!was || dialog.chosen().id !== was)
                dialog.showCurrent()
        }
        function onVersionRead(request, documentId, versionId, text, problem) {
            if (request !== dialog.previewRequest)
                return
            dialog.previewText = text
            dialog.previewProblem = problem
            dialog.loading = false
        }
        function onVersionsChanged(documentId) {
            if (dialog.visible && dialog.document && documentId === dialog.document.id)
                dialog.reload()
        }
    }

    contentItem: Item {
        implicitWidth: dialog.preferredCharacters * dialog.characterWidth
        implicitHeight: 24 * Math.round(theme.fontSize * 1.5)

        // The list in a frame of its own, as the preview has.
        Rectangle {
            id: listFrame
            width: dialog.stacked ? parent.width : Math.round(parent.width * 0.4)
            height: dialog.stacked ? Math.max(3 * dialog.rowHeight + 2 * inset, Math.round((parent.height - dialog.gap) * 0.4))
                                   : parent.height
            readonly property real inset: Math.round(theme.fontSize / 4)
            color: theme.background
            radius: theme.radius
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: theme.controlBorder
        }

        ListView {
            id: list
            objectName: "versionList"
            x: listFrame.inset
            y: listFrame.inset
            width: listFrame.width - 2 * listFrame.inset
            height: listFrame.height - 2 * listFrame.inset
            clip: true
            model: dialog.rows
            activeFocusOnTab: true
            keyNavigationEnabled: true
            boundsBehavior: Flickable.StopAtBounds
            highlightFollowsCurrentItem: false
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Versions")
            onCurrentIndexChanged: {
                if (dialog.rereading)
                    return
                dialog.chosenIndex = currentIndex
                if (dialog.visible)
                    dialog.showCurrent()
            }

            delegate: Rectangle {
                id: entry

                required property int index
                required property var modelData
                readonly property bool chosen: ListView.isCurrentItem

                width: ListView.view.width
                height: dialog.rowHeight
                radius: theme.radius
                color: chosen ? (list.activeFocus ? theme.selectedFill : theme.hoverFill) : "transparent"
                border.width: chosen && list.activeFocus ? Math.max(1, theme.controlBorderWidth) : 0
                border.color: theme.accent
                Accessible.role: Accessible.ListItem
                Accessible.name: dialog.describe(modelData)
                Accessible.selected: chosen

                Text {
                    id: when
                    anchors.left: parent.left
                    anchors.leftMargin: Math.round(theme.fontSize / 2)
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width - 2 * anchors.leftMargin)
                    text: entry.modelData.id ? entry.modelData.when : qsTr("Current text")
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: entry.chosen ? theme.bright : theme.foreground
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }

                // Why history kept the version by itself, quietly, after the time.
                Text {
                    id: reason
                    objectName: "versionReason"
                    anchors.left: when.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, Math.max(0, parent.width - x - Math.round(theme.fontSize / 2)))
                    visible: text.length > 0
                    text: entry.modelData.reason ? qsTr(" · %1").arg(entry.modelData.reason) : ""
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: theme.dim
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }

                Text {
                    anchors.left: reason.visible ? reason.right : when.right
                    anchors.leftMargin: dialog.gap
                    anchors.right: parent.right
                    anchors.rightMargin: Math.round(theme.fontSize / 2)
                    anchors.verticalCenter: parent.verticalCenter
                    visible: text.length > 0 && width > dialog.characterWidth
                    text: entry.modelData.name || ""
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: theme.accent
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        list.currentIndex = entry.index
                        list.forceActiveFocus(Qt.MouseFocusReason)
                    }
                }
            }
        }

        ThemedDialog.TextPane {
            objectName: "versionPreview"
            x: dialog.stacked ? 0 : listFrame.width + dialog.gap
            y: dialog.stacked ? listFrame.height + dialog.gap : 0
            width: dialog.stacked ? parent.width : parent.width - x
            height: dialog.stacked ? parent.height - y : parent.height
            caption: dialog.describe(dialog.current)
            text: dialog.previewText
            note: dialog.loading ? qsTr("Reading the version…") : dialog.previewProblem
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: nameButton
            text: qsTr("Name this version")
            onClicked: namePrompt.ask()
        }
        ThemedDialog.Choice {
            objectName: "restoreVersion"
            text: qsTr("Restore")
            enabled: !dialog.currentText && !dialog.loading && dialog.previewProblem.length === 0
            Accessible.description: qsTr("Replace the text with this version; undo takes it back.")
            onClicked: {
                backend.review.restoreVersion(dialog.document, dialog.chosen().id)
                dialog.accept()
            }
        }
        ThemedDialog.Choice {
            id: deleteButton
            text: qsTr("Delete version")
            enabled: !dialog.currentText
            onClicked: deletePrompt.ask()
        }
        ThemedDialog.Choice {
            text: qsTr("Close")
            onClicked: dialog.reject()
        }
    }

    // A prompt's action done, the keys go to the list, which it changed;
    // cancelled, they go back to the button that opened it.
    function afterPrompt(done, opener) {
        if (dialog.visible)
            (done || !opener.enabled ? list : opener).forceActiveFocus(Qt.TabFocusReason)
    }

    // A name for the row chosen: for the text as it is now, a new version.
    ThemedDialog {
        id: namePrompt
        objectName: "versionNamePrompt"

        property bool done: false

        function ask() {
            done = false
            field.text = dialog.chosen().name || ""
            open()
        }

        function name() {
            const name = field.text.trim()
            if (name.length === 0)
                return
            const row = dialog.chosen()
            if (!row.id)
                backend.review.checkpoint(dialog.document, name)
            else
                backend.review.nameVersion(dialog.document, row.id, name)
            done = true
            namePrompt.accept()
        }

        title: dialog.currentText ? qsTr("Name the current text") : qsTr("Name this version")
        preferredCharacters: 44
        initialFocus: field
        onClosed: dialog.afterPrompt(done, nameButton)

        contentItem: TextField {
            id: field
            objectName: "versionName"
            placeholderText: qsTr("Name")
            placeholderTextColor: theme.dim
            color: theme.foreground
            selectionColor: theme.selection
            selectedTextColor: theme.foreground
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            cursorDelegate: FieldCaret { rest: fieldCaret }
            Accessible.name: qsTr("Name")
            onAccepted: namePrompt.name()

            CaretRest {
                id: fieldCaret
                objectName: "caretRest"
                target: field
                steady: theme.reduceMotion
            }

            background: Rectangle {
                implicitHeight: Math.round(theme.fontSize * 2.2)
                radius: theme.radius
                color: theme.background
                border.width: Math.max(1, theme.controlBorderWidth)
                border.color: field.activeFocus ? theme.accent : theme.controlBorder
            }
        }

        footer: ThemedDialog.Choices {
            ThemedDialog.Choice {
                text: qsTr("Name")
                enabled: field.text.trim().length > 0
                onClicked: namePrompt.name()
            }
            ThemedDialog.Choice {
                text: qsTr("Cancel")
                onClicked: namePrompt.reject()
            }
        }
    }

    // Deleting cannot be undone, so it asks, with Cancel in front.
    ThemedDialog {
        id: deletePrompt
        objectName: "versionDeletePrompt"

        property bool done: false

        function ask() {
            done = false
            open()
        }

        title: qsTr("Delete this version?")
        preferredCharacters: 52
        initialFocus: keep
        onClosed: dialog.afterPrompt(done, deleteButton)

        contentItem: Text {
            text: qsTr("The version from %1 leaves history for good.").arg(dialog.describe(dialog.current))
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
                text: qsTr("Delete")
                textColor: theme.danger
                onClicked: {
                    backend.review.removeVersion(dialog.document, dialog.chosen().id)
                    deletePrompt.done = true
                    deletePrompt.accept()
                }
            }
            ThemedDialog.Choice {
                id: keep
                text: qsTr("Cancel")
                onClicked: deletePrompt.reject()
            }
        }
    }
}
