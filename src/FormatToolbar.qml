import QtQuick
import QtQuick.Controls.Basic
import Parch
import "icons.js" as Icons

// A quiet row of formatting buttons under the tab strip, each a command of
// the registry, named for assistive technology and in its tooltip by the
// command's title and keys. A click leaves the keys in the text; Tab
// reaches every button from the window's controls. In a window too narrow
// for one row the buttons wrap onto another rather than scroll.
Rectangle {
    id: bar

    property CommandRegistry commands: null
    // The text of each command's keys, by command id.
    property var keys: ({})
    // Whether a command can run now, as the window says.
    property var canRun: function(command) { return true }

    signal commandChosen(string command)

    readonly property real inset: Math.round(theme.fontSize / 4)
    readonly property real buttonSize: Math.round(theme.fontSize * 2.2)
    readonly property real gap: Math.round(theme.fontSize / 6)
    readonly property real groupGap: Math.round(theme.fontSize * 2 / 3)
    // The width the buttons take on one line, the room the toolbar needs.
    readonly property real naturalWidth: {
        let width = 2 * inset
        for (let i = 0; i < buttons.children.length; ++i)
            width += buttons.children[i].width + (i > 0 ? buttons.spacing : 0)
        return width
    }

    implicitHeight: buttons.implicitHeight + 2 * inset + theme.controlBorderWidth
    color: theme.background

    Flow {
        id: buttons
        objectName: "toolbarButtons"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: bar.inset
        spacing: bar.gap

        Tool { command: "bold"; iconPath: Icons.bold }
        Tool { command: "italic"; iconPath: Icons.italic }
        Tool { command: "strikethrough"; iconPath: Icons.strikethrough }
        Tool { command: "highlight"; iconPath: Icons.highlight }
        Tool { command: "code"; iconPath: Icons.code }
        Spacer {}
        // Headings have a menu of their own: their levels and plain paragraph.
        ThemedButton {
            id: headingButton
            objectName: "toolbar-heading"
            width: bar.buttonSize
            height: bar.buttonSize
            iconPath: Icons.heading
            text: qsTr("Heading")
            enabled: bar.canRun("heading1")
            focusPolicy: Qt.TabFocus
            onClicked: headings.popup(headingButton, 0, headingButton.height)
        }
        Tool { command: "quote"; iconPath: Icons.quote }
        Tool { command: "bulletList"; iconPath: Icons.bulletList }
        Tool { command: "numberedList"; iconPath: Icons.numberedList }
        Tool { command: "taskList"; iconPath: Icons.taskList }
        Spacer {}
        Tool { command: "link"; iconPath: Icons.link }
        Tool { command: "image"; iconPath: Icons.image }
        Tool { command: "codeBlock"; iconPath: Icons.codeBlock }
        Tool { command: "table"; iconPath: Icons.table }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: theme.controlBorderWidth
        color: theme.controlBorder
    }

    ThemedMenu {
        id: headings
        objectName: "toolbarHeadingMenu"

        Heading { command: "heading1" }
        Heading { command: "heading2" }
        Heading { command: "heading3" }
        Heading { command: "heading4" }
        Heading { command: "heading5" }
        Heading { command: "heading6" }
        Heading { command: "paragraph" }
    }

    // A button for one command.
    component Tool: ThemedButton {
        property string command

        objectName: "toolbar-" + command
        width: bar.buttonSize
        height: bar.buttonSize
        text: bar.commands ? bar.commands.title(command) : ""
        keys: bar.keys[command] || ""
        enabled: bar.canRun(command)
        // A click leaves the keys where they were, in the text.
        focusPolicy: Qt.TabFocus
        onClicked: bar.commandChosen(command)
    }

    // The room between groups of buttons.
    component Spacer: Item {
        width: bar.groupGap - bar.gap
        height: bar.buttonSize
    }

    component Heading: MenuEntry {
        property string command

        text: bar.commands ? bar.commands.title(command) : ""
        keys: bar.keys[command] || ""
        onTriggered: bar.commandChosen(command)
    }
}
