import QtQuick
import Parch

// Reopen as…: for a file open read-only because it is not UTF-8, the
// encodings to read it in, the likely one chosen, and the start of its text
// as the chosen one reads it. Convert makes it editable with that text;
// nothing is written. Asked again as such a document is first saved, it says
// that saving writes UTF-8 over the file, with the original bytes kept in
// history first, and Save as UTF-8 does so. Cancel is the safe choice.
ThemedDialog {
    id: dialog

    property Document document: null
    // Asking whether saving may write UTF-8, rather than choosing an encoding.
    property bool confirming: false

    function choose(document) {
        dialog.confirming = false
        dialog.document = document
        choice.document = document
        open()
    }

    function confirm(document) {
        dialog.confirming = true
        dialog.document = document
        open()
    }

    title: !dialog.document ? ""
           : dialog.confirming ? qsTr("Save “%1” as UTF-8?").arg(dialog.document.title)
           : qsTr("Reopen “%1” as…").arg(dialog.document.title)
    preferredCharacters: 72
    preferredHeight: dialog.confirming ? implicitHeight : Math.round(theme.fontSize * 34)
    initialFocus: cancel

    onClosed: choice.document = null

    EncodingChoice {
        id: choice
        objectName: "encodingChoice"
    }

    contentItem: Item {
        implicitHeight: dialog.confirming ? question.implicitHeight : choosing.implicitHeight
        implicitWidth: dialog.preferredCharacters * dialog.characterWidth

        Text {
            id: question
            objectName: "encodingQuestion"
            visible: dialog.confirming
            width: parent.width
            text: !dialog.document ? ""
                  : qsTr("“%1” was read as %2. Saving writes it as UTF-8 over the file, and its original bytes are kept in history first.")
                        .arg(dialog.document.title).arg(dialog.document.convertedFrom)
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.foreground
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Column {
            id: choosing
            visible: !dialog.confirming
            anchors.fill: parent
            spacing: Math.round(theme.fontSize / 2)

            PreferenceRow {
                id: encodingRow
                width: parent.width
                label: qsTr("Encoding")

                ThemedComboBox {
                    objectName: "encodingList"
                    width: parent.width
                    model: choice.labels
                    currentIndex: choice.chosen
                    Accessible.name: qsTr("Encoding")
                    onActivated: (index) => choice.chosen = index
                }
            }

            Text {
                id: state
                objectName: "encodingState"
                width: parent.width
                text: choice.error.length > 0 ? choice.error
                      : !choice.ready ? qsTr("Reading the file…")
                      : choice.failures > 0 ? qsTr("%n character(s) cannot be read in this encoding, and show as �.", "", choice.failures)
                      : qsTr("Every character reads in this encoding. Check that the text looks right.")
                textFormat: Text.PlainText
                font.family: theme.fontFamily
                font.pixelSize: theme.sizes.small
                color: choice.failures > 0 || choice.error.length > 0 ? theme.warning : theme.dim
                wrapMode: Text.Wrap
                Accessible.role: Accessible.StaticText
                Accessible.name: text
            }

            ThemedDialog.TextPane {
                objectName: "encodingPreview"
                width: parent.width
                height: Math.max(Math.round(theme.fontSize * 6),
                                 parent.height - encodingRow.height - state.height - 2 * parent.spacing)
                caption: qsTr("Preview")
                text: choice.preview
                note: choice.ready ? "" : choice.error.length > 0 ? choice.error : qsTr("Reading the file…")
            }
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: cancel
            objectName: "cancelEncoding"
            text: qsTr("Cancel")
            onClicked: dialog.reject()
        }
        ThemedDialog.Choice {
            objectName: "convertEncoding"
            visible: !dialog.confirming
            enabled: choice.ready
            text: qsTr("Convert")
            // Taken before the dialog closes, which lets go of the file read.
            onClicked: {
                if (dialog.document)
                    backend.reopenAs(dialog.document, choice)
                dialog.accept()
            }
        }
        ThemedDialog.Choice {
            objectName: "confirmEncoding"
            visible: dialog.confirming
            text: qsTr("Save as UTF-8")
            onClicked: {
                const document = dialog.document
                dialog.accept()
                if (document)
                    backend.confirmEncoding(document)
            }
        }
    }
}
