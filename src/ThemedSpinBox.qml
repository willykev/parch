import QtQuick
import QtQuick.Controls.Basic
import Parch
import "icons.js" as Icons

// A number within a range, typed or stepped with the arrows or its buttons,
// in the theme's control states. `decimals` shows value / 10^decimals, for
// a number with a fraction.
SpinBox {
    id: control

    property int decimals: 0
    // What the number counts, after it, such as "px".
    property string unit
    readonly property real factor: Math.pow(10, decimals)
    readonly property real buttonWidth: Math.round(theme.fontSize * 2)

    hoverEnabled: true
    editable: true
    implicitHeight: Math.round(theme.fontSize * 2.4)
    implicitWidth: 9 * Math.round(theme.fontSize * 0.6) + 2 * buttonWidth
    leftPadding: buttonWidth
    rightPadding: buttonWidth
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize

    textFromValue: (value, locale) => {
        const shown = Number(value / factor).toLocaleString(locale, "f", decimals)
        return unit.length > 0 ? shown + " " + unit : shown
    }
    valueFromText: (text, locale) => {
        const number = Number.fromLocaleString(locale, text.replace(unit, "").trim())
        return Math.round(number * factor)
    }
    validator: RegularExpressionValidator { regularExpression: /[0-9.,]*\s*[a-z]*/ }

    contentItem: TextInput {
        text: control.displayText
        font: control.font
        color: control.enabled ? theme.foreground : theme.faint
        selectionColor: theme.selection
        selectedTextColor: theme.foreground
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        selectByMouse: true
        cursorDelegate: FieldCaret { rest: spinCaret }
        Accessible.name: control.Accessible.name

        CaretRest {
            id: spinCaret
            objectName: "caretRest"
            target: parent
            steady: theme.reduceMotion
        }
    }

    down.indicator: Item {
        x: 0
        height: control.height
        implicitWidth: control.buttonWidth

        Icon {
            anchors.centerIn: parent
            path: Icons.minus
            size: Math.round(theme.fontSize)
            color: !control.enabled || control.value <= control.from ? theme.faint
                   : control.down.hovered ? theme.bright : theme.foreground
        }
    }

    up.indicator: Item {
        x: control.width - width
        height: control.height
        implicitWidth: control.buttonWidth

        Icon {
            anchors.centerIn: parent
            path: Icons.plus
            size: Math.round(theme.fontSize)
            color: !control.enabled || control.value >= control.to ? theme.faint
                   : control.up.hovered ? theme.bright : theme.foreground
        }
    }

    background: Rectangle {
        radius: theme.radius
        color: control.hovered || control.visualFocus ? theme.hoverFill : theme.background
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: control.activeFocus ? theme.accent : theme.controlBorder
    }
}
