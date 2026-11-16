import QtQuick

// The line between a document's text and its preview. Dragging it gives one
// pane more of the width and the other less; with the keys, Left and Right
// do the same in steps, and Home puts it back in the middle. It says where
// it wants to be with moved(); whoever lays out the panes keeps the place.
Item {
    id: divider

    // The part of the width the text takes, and the least and the most it may.
    property real value: 0.5
    property real from: 0.2
    property real to: 0.8
    property real stepSize: 0.02
    // The width the two panes share.
    property real room: 0

    signal moved(real value)

    function moveTo(place) {
        const next = Math.max(from, Math.min(to, place))
        if (next !== value)
            moved(next)
    }

    width: Math.max(8, Math.round(theme.fontSize * 2 / 3))
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Divider between the text and the preview")
    Accessible.description: qsTr("The Left and Right keys move it, and Home puts it in the middle.")
    Accessible.focusable: true

    Keys.onLeftPressed: moveTo(value - stepSize)
    Keys.onRightPressed: moveTo(value + stepSize)
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Home && event.modifiers === Qt.NoModifier) {
            moveTo(0.5)
            event.accepted = true
        }
    }

    // The line itself, plainer while it is held, pointed at or has the keys.
    Rectangle {
        objectName: "dividerLine"
        readonly property bool marked: divider.activeFocus || drag.active || hover.hovered
        anchors.horizontalCenter: parent.horizontalCenter
        width: marked ? Math.max(2, 2 * theme.controlBorderWidth) : Math.max(1, theme.controlBorderWidth)
        height: parent.height
        color: marked ? theme.accent : theme.controlBorder
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.SplitHCursor
    }

    DragHandler {
        id: drag
        property real startValue: 0
        target: null
        yAxis.enabled: false
        cursorShape: Qt.SplitHCursor
        onActiveChanged: if (active) startValue = divider.value
        onActiveTranslationChanged: if (active && divider.room > 0) divider.moveTo(startValue + activeTranslation.x / divider.room)
    }
}
