import QtQuick
import QtQuick.Controls.Basic
import "icons.js" as Icons

// A check box with its label, in the theme's control states: keyboard focus
// is drawn with the hover fill and a ring in the accent.
CheckBox {
    id: control

    readonly property real box: Math.round(theme.fontSize * 1.2)

    hoverEnabled: true
    padding: Math.round(theme.fontSize / 3)
    spacing: Math.round(theme.fontSize * 2 / 3)
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize

    indicator: Rectangle {
        x: control.leftPadding
        y: Math.round((control.height - height) / 2)
        implicitWidth: control.box
        implicitHeight: control.box
        radius: Math.min(theme.radius, width / 4)
        color: control.checked ? theme.accent : theme.background
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: control.checked ? theme.accent : theme.controlBorder

        Icon {
            anchors.centerIn: parent
            visible: control.checked
            path: Icons.check
            size: control.box
            color: theme.accentInk
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        color: control.enabled ? theme.foreground : theme.faint
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: theme.radius
        color: control.down ? theme.pressedFill : control.hovered || control.visualFocus ? theme.hoverFill : "transparent"
        border.width: control.visualFocus ? Math.max(1, theme.controlBorderWidth) : 0
        border.color: theme.accent
    }
}
