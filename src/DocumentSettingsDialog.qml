import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Templates as T
import Parch

// Document settings: how the document in front is read. Its Markdown
// profile and extensions, its citation style and bibliography and its
// library root, each with where it comes from: the document itself, its
// front matter, its library folder, Preferences or the default. A change
// applies at once, to the document alone or to every document in its
// library folder, and is kept by parch, never in the file.
ThemedDialog {
    id: dialog

    property Document document: null
    // "document" or "library": the settings a change goes into.
    property string scope: "document"
    readonly property var info: {
        // Read again whenever any setting changes, while the dialog shows.
        const revision = backend.documentSettings.revision
        return dialog.visible && dialog.document && revision >= 0 ? backend.documentSettings.describe(dialog.document)
                                                                    : ({})
    }
    readonly property bool inLibrary: dialog.info.library ? dialog.info.library.length > 0 : false

    function show(document) {
        dialog.document = document
        dialog.scope = "document"
        open()
    }

    // Keeps the control with the keys in view.
    function reveal(item) {
        if (!item || !visible)
            return
        for (let at = item; at; at = at.parent) {
            if (at === body) {
                const box = item.mapToItem(body, 0, 0, item.width, item.height)
                if (box.y < flick.contentY)
                    flick.contentY = box.y
                else if (box.y + box.height > flick.contentY + flick.height)
                    flick.contentY = Math.min(box.y + box.height - flick.height, flick.contentHeight - flick.height)
                return
            }
        }
    }

    title: dialog.document ? qsTr("Document settings for “%1”").arg(dialog.document.title) : qsTr("Document settings")
    preferredCharacters: 72
    preferredHeight: Math.round(theme.fontSize * 36)
    initialFocus: closeButton

    contentItem: Flickable {
        id: flick
        objectName: "documentSettingsBody"
        clip: true
        contentHeight: body.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        implicitHeight: body.implicitHeight

        Connections {
            target: dialog.visible ? flick.Window.window : null
            function onActiveFocusItemChanged() { dialog.reveal(flick.Window.window.activeFocusItem) }
        }

        Column {
            id: body
            width: flick.width - (scroller.visible ? scroller.width : 0)
            spacing: Math.round(theme.fontSize)

            Column {
                width: parent.width
                spacing: Math.round(theme.fontSize / 4)

                Text {
                    width: parent.width
                    text: qsTr("Change the settings of:")
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: theme.foreground
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                Choice {
                    objectName: "scopeDocument"
                    text: qsTr("This document")
                    checked: dialog.scope === "document"
                    onToggled: dialog.scope = "document"
                }

                Choice {
                    objectName: "scopeLibrary"
                    visible: dialog.inLibrary
                    text: qsTr("Every document in the library folder “%1”").arg(dialog.info.libraryName || "")
                    checked: dialog.scope === "library"
                    onToggled: dialog.scope = "library"
                }

                Text {
                    objectName: "scopeNote"
                    width: parent.width
                    text: dialog.info.draft ? qsTr("A draft is in no library folder until it is saved in one.")
                          : !dialog.inLibrary ? qsTr("This document is in no library folder.")
                          : dialog.scope === "library" ? qsTr("A setting of the document itself still wins over the library's.")
                          : ""
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizes.small
                    color: theme.dim
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }
            }

            DialectSettingsForm {
                objectName: "documentSettingsForm"
                width: parent.width
                document: dialog.document
                scope: dialog.scope
                info: dialog.info
            }
        }

        ScrollBar.vertical: ThemedDialog.Scroller {
            id: scroller
            Accessible.name: qsTr("Scroll the document settings")
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: closeButton
            objectName: "closeDocumentSettings"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    }

    // A radio button as the shell draws its controls, as Preferences draws them.
    component Choice: T.RadioButton {
        id: choice

        function choose(forward) {
            const next = choice.nextItemInFocusChain(forward)
            if (!next || next.parent !== choice.parent || next.autoExclusive === undefined || !next.visible)
                return
            next.forceActiveFocus(Qt.TabFocusReason)
            next.click()
        }
        Keys.onDownPressed: choose(true)
        Keys.onRightPressed: choose(true)
        Keys.onUpPressed: choose(false)
        Keys.onLeftPressed: choose(false)

        width: parent ? Math.min(implicitWidth, parent.width) : implicitWidth
        implicitWidth: implicitContentWidth + leftPadding + rightPadding
        implicitHeight: Math.max(implicitContentHeight, implicitIndicatorHeight) + topPadding + bottomPadding
        padding: Math.round(theme.fontSize / 3)
        leftPadding: indicator.width + 2 * padding
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        hoverEnabled: true
        Accessible.name: text

        indicator: Rectangle {
            x: choice.padding
            anchors.verticalCenter: parent.verticalCenter
            width: Math.round(theme.fontSize * 1.1)
            height: width
            radius: width / 2
            color: "transparent"
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: choice.checked ? theme.accent : theme.controlBorder

            Rectangle {
                anchors.centerIn: parent
                width: Math.round(parent.width / 2)
                height: width
                radius: width / 2
                visible: choice.checked
                color: theme.accent
            }
        }

        contentItem: Text {
            text: choice.text
            textFormat: Text.PlainText
            font: choice.font
            color: choice.enabled ? theme.foreground : theme.faint
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: theme.radius
            color: choice.down ? theme.pressedFill : choice.hovered || choice.visualFocus ? theme.hoverFill : "transparent"
            border.width: choice.visualFocus ? Math.max(1, theme.controlBorderWidth) : 0
            border.color: theme.accent
        }
    }
}
