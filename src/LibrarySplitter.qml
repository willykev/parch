import QtQuick
import Parch

// The edge between the library and the text: a line in the theme's border
// colour, which a pointer drags a little either side of it, and which Tab
// reaches, where Left and Right make the library narrower and wider a step
// at a time, Page Up and Page Down by more, and Home and End as narrow and
// as wide as it goes. With the keys, it is drawn in the accent.
Item {
    id: splitter

    // The library's width now, and the widths it may take.
    property int libraryWidth
    property int minimum
    property int maximum
    readonly property int step: Math.max(1, Math.round(theme.fontSize))

    // The library is to take this width, within the range.
    signal resized(int width)
    // A drag, or a key, chose a width: where new windows start.
    signal chosen(int width)

    function resizeTo(width) {
        const kept = Math.max(minimum, Math.min(maximum, Math.round(width)))
        if (kept !== libraryWidth)
            resized(kept)
        return kept
    }

    activeFocusOnTab: true
    Accessible.role: Accessible.Splitter
    Accessible.name: qsTr("Library width")
    Accessible.description: qsTr("Left and Right make the library narrower or wider.")
    Accessible.focusable: true
    Accessible.focused: activeFocus

    Keys.onPressed: (event) => {
        let width = splitter.libraryWidth
        switch (event.key) {
        case Qt.Key_Left:
            width -= splitter.step
            break
        case Qt.Key_Right:
            width += splitter.step
            break
        case Qt.Key_PageUp:
            width -= 5 * splitter.step
            break
        case Qt.Key_PageDown:
            width += 5 * splitter.step
            break
        case Qt.Key_Home:
            width = splitter.minimum
            break
        case Qt.Key_End:
            width = splitter.maximum
            break
        default:
            return
        }
        event.accepted = true
        splitter.chosen(splitter.resizeTo(width))
    }

    Rectangle {
        anchors.fill: parent
        color: theme.controlBorder
    }

    // With the keys, a wider bar in the accent, over the line.
    Rectangle {
        objectName: "librarySplitterFocus"
        visible: splitter.activeFocus
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.max(2, Math.round(theme.fontSize / 5))
        color: theme.accent
    }

    // Where a pointer takes hold of the edge, a little either side of it.
    MouseArea {
        id: grip
        objectName: "librarySplitterGrip"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: Math.max(parent.width, Math.round(theme.fontSize * 2 / 3))
        cursorShape: Qt.SplitHCursor
        preventStealing: true

        property real pressedAt
        property int startWidth

        onPressed: (mouse) => {
            pressedAt = mapToItem(null, mouse.x, 0).x
            startWidth = splitter.libraryWidth
        }
        onPositionChanged: (mouse) => {
            if (pressed)
                splitter.resizeTo(startWidth + mapToItem(null, mouse.x, 0).x - pressedAt)
        }
        onReleased: splitter.chosen(splitter.libraryWidth)
    }
}
