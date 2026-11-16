import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Templates as T
import Parch

// A dialog over the window, drawn as the shell draws its floating surfaces:
// the menu colours, the theme's corners, a fine edge, and the scrim over
// what is behind. It takes the keys while it is open, with focus on its
// safe choice, Tab moves through its controls without leaving it, and
// Escape cancels it. It is never wider or taller than the window allows.
T.Dialog {
    id: dialog

    // What has focus when the dialog opens: its safe choice.
    property Item initialFocus
    // How wide the dialog would like to be, in characters of the interface font.
    property int preferredCharacters: 56
    // How tall it would like to be; its content's own height unless it says.
    property real preferredHeight: implicitHeight

    readonly property real gap: Math.round(theme.fontSize * 2 / 3)
    // The advance of "0" in the interface font, the unit of every measure here.
    readonly property real characterWidth: zero.advanceWidth
    // What is kept clear around the dialog inside the window.
    readonly property real margin: Math.round(theme.fontSize)

    parent: T.Overlay.overlay
    anchors.centerIn: parent
    modal: true
    dim: true
    focus: true
    closePolicy: T.Popup.CloseOnEscape
    width: Math.min(parent ? parent.width - 2 * margin : implicitWidth,
                    preferredCharacters * characterWidth + leftPadding + rightPadding)
    height: Math.min(parent ? parent.height - 2 * margin : implicitHeight, preferredHeight)
    padding: Math.round(theme.fontSize)
    topPadding: Math.round(theme.fontSize / 2)
    spacing: 0
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding,
                            implicitHeaderWidth, implicitFooterWidth)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding
                             + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
                             + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0))

    // On the safe choice, drawn as keyboard focus is, so that it is plain
    // which control Enter and Space act on.
    onOpened: if (initialFocus) initialFocus.forceActiveFocus(Qt.TabFocusReason)

    TextMetrics {
        id: zero
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: "0"
    }

    // One line, elided, of a height the theme decides rather than its text,
    // so that the dialog's size never waits on laying the title out.
    header: Item {
        implicitHeight: heading.height + dialog.leftPadding + Math.round(theme.fontSize / 2)

        FontMetrics {
            id: titleMetrics
            font: heading.font
        }

        Text {
            id: heading
            x: dialog.leftPadding
            y: dialog.leftPadding
            width: parent.width - dialog.leftPadding - dialog.rightPadding
            height: Math.ceil(Math.max(titleMetrics.height, titleMetrics.lineSpacing))
            text: dialog.title
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.subtitle
            font.weight: Font.DemiBold
            color: theme.bright
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            Accessible.role: Accessible.Heading
            Accessible.name: text
        }
    }

    background: Rectangle {
        color: theme.menuBackground
        radius: theme.radius
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: theme.menuBorder
    }

    T.Overlay.modal: Rectangle { color: theme.scrim }

    // The choices along a dialog's foot, from the left, kept to the right
    // edge and wrapping onto another line in a narrow window.
    component Choices: Item {
        id: foot

        default property alias buttons: row.data
        readonly property real gap: Math.round(theme.fontSize / 2)
        readonly property real padding: Math.round(theme.fontSize)
        // The width the buttons take on one line.
        readonly property real natural: {
            let width = 0
            let shown = 0
            for (let i = 0; i < row.children.length; ++i) {
                const button = row.children[i]
                if (!button.visible)
                    continue
                width += button.implicitWidth
                ++shown
            }
            return width + Math.max(0, shown - 1) * row.spacing
        }

        implicitWidth: natural + 2 * padding
        implicitHeight: row.implicitHeight + padding + gap

        Flow {
            id: row
            anchors.right: parent.right
            anchors.rightMargin: foot.padding
            y: foot.gap
            width: Math.min(foot.natural, Math.max(0, foot.width - 2 * foot.padding))
            spacing: foot.gap
        }
    }

    // A choice of a dialog: a button with a fine edge at rest, and Enter
    // acting on it as Space does.
    component Choice: ThemedButton {
        id: choice

        framed: true
        horizontalPadding: Math.round(theme.fontSize)
        Keys.onReturnPressed: choice.clicked()
        Keys.onEnterPressed: choice.clicked()
    }

    // Text to read and not to edit, in a pane of its own that scrolls; the
    // keyboard reaches it to scroll and to select and copy from it.
    component TextPane: Rectangle {
        id: pane

        property string caption
        property string text
        // Said in place of the text, when it cannot be shown.
        property string note
        // The runs of the text that differ, each marked behind it from its
        // first position to its last, as the text view counts them, as
        // {start, end}; one whose start and end are the same is a line where
        // the other text has more.
        property var marks: []
        readonly property real inset: Math.round(theme.fontSize / 2)

        // Brings the first run into view, a little below the top, once the
        // text has its layout.
        function revealMark() {
            const first = markRepeater.count > 0 ? markRepeater.itemAt(0) : null
            if (!first || !first.visible)
                return
            flick.contentY = Math.max(0, Math.min(first.y - flick.height / 4, flick.contentHeight - flick.height))
        }
        onMarksChanged: Qt.callLater(pane.revealMark)
        onTextChanged: Qt.callLater(pane.revealMark)

        color: theme.background
        radius: theme.radius
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: body.activeFocus ? theme.accent : theme.controlBorder

        Text {
            id: label
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: pane.inset
            text: pane.caption
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.small
            color: theme.dim
            elide: Text.ElideRight
            Accessible.ignored: true
        }

        Flickable {
            id: flick
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: label.bottom
            anchors.bottom: parent.bottom
            anchors.margins: pane.inset
            anchors.topMargin: Math.round(theme.fontSize / 4)
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            TextArea.flickable: TextArea {
                id: body
                objectName: "paneText"
                readOnly: true
                text: pane.note.length > 0 ? pane.note : pane.text
                textFormat: TextEdit.PlainText
                wrapMode: TextEdit.Wrap
                selectByMouse: true
                selectByKeyboard: true
                activeFocusOnTab: true
                padding: 0
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                color: pane.note.length > 0 ? theme.dim : theme.foreground
                selectionColor: theme.selection
                selectedTextColor: theme.foreground
                cursorDelegate: FieldCaret { rest: bodyCaret }
                background: null
                Accessible.name: pane.caption
                Accessible.description: pane.note

                CaretRest {
                    id: bodyCaret
                    objectName: "caretRest"
                    target: body
                    steady: theme.reduceMotion
                }

                // Behind the text, each run of lines that differ, in the
                // theme's selection colour: from the top of its first line
                // to the bottom of its last, or a bar where the other text
                // has lines this one lacks.
                Repeater {
                    id: markRepeater
                    model: pane.note.length > 0 ? [] : pane.marks

                    delegate: Rectangle {
                        required property var modelData

                        readonly property rect band: {
                            // Laid out again whenever the text or its width changes.
                            const depends = body.contentHeight + body.width + body.length
                            const start = modelData.start, end = modelData.end
                            // Nothing, too, until the text shown is the text the
                            // runs were found in.
                            if (start < 0 || depends < 0 || end > body.length || start > body.length)
                                return Qt.rect(0, 0, 0, 0)
                            const first = body.positionToRectangle(start)
                            if (end <= start)
                                return Qt.rect(0, first.y, body.width, Math.max(2, 2 * theme.controlBorderWidth))
                            const last = body.positionToRectangle(Math.max(start, end - 1))
                            return Qt.rect(0, first.y, body.width, last.y + last.height - first.y)
                        }

                        objectName: "differenceMark"
                        z: -1
                        visible: band.height > 0
                        x: band.x
                        y: band.y
                        width: band.width
                        height: band.height
                        color: theme.selection
                    }
                }
            }

            ScrollBar.vertical: Scroller {}
        }
    }

    // A scroll bar that shows while there is more to scroll to, without
    // fading in or out.
    component Scroller: T.ScrollBar {
        id: scroller

        padding: Math.max(1, theme.controlBorderWidth)
        implicitWidth: Math.round(theme.fontSize / 2) + leftPadding + rightPadding
        implicitHeight: Math.round(theme.fontSize / 2) + topPadding + bottomPadding
        minimumSize: 0.1
        visible: size < 1

        contentItem: Rectangle {
            implicitWidth: Math.round(theme.fontSize / 2)
            implicitHeight: Math.round(theme.fontSize / 2)
            radius: Math.min(theme.radius, width / 2)
            color: scroller.pressed ? theme.dim : theme.muted
        }
    }
}
