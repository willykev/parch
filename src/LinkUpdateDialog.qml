import QtQuick
import QtQuick.Controls.Basic
import Parch

// Shown before a move that would break links, images or includes: every
// change it would make, by document and line, the line as it is and as it
// would be. Update links moves and updates them, each open document as one
// undo step and every other after history has kept what it held; Move only
// moves and leaves them; Cancel moves nothing.
ThemedDialog {
    id: dialog

    readonly property var review: backend.organizer.review
    readonly property real gap: Math.round(theme.fontSize / 2)

    title: review.title || ""
    preferredCharacters: 84
    preferredHeight: parent ? Math.round(parent.height * 0.8) : implicitHeight
    initialFocus: update

    onRejected: backend.organizer.cancelMove()

    contentItem: Item {
        implicitHeight: detail.implicitHeight + Math.round(theme.fontSize * 2.2) * 8

        Text {
            id: detail
            objectName: "linkUpdateDetail"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            text: dialog.review.detail || ""
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.foreground
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        ListView {
            id: list
            objectName: "linkUpdateChanges"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: detail.bottom
            anchors.topMargin: dialog.gap
            anchors.bottom: parent.bottom
            clip: true
            model: dialog.review.rows || []
            boundsBehavior: Flickable.StopAtBounds
            activeFocusOnTab: count > 0
            keyNavigationEnabled: true
            highlightMoveDuration: 0
            section.property: "path"
            section.criteria: ViewSection.FullString
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Links to update")

            section.delegate: Text {
                required property string section

                width: list.width
                topPadding: dialog.gap
                bottomPadding: Math.round(dialog.gap / 2)
                text: section.substring(section.lastIndexOf("/") + 1)
                textFormat: Text.PlainText
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                font.weight: Font.DemiBold
                color: theme.bright
                elide: Text.ElideMiddle
                Accessible.role: Accessible.Heading
                Accessible.name: text
            }

            delegate: Rectangle {
                id: change
                objectName: "linkUpdateChange"

                required property int index
                required property var modelData
                readonly property bool current: ListView.isCurrentItem

                width: list.width
                implicitHeight: lines.implicitHeight + dialog.gap
                height: implicitHeight
                radius: theme.radius
                color: current && list.activeFocus ? theme.hoverFill : "transparent"
                border.width: current && list.activeFocus ? Math.max(1, theme.controlBorderWidth) : 0
                border.color: theme.accent
                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, line %2: %3, becomes %4").arg(modelData.file).arg(modelData.line)
                                 .arg(modelData.before).arg(modelData.after)

                Column {
                    id: lines
                    x: dialog.gap
                    y: Math.round(dialog.gap / 2)
                    width: parent.width - 2 * dialog.gap

                    Text {
                        width: parent.width
                        text: qsTr("Line %1").arg(change.modelData.line)
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizes.small
                        color: theme.dim
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: "− " + change.modelData.before
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.fontSize
                        color: theme.dim
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: "+ " + change.modelData.after
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.fontSize
                        color: theme.foreground
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            text: qsTr("Cancel")
            Accessible.description: qsTr("Move nothing.")
            onClicked: dialog.reject()
        }
        ThemedDialog.Choice {
            objectName: "linkUpdateMoveOnly"
            text: qsTr("Move only")
            Accessible.description: qsTr("Move it, and leave these links as they are.")
            onClicked: {
                dialog.accept()
                backend.organizer.confirmMove(false)
            }
        }
        ThemedDialog.Choice {
            id: update
            objectName: "linkUpdateConfirm"
            text: qsTr("Update links")
            Accessible.description: qsTr("Move it, and update these links so that they still lead where they led.")
            onClicked: {
                dialog.accept()
                backend.organizer.confirmMove(true)
            }
        }
    }
}
