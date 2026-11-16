import QtQuick
import QtQuick.Controls.Basic

// The window's menu: its actions with the keys that do the same, so that
// every action can be found without knowing its keys. The keys are the
// window's own shortcuts' text, so the two cannot disagree.
Menu {
    id: menu

    // The text of the window's shortcuts, by action.
    property var keys: ({})
    property bool libraryShown: false
    // Whether the front tab has a document a second view can show.
    property bool canNewView: true
    // Whether the front tab has a document to save or look back through.
    property bool hasDocument: true

    signal newDraft()
    signal newWindow()
    signal newView()
    signal openFiles()
    signal openFolder()
    signal save()
    signal saveAs()
    signal showHistory()
    signal checkLinks()
    signal closeTab()
    signal findDocument()
    signal toggleLibrary()
    signal zoomIn()
    signal zoomOut()
    signal resetZoom()
    signal quit()

    readonly property real gap: Math.round(theme.fontSize * 2 / 3)
    readonly property real inset: Math.round(theme.fontSize / 4)
    // The advance of "0" in the interface font.
    readonly property TextMetrics zero: TextMetrics {
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: "0"
    }

    padding: Math.max(1, theme.controlBorderWidth)
    topPadding: inset
    bottomPadding: inset
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize

    // The Basic menu's own list fades its scroll indicator in and out at a
    // fixed pace; this one has none, and scrolls only if it must.
    contentItem: ListView {
        // As wide as the widest entry, so that none is cut short.
        implicitWidth: {
            let widest = 0
            for (let i = 0; i < menu.count; ++i)
                widest = Math.max(widest, menu.itemAt(i).implicitWidth)
            return widest
        }
        implicitHeight: contentHeight
        model: menu.contentModel
        interactive: Window.window ? contentHeight + menu.topPadding + menu.bottomPadding > menu.height : false
        clip: true
        currentIndex: menu.currentIndex
    }

    background: Rectangle {
        implicitWidth: 20 * menu.zero.advanceWidth
        color: theme.menuBackground
        radius: theme.radius
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: theme.menuBorder
    }

    MenuEntry {
        text: qsTr("New draft")
        keys: menu.keys.newDraft || ""
        onTriggered: menu.newDraft()
    }
    MenuEntry {
        text: qsTr("New window")
        keys: menu.keys.newWindow || ""
        onTriggered: menu.newWindow()
    }
    // A second tab on the document in front, with a cursor and scroll of its own.
    MenuEntry {
        objectName: "newViewEntry"
        text: qsTr("New view")
        enabled: menu.canNewView
        onTriggered: menu.newView()
    }
    MenuLine {}
    MenuEntry {
        text: qsTr("Open…")
        keys: menu.keys.open || ""
        onTriggered: menu.openFiles()
    }
    MenuEntry {
        text: qsTr("Open folder…")
        onTriggered: menu.openFolder()
    }
    MenuEntry {
        text: qsTr("Find a document…")
        keys: menu.keys.find || ""
        onTriggered: menu.findDocument()
    }
    MenuEntry {
        text: qsTr("Save")
        keys: menu.keys.save || ""
        enabled: menu.hasDocument
        onTriggered: menu.save()
    }
    MenuEntry {
        text: qsTr("Save as…")
        keys: menu.keys.saveAs || ""
        enabled: menu.hasDocument
        onTriggered: menu.saveAs()
    }
    MenuEntry {
        objectName: "historyEntry"
        text: qsTr("Version history…")
        enabled: menu.hasDocument
        onTriggered: menu.showHistory()
    }
    MenuEntry {
        objectName: "checkLinksEntry"
        text: qsTr("Check links…")
        enabled: menu.hasDocument
        onTriggered: menu.checkLinks()
    }
    MenuEntry {
        text: qsTr("Close tab")
        keys: menu.keys.closeTab || ""
        onTriggered: menu.closeTab()
    }
    MenuLine {}
    MenuEntry {
        text: menu.libraryShown ? qsTr("Hide library") : qsTr("Show library")
        keys: menu.keys.library || ""
        onTriggered: menu.toggleLibrary()
    }
    MenuEntry {
        text: qsTr("Larger text")
        keys: menu.keys.zoomIn || ""
        onTriggered: menu.zoomIn()
    }
    MenuEntry {
        text: qsTr("Smaller text")
        keys: menu.keys.zoomOut || ""
        onTriggered: menu.zoomOut()
    }
    MenuEntry {
        text: qsTr("Theme text size")
        keys: menu.keys.resetZoom || ""
        onTriggered: menu.resetZoom()
    }
    MenuLine {}
    MenuEntry {
        text: qsTr("Quit")
        keys: menu.keys.quit || ""
        onTriggered: menu.quit()
    }

    // A row of the menu: what it does on the left, its keys on the right.
    component MenuEntry: MenuItem {
        id: entry

        property string keys

        implicitHeight: Math.round(theme.fontSize * 2.2)
        leftPadding: menu.gap
        rightPadding: menu.gap
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize

        contentItem: Item {
            implicitWidth: name.implicitWidth + 3 * menu.zero.advanceWidth + shortcut.implicitWidth
            implicitHeight: name.implicitHeight

            Text {
                id: name
                anchors.left: parent.left
                anchors.right: shortcut.left
                anchors.rightMargin: menu.zero.advanceWidth
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
        }

        background: Rectangle {
            radius: theme.radius
            color: entry.down ? theme.pressedFill : entry.highlighted ? theme.hoverFill : "transparent"
        }
    }

    component MenuLine: MenuSeparator {
        topPadding: menu.inset
        bottomPadding: menu.inset

        contentItem: Rectangle {
            implicitHeight: Math.max(1, theme.controlBorderWidth)
            color: theme.controlBorder
        }
    }
}
