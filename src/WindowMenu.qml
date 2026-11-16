import QtQuick
import QtQuick.Controls.Basic
import Parch

// The window's menu: its commands with the keys that do the same, so that
// every command can be found without knowing its keys. Each entry is a
// command of the registry, named and keyed as the registry has it, and
// chosen through commandChosen(), as the window's shortcuts and palette
// run it. The formatting commands are in a Format section of their own.
ThemedMenu {
    id: windowMenu

    property CommandRegistry commands: null
    // The text of each command's keys, by command id.
    property var keys: ({})
    property bool libraryShown: false
    property bool toolbarShown: false
    property bool outlineShown: false
    // How the window shows the document in front: a TabModel view mode.
    property int viewMode: TabModel.Source
    property bool focusMode: false
    property bool distractionFree: false
    property bool spellingWanted: false
    property bool liveFormatting: true
    // Whether a command can run now, as the window says.
    property var canRun: function(command) { return true }

    signal commandChosen(string command)

    CommandEntry { command: "newDraft" }
    CommandEntry { command: "newWindow" }
    // A second tab on the document in front, with a cursor and scroll of its own.
    CommandEntry { objectName: "newViewEntry"; command: "newView" }
    MenuLine {}
    CommandEntry { command: "open" }
    CommandEntry { command: "openFolder" }
    CommandEntry { command: "findDocument" }
    CommandEntry { command: "findText" }
    CommandEntry { command: "replace" }
    CommandEntry { command: "searchLibrary" }
    CommandEntry { command: "goToHeading" }
    CommandEntry { command: "save" }
    CommandEntry { command: "saveAs" }
    CommandEntry { objectName: "historyEntry"; command: "history" }
    CommandEntry { objectName: "checkLinksEntry"; command: "checkLinks" }
    CommandEntry { command: "closeTab" }
    MenuLine {}
    ThemedMenu {
        objectName: "formatMenu"
        title: qsTr("Format")

        CommandEntry { command: "bold" }
        CommandEntry { command: "italic" }
        CommandEntry { command: "strikethrough" }
        CommandEntry { command: "highlight" }
        CommandEntry { command: "code" }
        CommandEntry { command: "comment" }
        MenuLine {}
        ThemedMenu {
            objectName: "headingMenu"
            title: qsTr("Heading")

            CommandEntry { command: "heading1" }
            CommandEntry { command: "heading2" }
            CommandEntry { command: "heading3" }
            CommandEntry { command: "heading4" }
            CommandEntry { command: "heading5" }
            CommandEntry { command: "heading6" }
            CommandEntry { command: "paragraph" }
        }
        CommandEntry { command: "quote" }
        CommandEntry { command: "bulletList" }
        CommandEntry { command: "numberedList" }
        CommandEntry { command: "taskList" }
        CommandEntry { command: "toggleTask" }
        CommandEntry { command: "codeBlock" }
        CommandEntry { command: "formatTable" }
        MenuLine {}
        CommandEntry { command: "link" }
        CommandEntry { command: "image" }
        CommandEntry { command: "footnote" }
        CommandEntry { command: "table" }
        CommandEntry { command: "inlineMath" }
        CommandEntry { command: "displayMath" }
        CommandEntry { command: "thematicBreak" }
        CommandEntry { command: "pageBreak" }
    }
    CommandEntry {
        objectName: "spellingEntry"
        command: "checkSpelling"
        text: windowMenu.spellingWanted ? qsTr("Stop checking spelling") : qsTr("Check spelling")
    }
    MenuLine {}
    // The three views of the document in front, the one shown marked, and
    // what goes with its preview.
    ThemedMenu {
        objectName: "viewMenu"
        title: qsTr("View")

        CommandEntry { objectName: "viewSourceEntry"; command: "viewSource"; marked: windowMenu.viewMode === TabModel.Source }
        CommandEntry { objectName: "viewSplitEntry"; command: "viewSplit"; marked: windowMenu.viewMode === TabModel.Split }
        CommandEntry { objectName: "viewReadingEntry"; command: "viewReading"; marked: windowMenu.viewMode === TabModel.Reading }
        MenuLine {}
        CommandEntry {
            objectName: "togglePreviewEntry"
            command: "togglePreview"
            text: windowMenu.viewMode === TabModel.Source ? qsTr("Show preview") : qsTr("Hide preview")
        }
        CommandEntry { command: "otherPane" }
        CommandEntry {
            objectName: "scrollSyncEntry"
            command: "scrollSync"
            marked: settings.scrollSync
        }
        CommandEntry { command: "previewBack" }
    }
    MenuLine {}
    CommandEntry {
        command: "library"
        text: windowMenu.libraryShown ? qsTr("Hide library") : qsTr("Show library")
    }
    CommandEntry {
        objectName: "outlineEntry"
        command: "outline"
        text: windowMenu.outlineShown ? qsTr("Hide outline") : qsTr("Show outline")
    }
    CommandEntry {
        objectName: "toolbarEntry"
        command: "toolbar"
        text: windowMenu.toolbarShown ? qsTr("Hide toolbar") : qsTr("Show toolbar")
    }
    CommandEntry {
        objectName: "liveFormattingEntry"
        command: "liveFormatting"
        text: windowMenu.liveFormatting ? qsTr("Turn off live formatting") : qsTr("Turn on live formatting")
    }
    CommandEntry {
        objectName: "focusModeEntry"
        command: "focusMode"
        text: windowMenu.focusMode ? qsTr("Leave focus mode") : qsTr("Focus mode")
    }
    CommandEntry {
        objectName: "distractionFreeEntry"
        command: "distractionFree"
        text: windowMenu.distractionFree ? qsTr("Leave distraction-free writing") : qsTr("Distraction-free writing")
    }
    CommandEntry { command: "zoomIn" }
    CommandEntry { command: "zoomOut" }
    CommandEntry { command: "resetZoom" }
    MenuLine {}
    CommandEntry { command: "palette" }
    CommandEntry { objectName: "preferencesEntry"; command: "preferences" }
    CommandEntry { objectName: "documentSettingsEntry"; command: "documentSettings" }
    CommandEntry { command: "shortcuts" }
    CommandEntry {
        objectName: "vimKeysEntry"
        command: "vimKeys"
        text: settings.vimKeys ? qsTr("Turn off Vim keys") : qsTr("Turn on Vim keys")
    }
    MenuLine {}
    CommandEntry { command: "quit" }

    // A command's entry: its title, or a text of its own, and its keys.
    component CommandEntry: MenuEntry {
        property string command

        text: windowMenu.commands ? windowMenu.commands.title(command) : ""
        keys: windowMenu.keys[command] || ""
        enabled: windowMenu.canRun(command)
        onTriggered: windowMenu.commandChosen(command)
    }
}
