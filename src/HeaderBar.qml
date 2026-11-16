import QtQuick
import Parch
import "icons.js" as Icons

// The bar along the top of the window: the library button and parch's name
// over the library, where the document lives and its name, and the button
// for the window's menu. It is part of the window's content, not a title
// bar: the compositor draws whatever frame the window has.
Rectangle {
    id: bar

    property Document document
    // The name shown, the document's own unless the window names the tab otherwise.
    property string title: document ? document.title : ""
    // Where the document lives, as Backend.folderLabel() names it.
    property string place
    // The width of the docked library beneath, which the part with the name
    // spans; 0 while the library is not docked and shown.
    property real libraryWidth: 0
    property bool libraryShown: false
    // The document is shown with its preview, beside its text or alone.
    property bool previewShown: false
    // The text of the window's shortcuts, by action.
    property var keys: ({})

    signal toggleLibrary()
    signal togglePreview()
    // The menu button was pressed; the window opens its menu below it.
    signal menuRequested()

    readonly property real inset: Math.round(theme.fontSize / 4)
    readonly property real buttonSize: height - theme.controlBorderWidth - 2 * inset
    readonly property real gap: Math.round(theme.fontSize * 2 / 3)

    color: theme.panel

    // The advance of "0" in the interface font, the unit of the room below.
    TextMetrics {
        id: zero
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: "0"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: theme.controlBorderWidth
        color: theme.controlBorder
    }

    // Over the library: its button and the name.
    Item {
        id: lead
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: theme.controlBorderWidth
        width: Math.max(bar.libraryWidth, menuButton.width + appName.implicitWidth + bar.gap + 2 * bar.inset)

        ThemedButton {
            id: menuButton
            objectName: "menuButton"
            x: bar.inset
            anchors.verticalCenter: parent.verticalCenter
            width: bar.buttonSize
            height: bar.buttonSize
            iconPath: Icons.menu
            text: bar.libraryShown ? qsTr("Hide library") : qsTr("Show library")
            keys: bar.keys.library || ""
            onClicked: bar.toggleLibrary()
        }

        Text {
            id: appName
            anchors.left: menuButton.right
            anchors.leftMargin: bar.gap - bar.inset
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("parch")
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.title
            font.weight: Font.DemiBold
            color: theme.bright
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    // The line that continues the docked library's edge.
    Rectangle {
        id: divider
        visible: bar.libraryWidth > 0
        x: lead.width
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: visible ? theme.controlBorderWidth : 0
        color: theme.controlBorder
    }

    // Where the document lives and what it is called: its folders, joined by
    // " / ", then the name. The name keeps the room it needs first; the place
    // is cut short from its start, so the innermost folders stay, and gives
    // way altogether to a long name.
    Item {
        id: where
        anchors.left: divider.right
        anchors.leftMargin: bar.gap
        anchors.right: previewButton.left
        anchors.rightMargin: bar.gap
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.bottomMargin: theme.controlBorderWidth

        readonly property real placeRoom: width - slash.implicitWidth - title.implicitWidth
        readonly property bool placeShown: bar.place.length > 0
                                           && placeRoom >= Math.min(place.implicitWidth, 6 * zero.advanceWidth)

        Text {
            id: place
            objectName: "place"
            visible: where.placeShown
            width: visible ? Math.min(implicitWidth, where.placeRoom) : 0
            height: parent.height
            text: bar.place
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.dim
            elide: Text.ElideLeft
            // Cut short, the text is narrower than its room: it keeps to the
            // slash, with no gap before it.
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            Accessible.role: Accessible.StaticText
            Accessible.name: bar.place
        }

        Text {
            id: slash
            visible: where.placeShown
            x: place.width
            height: parent.height
            text: " / "
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.accent
            verticalAlignment: Text.AlignVCenter
            Accessible.ignored: true
        }

        Text {
            id: title
            objectName: "documentTitle"
            x: where.placeShown ? place.width + slash.implicitWidth : 0
            width: Math.max(0, Math.min(implicitWidth, where.width - x))
            height: parent.height
            text: bar.title
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.bright
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    // The preview beside the text, or the text alone again.
    ThemedButton {
        id: previewButton
        objectName: "previewButton"
        anchors.right: moreButton.left
        anchors.rightMargin: bar.inset
        anchors.verticalCenter: moreButton.verticalCenter
        width: bar.buttonSize
        height: bar.buttonSize
        enabled: bar.document !== null
        iconPath: Icons.split
        text: bar.previewShown ? qsTr("Hide preview") : qsTr("Show preview")
        keys: bar.keys.togglePreview || ""
        textColor: bar.previewShown ? theme.accent : theme.foreground
        onClicked: bar.togglePreview()
    }

    ThemedButton {
        id: moreButton
        objectName: "moreButton"
        anchors.right: parent.right
        anchors.rightMargin: bar.inset
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: -theme.controlBorderWidth / 2
        width: bar.buttonSize
        height: bar.buttonSize
        iconPath: Icons.more
        text: qsTr("Menu")
        keys: bar.keys.menu || ""
        onClicked: bar.menuRequested()
    }
}
