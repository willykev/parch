import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Templates as T
import "icons.js" as Icons

// A choice from a list, in the theme's control states. Closed, the arrows
// change the choice and typing a letter finds one; Space opens the list,
// which scrolls when it is long. Keyboard focus is drawn with the hover fill
// and a ring in the accent.
ComboBox {
    id: control

    readonly property real gap: Math.round(theme.fontSize * 2 / 3)
    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)

    hoverEnabled: true
    implicitHeight: Math.round(theme.fontSize * 2.4)
    leftPadding: gap
    rightPadding: gap + indicator.width + Math.round(theme.fontSize / 3)
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize

    contentItem: Text {
        text: control.displayText
        textFormat: Text.PlainText
        font: control.font
        color: control.enabled ? theme.foreground : theme.faint
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    indicator: Icon {
        x: control.width - width - control.gap
        y: Math.round((control.height - height) / 2)
        path: Icons.chevronDown
        size: Math.round(theme.fontSize)
        color: control.hovered || control.visualFocus ? theme.bright : theme.dim
    }

    background: Rectangle {
        radius: theme.radius
        color: control.down ? theme.pressedFill : control.hovered || control.visualFocus ? theme.hoverFill : theme.background
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: control.visualFocus ? theme.accent : theme.controlBorder
    }

    delegate: ItemDelegate {
        id: row

        required property int index
        required property var modelData

        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: control.rowHeight
        leftPadding: control.gap
        rightPadding: control.gap
        highlighted: control.highlightedIndex === index
        font: control.font
        text: control.textRole ? modelData[control.textRole] : modelData

        contentItem: Text {
            text: row.text
            textFormat: Text.PlainText
            font: row.font
            color: row.highlighted ? theme.bright : theme.foreground
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: theme.radius
            color: row.down ? theme.pressedFill : row.highlighted ? theme.hoverFill
                   : control.currentIndex === row.index ? theme.selectedFill : "transparent"
        }
    }

    popup: T.Popup {
        y: control.height + Math.round(theme.fontSize / 4)
        width: control.width
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, 12 * control.rowHeight,
                         control.Window.height - 2 * theme.fontSize)
        padding: Math.max(1, theme.controlBorderWidth)

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
        }

        background: Rectangle {
            color: theme.menuBackground
            radius: theme.radius
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: theme.menuBorder
        }
    }
}
