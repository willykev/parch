import QtQuick
import Parch

// The text here and the file on disk side by side, read-only, and what to do
// about them: take the file's text, keep both, or save this text over the
// file. A draft's file is its own draft file, which has no folder to keep a
// copy in. A file that was deleted has nothing to show or keep: this text
// can be saved again where it was, or somewhere else. A file that is there
// but cannot be read is never saved over: it can be read again, or this
// text saved somewhere else. A path that now leads to another file, as
// through a link put in the file's place, is never written through: the
// link can be followed, which makes that file the document's, or this text
// kept beside it, put at the document's own name in place of the link, or
// saved somewhere else. A document opened again at such a path has no text
// of its own, and is offered only what it can do without one: follow the
// link, take a version from its history, or, once no link is left and a
// file is at its name again, reload that file.
// Cancel is the safe choice and has focus, so neither
// Enter nor Escape on opening loses any text. In a narrow window the two
// texts are one above the other. The lines that differ are marked in each,
// and each opens on the first of them. The file is read again whenever
// something touches it, and only the newest read is shown; a file that
// cannot be read says so rather than being waited for.
ThemedDialog {
    id: dialog

    required property WindowContext context
    // The document in conflict, while the dialog shows it.
    property Document document: null
    // This text, and the file's, or why the file's cannot be shown.
    property string localText
    property string diskText
    property string diskProblem
    // What differs between the two, as DocumentReview.difference() finds it.
    property var difference: ({})
    property bool loading: false
    // The read of the file waited for; an answer to any other is out of date.
    property int diskRequest: 0
    readonly property bool missing: document !== null && document.fileMissing
    readonly property bool unreadable: document !== null && document.fileUnreadable
    // The other file the document's path leads to now, or nothing.
    readonly property string leadsTo: document !== null ? document.leadsTo : ""
    readonly property bool elsewhere: leadsTo.length > 0
    // The document has no text of its own to save or keep.
    readonly property bool textless: document !== null && document.textless
    // The writer asked for the document's history, which the window shows.
    signal historyRequested(Document document)

    // Room for two texts of about 32 characters beside each other.
    readonly property bool stacked: availableWidth < 2 * 32 * characterWidth + gap

    // Shows the document's conflict, reading the file off the UI thread.
    function review(document) {
        dialog.document = document
        localText = document.buffer.text()
        readDisk()
        open()
    }

    function readDisk() {
        diskText = ""
        diskProblem = ""
        difference = ({})
        diskRequest = dialog.document.fileMissing || dialog.document.fileUnreadable
                      ? 0 : backend.review.readDiskText(dialog.document, dialog.localText)
        loading = diskRequest !== 0
        if (dialog.document.fileUnreadable)
            diskProblem = backend.shownText(dialog.document.lastError)
        else if (!loading && !dialog.document.fileMissing)
            diskProblem = qsTr("There is no file to show.")
    }

    // Each run that differs, as one side's pane marks it.
    function marksOf(side) {
        if (!dialog.difference.differs || !dialog.difference.runs)
            return []
        return dialog.difference.runs.map(run => ({ start: run[side + "Start"], end: run[side + "End"] }))
    }

    // A copy that Keep both makes opens in this window, in front.
    function choose(resolution) {
        backend.activate(dialog.context)
        backend.review.resolveConflict(dialog.document, resolution)
        dialog.accept()
    }

    title: dialog.document === null ? ""
           : missing ? qsTr("“%1” no longer exists").arg(dialog.document.title)
           : unreadable ? qsTr("“%1” cannot be read").arg(dialog.document.title)
           : elsewhere ? qsTr("“%1” now leads to another file").arg(dialog.document.title)
           : qsTr("“%1” changed on disk").arg(dialog.document.title)
    preferredCharacters: 100
    preferredHeight: 34 * Math.round(theme.fontSize * 1.5)
    initialFocus: cancel

    Connections {
        target: backend.review
        function onDiskTextRead(request, documentId, text, problem, difference) {
            if (request !== dialog.diskRequest)
                return
            dialog.diskText = text
            dialog.diskProblem = problem
            dialog.difference = difference
            dialog.loading = false
        }
        function onDiskChanged(documentId) {
            if (dialog.opened && dialog.document && documentId === dialog.document.id)
                dialog.readDisk()
        }
    }

    // A file that goes, or comes back, is read again.
    onMissingChanged: if (dialog.opened) dialog.readDisk()
    onUnreadableChanged: if (dialog.opened) dialog.readDisk()

    // Resolved, here or elsewhere, there is nothing left to choose.
    Connections {
        target: dialog.document
        function onChanged() {
            if (dialog.opened && dialog.document.state !== Document.Conflict)
                dialog.reject()
        }
    }

    contentItem: Item {
        implicitWidth: dialog.preferredCharacters * dialog.characterWidth
        implicitHeight: 24 * Math.round(theme.fontSize * 1.5)

        Text {
            id: explanation
            objectName: "conflictExplanation"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            text: dialog.textless && !dialog.elsewhere && dialog.missing
                  ? qsTr("This document has no text of its own, and nothing is at its name. Its history may have a version of its text.")
                  : dialog.textless && !dialog.elsewhere && !dialog.unreadable
                  ? qsTr("This document has no text of its own, and a file is at its name again, with no link. Reload takes that file; its history may have a version of its text.")
                  : dialog.missing
                  ? qsTr("Something removed the file. This text is only here and in recovery until it is saved again.")
                  : dialog.unreadable
                  ? qsTr("parch cannot read the file, so it cannot tell what saving would replace. Nothing is saved over it until it can be read; this text stays here and in recovery.")
                  : dialog.elsewhere && dialog.textless
                  ? qsTr("Something put a link in the file's place, which leads to “%1”. Nothing is taken from there unless you follow the link, and nothing kept this document's own text: its history may have a version of it.").arg(backend.shownText(dialog.leadsTo))
                  : dialog.elsewhere
                  ? qsTr("Something put a link in the file's place, which leads to “%1”. Nothing is saved there, and nothing taken from there, unless you follow the link.").arg(backend.shownText(dialog.leadsTo))
                  : qsTr("Something else changed the file while this text had changes of its own. Nothing is saved over the file until you choose.")
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.foreground
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Grid {
            id: panes
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: explanation.bottom
            anchors.bottom: parent.bottom
            anchors.topMargin: dialog.gap
            columns: dialog.stacked ? 1 : 2
            spacing: dialog.gap

            readonly property real paneWidth: dialog.stacked ? width : (width - spacing) / 2
            readonly property real paneHeight: dialog.stacked ? (height - spacing) / 2 : height

            ThemedDialog.TextPane {
                objectName: "localPane"
                width: panes.paneWidth
                height: panes.paneHeight
                caption: qsTr("This text")
                text: dialog.localText
                marks: dialog.marksOf("local")
            }

            ThemedDialog.TextPane {
                objectName: "diskPane"
                width: panes.paneWidth
                height: panes.paneHeight
                caption: qsTr("On disk")
                text: dialog.diskText
                marks: dialog.marksOf("disk")
                note: dialog.missing ? qsTr("The file no longer exists.")
                      : dialog.loading ? qsTr("Reading the file…")
                      : dialog.diskProblem
            }
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            visible: dialog.unreadable
            text: qsTr("Retry")
            Accessible.description: qsTr("Read the file again.")
            onClicked: {
                backend.activate(dialog.context)
                backend.readAgain()
                dialog.accept()
            }
        }
        ThemedDialog.Choice {
            visible: dialog.elsewhere
            text: qsTr("Follow the link")
            // A document with no text of its own has none to keep.
            Accessible.description: (dialog.textless
                                     ? qsTr("Take the text of “%1”, and save there from now on.")
                                     : qsTr("Take the text of “%1”, and save there from now on; this text stays in history and undo."))
                                    .arg(backend.shownText(dialog.leadsTo))
            onClicked: dialog.choose(DocumentReview.Follow)
        }
        ThemedDialog.Choice {
            visible: !dialog.missing && !dialog.unreadable && !dialog.elsewhere
            text: qsTr("Reload")
            Accessible.description: dialog.textless
                                    ? qsTr("Take the file's text.")
                                    : qsTr("Take the file's text; this text stays in history and undo.")
            onClicked: dialog.choose(DocumentReview.Reload)
        }
        ThemedDialog.Choice {
            visible: dialog.textless
            text: qsTr("Version history…")
            Accessible.description: qsTr("Choose a version of this document's text from its history.")
            onClicked: {
                const document = dialog.document
                dialog.accept()
                dialog.historyRequested(document)
            }
        }
        ThemedDialog.Choice {
            visible: !dialog.missing && !dialog.unreadable && !dialog.textless && dialog.document !== null
                     && !dialog.document.draft
            text: qsTr("Keep both")
            Accessible.description: dialog.elsewhere
                                    ? qsTr("Save this text as a copy in the document's folder; the document keeps this text, and waits.")
                                    : qsTr("Save this text as a copy beside the file, and take the file's text here.")
            onClicked: dialog.choose(DocumentReview.KeepBoth)
        }
        ThemedDialog.Choice {
            visible: !dialog.missing && !dialog.unreadable && !dialog.elsewhere && !dialog.textless
            text: qsTr("Overwrite")
            Accessible.description: qsTr("Save this text over the file; the file's text stays in history.")
            onClicked: dialog.choose(DocumentReview.Overwrite)
        }
        ThemedDialog.Choice {
            visible: dialog.elsewhere && !dialog.textless
            text: dialog.document !== null ? qsTr("Put this text at “%1”").arg(dialog.document.title) : ""
            Accessible.description: qsTr("Save this text at the document's own name, in place of the link; “%1” stays as it is.")
                                    .arg(backend.shownText(dialog.leadsTo))
            onClicked: dialog.choose(DocumentReview.PutHere)
        }
        ThemedDialog.Choice {
            visible: dialog.missing && !dialog.textless
            text: qsTr("Save again")
            Accessible.description: qsTr("Save this text where the file was.")
            onClicked: dialog.choose(DocumentReview.Overwrite)
        }
        ThemedDialog.Choice {
            visible: (dialog.missing || dialog.unreadable || dialog.elsewhere) && !dialog.textless
            text: qsTr("Save as…")
            Accessible.description: qsTr("Choose another place to save this text.")
            onClicked: {
                backend.activate(dialog.context)
                dialog.accept()
                backend.saveAs()
            }
        }
        ThemedDialog.Choice {
            id: cancel
            text: qsTr("Cancel")
            onClicked: dialog.reject()
        }
    }
}
