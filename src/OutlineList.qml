import QtQuick
import QtQuick.Controls.Basic
import Parch
import "icons.js" as Icons

// The outline of the document in front, beneath the library's folders: a
// row that shows and hides it, which the settings keep, and the document's
// headings, indented by level, the one the caret is under marked. Up and
// Down move through them, and Enter, Space or a click goes to one. Nothing
// is looked for while it is hidden.
Item {
    id: section

    property Document document: null
    // Where the caret is, as a source offset.
    property int caret: 0
    // The most the headings take, so that the folders keep room above.
    property real maxListHeight: 200

    // A heading was chosen: the window puts its line at the top of the view.
    signal chosen(int position)
    // A heading an include brought in was chosen: the window opens its file there.
    signal includedChosen(string path, int line)

    function choose(row) {
        const file = outline.fileAt(row)
        if (file.length > 0)
            section.includedChosen(file, outline.fileLineAt(row))
        else if (row >= 0)
            section.chosen(outline.positionAt(row))
    }

    // The keys go to the headings. Until they are found, and when there are
    // none, the row that shows and hides them has the keys.
    property bool keysPending: false
    function takeKeys() {
        if (open && list.count > 0) {
            list.forceActiveFocus(Qt.TabFocusReason)
        } else {
            header.forceActiveFocus(Qt.TabFocusReason)
            keysPending = open
        }
    }

    readonly property bool open: settings.outlineShown
    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)
    readonly property real indent: Math.round(theme.fontSize)
    readonly property int currentRow: outline.ready ? outline.rowAt(caret) : -1

    implicitHeight: header.height + (open ? Math.min(Math.max(list.contentHeight, empty.implicitHeight), maxListHeight) : 0)

    OutlineModel {
        id: outline
        objectName: "libraryOutline"
        document: section.document
        active: section.open && section.visible
    }

    ThemedButton {
        id: header
        objectName: "outlineToggle"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: section.rowHeight
        iconPath: section.open ? Icons.chevronDown : Icons.chevronRight
        iconSize: Math.round(theme.fontSize * 0.9)
        labelled: true
        text: qsTr("Outline")
        textColor: theme.dim
        Accessible.description: section.open ? qsTr("Hide the headings of the document in front")
                                             : qsTr("Show the headings of the document in front")
        onClicked: settings.outlineShown = !settings.outlineShown
    }

    ListView {
        id: list
        objectName: "libraryOutlineList"
        visible: section.open
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        clip: true
        model: outline
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: count > 0
        keyNavigationEnabled: true
        highlightMoveDuration: 0
        currentIndex: section.currentRow
        Accessible.role: Accessible.List
        Accessible.name: qsTr("Outline")
        // The headings were asked for before they were found, and the keys
        // are still waiting on the row above.
        onCountChanged: {
            if (section.keysPending && count > 0 && header.activeFocus)
                forceActiveFocus(Qt.TabFocusReason)
            if (count > 0 || !header.activeFocus)
                section.keysPending = false
        }
        Keys.onReturnPressed: section.choose(currentIndex)
        Keys.onEnterPressed: section.choose(currentIndex)
        Keys.onSpacePressed: section.choose(currentIndex)

        delegate: AbstractButton {
            id: entry
            objectName: "outlineRow"

            required property int index
            required property string heading
            required property int level
            required property int position

            readonly property bool marked: index === section.currentRow
            readonly property bool keyboard: ListView.isCurrentItem && list.activeFocus

            width: list.width
            height: section.rowHeight
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            leftPadding: Math.round(theme.fontSize / 3) + (level - 1) * section.indent
            rightPadding: Math.round(theme.fontSize / 3)
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            Accessible.role: Accessible.ListItem
            text: heading
            Accessible.name: qsTr("%1, level %2").arg(heading).arg(level)
            Accessible.selected: marked
            onClicked: section.choose(index)

            contentItem: Text {
                text: entry.text
                textFormat: Text.PlainText
                font: entry.font
                color: entry.marked ? theme.accent : theme.foreground
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: theme.radius
                color: entry.down ? theme.pressedFill : entry.hovered || entry.keyboard ? theme.hoverFill : "transparent"
                border.width: entry.keyboard ? Math.max(1, theme.controlBorderWidth) : 0
                border.color: theme.accent
            }
        }
    }

    Text {
        id: empty
        visible: section.open && outline.ready && outline.count === 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        leftPadding: Math.round(theme.fontSize / 2)
        text: section.document ? qsTr("No headings") : qsTr("No document in front")
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.sizes.small
        color: theme.dim
        elide: Text.ElideRight
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
}
