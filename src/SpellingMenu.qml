import QtQuick
import QtQuick.Controls.Basic

// The menu on a misspelled word, from a right click or the Menu key: up to
// five spellings to put in its place, each one step to undo, then adding the
// word to the personal dictionary and ignoring it until parch ends. It is
// drawn as the window's menu is, and opens with its first entry highlighted,
// so that the arrows and Enter go on from there.
ThemedMenu {
    id: spellingMenu

    // The view's tables, whose commands follow the spellings in a cell.
    property var tables: null
    // The word, and where it is in the source.
    property int start: 0
    property int end: 0
    property string word

    signal replace(int start, int end, string word, string replacement)
    signal addWord(string word)
    signal ignoreWord(string word)

    // Fills the menu for a word and its suggestions.
    function offer(start, end, word, suggestions, inTable) {
        spellingMenu.start = start
        spellingMenu.end = end
        spellingMenu.word = word
        // A submenu goes with its entry, as removing the entry alone would leave it.
        while (spellingMenu.count > 0) {
            const item = spellingMenu.itemAt(0)
            if (item.subMenu)
                spellingMenu.removeMenu(item.subMenu)
            else
                spellingMenu.removeItem(item)
        }
        for (const suggestion of suggestions)
            spellingMenu.addItem(entry.createObject(spellingMenu.contentItem, { text: suggestion, replacement: suggestion }))
        if (suggestions.length === 0)
            spellingMenu.addItem(entry.createObject(spellingMenu.contentItem, { text: qsTr("No suggestions"), enabled: false }))
        spellingMenu.addItem(line.createObject(spellingMenu.contentItem))
        spellingMenu.addItem(entry.createObject(spellingMenu.contentItem, { text: qsTr("Add to dictionary"), kind: "add", objectName: "addToDictionary" }))
        spellingMenu.addItem(entry.createObject(spellingMenu.contentItem, { text: qsTr("Ignore"), kind: "ignore", objectName: "ignoreWord" }))
        if (inTable && spellingMenu.tables) {
            spellingMenu.addItem(line.createObject(spellingMenu.contentItem))
            spellingMenu.addMenu(table.createObject(spellingMenu, { tables: spellingMenu.tables }))
        }
    }

    // What assistive technology hears the menu named.
    title: qsTr("Spelling of “%1”").arg(spellingMenu.word)
    onOpened: spellingMenu.currentIndex = 0

    Component {
        id: entry

        MenuEntry {
            id: item

            // A spelling to put in the word's place, or what else the entry does.
            property string replacement
            property string kind

            objectName: "spellingSuggestion"
            onTriggered: {
                if (item.kind === "add")
                    spellingMenu.addWord(spellingMenu.word)
                else if (item.kind === "ignore")
                    spellingMenu.ignoreWord(spellingMenu.word)
                else if (item.replacement.length > 0)
                    spellingMenu.replace(spellingMenu.start, spellingMenu.end, spellingMenu.word, item.replacement)
            }
        }
    }

    Component {
        id: line

        MenuLine {}
    }

    // The table's commands, for a word in a cell.
    Component {
        id: table

        TableMenu {
            objectName: "spellingTableMenu"
            kind: "cell"
        }
    }
}
