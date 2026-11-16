import QtQuick
import QtQuick.Controls.Basic

// A flat button in the states the Omarchy shell's own controls share: no
// fill at rest and the theme's hover and pressed fills. Focus that came from
// the keyboard is drawn plainly, with the hover fill and a ring in the
// accent. An icon button keeps its text as its accessible name; it and any
// longer detail are shown as a tooltip on hover and on keyboard focus. A
// labelled icon button shows its text beside the icon instead.
Button {
    id: control

    // A path from icons.js; with one the button shows the icon, not its text.
    property string iconPath
    // With an icon, the text is shown beside it as well, from the left.
    property bool labelled: false
    property real iconSize: Math.round(theme.fontSize * 4 / 3)
    // The keys that do the same, named in the tooltip.
    property string keys
    // More than the text says, for the tooltip and assistive technology.
    property string detail
    // The tooltip opens above the button, for a button at the window's foot.
    property bool tipAbove: false
    property color textColor: theme.foreground
    // A fine edge at rest, for a button among others in a dialog, where a
    // bare label would not read as a button.
    property bool framed: false

    readonly property bool iconShown: iconPath.length > 0
    readonly property bool textShown: iconPath.length === 0 || labelled
    readonly property string tipText: detail.length > 0 ? detail
                                      : textShown ? ""
                                      : keys.length > 0 ? qsTr("%1 (%2)").arg(text).arg(keys) : text
    readonly property real gap: Math.round(theme.fontSize / 3)

    flat: true
    hoverEnabled: true
    padding: gap
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize
    Accessible.description: detail

    contentItem: Item {
        // Between the icon and the text, when both are shown.
        readonly property real spacing: control.iconShown && control.textShown ? Math.round(theme.fontSize * 2 / 3) : 0

        implicitWidth: (control.iconShown ? control.iconSize : 0) + spacing + (control.textShown ? label.implicitWidth : 0)
        implicitHeight: Math.max(control.iconShown ? control.iconSize : 0, control.textShown ? label.implicitHeight : 0)

        Icon {
            id: icon
            visible: control.iconShown
            x: control.textShown ? 0 : Math.round((parent.width - width) / 2)
            anchors.verticalCenter: parent.verticalCenter
            path: control.iconPath
            size: control.iconSize
            color: !control.enabled ? theme.faint : control.hovered || control.visualFocus ? theme.bright : control.textColor
        }

        Text {
            id: label
            visible: control.textShown
            x: control.iconShown ? icon.width + parent.spacing : 0
            width: parent.width - x
            height: parent.height
            text: control.text
            textFormat: Text.PlainText
            font: control.font
            color: control.enabled ? control.textColor : theme.faint
            elide: Text.ElideRight
            horizontalAlignment: control.iconShown ? Text.AlignLeft : Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    background: Rectangle {
        radius: theme.radius
        color: control.down ? theme.pressedFill : control.hovered || control.visualFocus ? theme.hoverFill : "transparent"
        border.width: control.visualFocus || control.framed ? Math.max(1, theme.controlBorderWidth) : 0
        border.color: control.visualFocus ? theme.accent : theme.controlBorder

        Behavior on color {
            ColorAnimation { duration: theme.reduceMotion ? 0 : 80 }
        }
    }

    ToolTip {
        id: tip
        objectName: "toolTip"
        visible: (control.hovered || control.visualFocus) && control.tipText.length > 0
        // At once for the keyboard; for the pointer only once it rests.
        delay: control.visualFocus ? 0 : 600
        // Clear of the button, and never wider than the window, where it wraps.
        y: control.tipAbove ? -height - control.gap : control.height + control.gap
        width: Math.min(implicitWidth, control.Window.width - 2 * margins)
        text: control.tipText
        font: control.font
        padding: Math.round(theme.fontSize / 2)

        contentItem: Text {
            text: tip.text
            textFormat: Text.PlainText
            font: tip.font
            color: theme.foreground
            wrapMode: Text.Wrap
        }

        background: Rectangle {
            color: theme.menuBackground
            radius: theme.radius
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: theme.menuBorder
        }
    }
}
