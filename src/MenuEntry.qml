import QtQuick
import QtQuick.Controls.Basic
import "icons.js" as Icons

// A row of a ThemedMenu: what it does on the left, and on the right its
// keys, or the arrow of a section that opens beside the menu.
MenuItem {
    id: entry

    // The text of the keys that do the same.
    property string keys

    readonly property real gap: Math.round(theme.fontSize * 2 / 3)

    implicitHeight: Math.round(theme.fontSize * 2.2)
    leftPadding: gap
    rightPadding: gap
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize
    arrow: null
    indicator: null

    TextMetrics {
        id: zero
        font: entry.font
        text: "0"
    }

    contentItem: Item {
        implicitWidth: name.implicitWidth + 3 * zero.advanceWidth + Math.max(shortcut.implicitWidth, opens.width)
        implicitHeight: name.implicitHeight

        Text {
            id: name
            anchors.left: parent.left
            anchors.right: shortcut.left
            anchors.rightMargin: zero.advanceWidth
            anchors.verticalCenter: parent.verticalCenter
            text: entry.text
            textFormat: Text.PlainText
            font: entry.font
            color: !entry.enabled ? theme.dim : entry.highlighted ? theme.bright : theme.foreground
            elide: Text.ElideRight
        }

        Text {
            id: shortcut
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: entry.keys
            textFormat: Text.PlainText
            font: entry.font
            color: theme.dim
        }

        Icon {
            id: opens
            visible: entry.subMenu !== null
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: visible ? size : 0
            path: Icons.chevronRight
            size: Math.round(theme.fontSize)
            color: entry.highlighted ? theme.bright : theme.dim
        }
    }

    background: Rectangle {
        radius: theme.radius
        color: entry.down ? theme.pressedFill : entry.highlighted ? theme.hoverFill : "transparent"
    }
}
