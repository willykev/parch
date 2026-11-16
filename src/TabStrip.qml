import QtQuick
import QtQuick.Controls.Basic
import Parch
import "icons.js" as Icons

// The window's tabs, shown only while there is more than one. Each tab takes
// the width of its name while they all fit. When they do not, they share the
// room equally, each keeping room for a readable name, and when even that is
// too little, as many as fit whole are shown and the strip scrolls sideways
// a whole tab at a time, so that no tab is ever cut off at the new tab button
// and the current tab is always wholly in view. An end with more tabs beyond
// it shows a chevron, in a gutter of its own beside the tabs.
// The model is the window's TabModel: each tab has a `title`, a `status` and
// a `document`, which is null while the tab has none. The notice bar speaks
// only of the document in front, so a tab whose save failed, whose file is
// in conflict or that is read-only carries a mark of the theme's danger or
// warning colour, edged as the text is so that it reads in any theme, and
// says why to assistive technology.
Rectangle {
    id: strip

    property alias model: tabs.model
    property int currentIndex: 0
    // The text of the window's shortcuts, by action.
    property var keys: ({})
    // The model's own count, which follows a closed tab at once; the list's
    // follows only when it next lays out, and depends on its own size.
    readonly property int count: tabs.model && tabs.model.count !== undefined ? tabs.model.count : tabs.count
    readonly property real gap: Math.round(theme.fontSize * 2 / 3)
    readonly property real closeSize: Math.round(theme.fontSize * 1.5)
    // Fourteen characters of a name beside the close button, at least, and
    // forty at most before the name is cut short.
    readonly property real minimumTabWidth: Math.ceil(14 * zero.advanceWidth + closeSize + 3 * gap)
    readonly property real maximumTabWidth: Math.ceil(40 * zero.advanceWidth + closeSize + 3 * gap)
    readonly property real inset: Math.round(theme.fontSize / 4)
    readonly property real markWidth: Math.round(theme.fontSize / 2) + 2 * Math.max(1, theme.controlBorderWidth)
    // Room for the tabs: the strip less the new tab button beside them.
    readonly property real room: strip.width - newButton.width - 2 * inset
    // What the tabs would take at the width of their names, as measure() finds it.
    property real naturalTotal: 0
    // The tabs do not all fit at the width of their names, so they share the
    // room: as many as fit whole at the minimum width are shown at once.
    readonly property bool squeezed: naturalTotal > room
    // Not even that is enough, so the tabs scroll beside the new tab button.
    readonly property bool overflowing: squeezed && count > Math.max(1, Math.floor(room / minimumTabWidth))
    // While they scroll, a gutter at each end of the view holds the mark
    // that more tabs lie beyond it, so that no mark covers a tab.
    readonly property real edgeRoom: Math.round(theme.fontSize)
    readonly property real tabRoom: overflowing ? room - 2 * edgeRoom : room
    readonly property int shownTabs: Math.max(1, Math.min(count, Math.floor(tabRoom / minimumTabWidth)))
    readonly property real sharedWidth: Math.floor(tabRoom / shownTabs)

    // What needs the writer's attention in a tab's document: "danger", "warning" or nothing.
    function attentionOf(document) {
        if (!document)
            return ""
        switch (document.state) {
        case Document.SaveFailed:
        case Document.Conflict:
            return "danger"
        case Document.ReadOnly:
            return "warning"
        default:
            return ""
        }
    }
    // The width of a tab with this name, with or without its mark.
    function naturalWidth(title, marked) {
        return Math.max(minimumTabWidth, Math.min(maximumTabWidth,
                Math.ceil((marked ? markWidth + gap / 2 : 0) + metrics.advanceWidth(title)) + closeSize + 3 * gap))
    }
    function measure() {
        let total = 0
        const model = strip.model
        if (model && model.index !== undefined) {
            for (let i = 0; i < strip.count; ++i) {
                const index = model.index(i, 0)
                total += naturalWidth(model.data(index, TabModel.TitleRole),
                                      attentionOf(model.data(index, TabModel.DocumentRole)).length > 0)
            }
        }
        naturalTotal = total
    }
    // Brings the current tab wholly into view. After a change of width the
    // tabs are laid out at their new widths only once the change is through,
    // so then it waits for that, which is still before the next frame.
    function keepCurrentInView() {
        if (tabs.currentIndex < 0)
            return
        tabs.forceLayout()
        tabs.positionViewAtIndex(tabs.currentIndex, ListView.Contain)
    }
    onCountChanged: Qt.callLater(measure)
    onMinimumTabWidthChanged: Qt.callLater(measure)
    onSharedWidthChanged: Qt.callLater(keepCurrentInView)
    onSqueezedChanged: Qt.callLater(keepCurrentInView)
    Component.onCompleted: measure()

    Connections {
        target: strip.model
        ignoreUnknownSignals: true
        function onDataChanged() { Qt.callLater(strip.measure) }
        function onModelReset() { Qt.callLater(strip.measure) }
    }

    FontMetrics {
        id: metrics
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
    }

    signal activated(int index)
    signal closeRequested(int index)
    signal newRequested()

    visible: count > 1
    color: theme.panel

    TextMetrics {
        id: zero
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: "0"
    }

    ListView {
        id: tabs
        objectName: "tabs"
        anchors.left: parent.left
        anchors.leftMargin: strip.overflowing ? strip.edgeRoom : 0
        anchors.top: parent.top
        anchors.bottom: line.top
        // Shared, the view holds exactly the tabs shown, so that its edge is a tab's.
        width: strip.squeezed ? strip.shownTabs * strip.sharedWidth : strip.room
        orientation: ListView.Horizontal
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapToItem
        currentIndex: strip.currentIndex
        // The current tab is brought into view at once, never by an animation.
        highlightFollowsCurrentItem: false
        onCurrentIndexChanged: strip.keepCurrentInView()
        onWidthChanged: Qt.callLater(strip.keepCurrentInView)

        delegate: AbstractButton {
            id: tab
            objectName: "tab"

            required property Document document
            required property string title
            required property string status
            required property int index
            readonly property bool current: index === strip.currentIndex
            // What needs the writer's attention here: "danger", "warning" or nothing.
            readonly property string attention: strip.attentionOf(document)

            width: strip.squeezed ? strip.sharedWidth : strip.naturalWidth(title, attention.length > 0)
            height: ListView.view.height
            text: title
            hoverEnabled: true
            Accessible.role: Accessible.PageTab
            Accessible.name: text
            Accessible.description: status
            onClicked: strip.activated(index)

            contentItem: Item {
                Rectangle {
                    id: mark
                    objectName: "tabMark"
                    visible: tab.attention.length > 0
                    anchors.left: parent.left
                    anchors.leftMargin: strip.gap
                    anchors.verticalCenter: parent.verticalCenter
                    width: strip.markWidth
                    height: width
                    // A light, as the notice bar's, and so round whatever the theme's corners.
                    radius: width / 2
                    color: tab.attention === "warning" ? theme.warning : theme.danger
                    border.width: Math.max(1, theme.controlBorderWidth)
                    border.color: theme.foreground
                    Accessible.role: Accessible.StaticText
                    Accessible.name: tab.status
                    Accessible.description: qsTr("%1 needs attention: %2").arg(tab.text).arg(tab.status)
                }

                Text {
                    id: name
                    anchors.left: mark.visible ? mark.right : parent.left
                    anchors.leftMargin: mark.visible ? strip.gap / 2 : strip.gap
                    anchors.right: close.left
                    anchors.rightMargin: strip.gap
                    anchors.verticalCenter: parent.verticalCenter
                    text: tab.text
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: tab.current ? theme.bright : theme.foreground
                    elide: Text.ElideRight
                }

                ThemedButton {
                    id: close

                    // Where the button lies along the strip's scrolled view.
                    readonly property real viewX: tab.x + tab.contentItem.x + x - tabs.contentX

                    // Shown only while wholly in view, where it can be pressed;
                    // the software renderer would also draw an icon lying
                    // wholly past the strip's clip.
                    visible: viewX >= 0 && viewX + width <= tabs.width
                    anchors.right: parent.right
                    anchors.rightMargin: strip.gap / 2
                    anchors.verticalCenter: parent.verticalCenter
                    width: strip.closeSize
                    height: strip.closeSize
                    padding: 0
                    iconPath: Icons.close
                    iconSize: Math.round(theme.fontSize * 0.9)
                    text: qsTr("Close %1").arg(tab.text)
                    keys: tab.current ? strip.keys.closeTab || "" : ""
                    onClicked: strip.closeRequested(tab.index)
                }
            }

            background: Rectangle {
                radius: theme.radius
                color: tab.down ? theme.pressedFill : tab.hovered || tab.visualFocus ? theme.hoverFill : "transparent"
                border.width: tab.visualFocus ? Math.max(1, theme.controlBorderWidth) : 0
                border.color: theme.accent

                // The current tab is underlined in the accent.
                Rectangle {
                    visible: tab.current
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: Math.max(2, 2 * theme.controlBorderWidth)
                    color: theme.accent
                }

                // A fine line between one tab and the next.
                Rectangle {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: theme.controlBorderWidth
                    color: theme.controlBorder
                }
            }
        }
    }

    // Where the tabs are cut off by the button, a line marks the edge.
    Rectangle {
        visible: strip.overflowing
        anchors.left: tabs.right
        anchors.top: parent.top
        anchors.bottom: line.top
        width: theme.controlBorderWidth
        color: theme.controlBorder
    }

    // More tabs lie before the view's start or past its end: a chevron in
    // the gutter there points to them.
    Icon {
        objectName: "tabsBeforeMark"
        visible: strip.overflowing && !tabs.atXBeginning
        x: Math.round((strip.edgeRoom - width) / 2)
        anchors.verticalCenter: tabs.verticalCenter
        size: Math.round(theme.fontSize * 0.75)
        path: Icons.chevronRight
        rotation: 180
        color: theme.dim
    }

    Icon {
        objectName: "tabsAfterMark"
        visible: strip.overflowing && !tabs.atXEnd
        x: tabs.x + tabs.width + Math.round((strip.edgeRoom - width) / 2)
        anchors.verticalCenter: tabs.verticalCenter
        size: Math.round(theme.fontSize * 0.75)
        path: Icons.chevronRight
        color: theme.dim
    }

    // Right after the last tab, or at the end while the tabs overflow.
    ThemedButton {
        id: newButton
        objectName: "newTabButton"
        x: tabs.x + Math.min(tabs.contentWidth, tabs.width) + (strip.overflowing ? strip.edgeRoom : 0) + strip.inset
        anchors.verticalCenter: tabs.verticalCenter
        width: tabs.height - 2 * strip.inset
        height: width
        iconPath: Icons.plus
        text: qsTr("New draft")
        keys: strip.keys.newDraft || ""
        onClicked: strip.newRequested()
    }

    Rectangle {
        id: line
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: theme.controlBorderWidth
        color: theme.controlBorder
    }
}
