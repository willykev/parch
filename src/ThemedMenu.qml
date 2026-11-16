import QtQuick
import QtQuick.Controls.Basic

// A menu drawn as the shell draws its own: the menu colours, the theme's
// corners and a fine edge, each row a MenuEntry, as wide as its widest
// entry. A ThemedMenu inside another is a section that opens beside it.
Menu {
    id: menu

    padding: Math.max(1, theme.controlBorderWidth)
    topPadding: Math.round(theme.fontSize / 4)
    bottomPadding: Math.round(theme.fontSize / 4)
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize
    // The entry of a section, as the menu makes it.
    delegate: MenuEntry {}

    TextMetrics {
        id: zero
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: "0"
    }

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
        implicitWidth: 20 * zero.advanceWidth
        color: theme.menuBackground
        radius: theme.radius
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: theme.menuBorder
    }
}
