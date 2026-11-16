import QtQuick
import QtQuick.Controls.Basic
import Parch

// Preferences, in sections that scroll: each section is a PreferenceSection
// in the column below, and a module adds its own there. Every control
// changes its setting at once, for every window, and keeps it; Tab moves
// through the controls, scrolling each into view, and Escape or Close
// closes the dialog.
ThemedDialog {
    id: dialog

    title: qsTr("Preferences")
    preferredCharacters: 64
    preferredHeight: Math.round(theme.fontSize * 36)
    initialFocus: closeButton

    // Keeps the control with the keys in view.
    function reveal(item) {
        if (!item || !visible)
            return
        for (let at = item; at; at = at.parent) {
            if (at === sections) {
                const box = item.mapToItem(sections, 0, 0, item.width, item.height)
                if (box.y < flick.contentY)
                    flick.contentY = box.y
                else if (box.y + box.height > flick.contentY + flick.height)
                    flick.contentY = Math.min(box.y + box.height - flick.height, flick.contentHeight - flick.height)
                return
            }
        }
    }

    contentItem: Flickable {
        id: flick
        objectName: "preferencesSections"
        clip: true
        contentHeight: sections.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        implicitHeight: sections.implicitHeight

        Connections {
            target: dialog.visible ? flick.Window.window : null
            function onActiveFocusItemChanged() { dialog.reveal(flick.Window.window.activeFocusItem) }
        }

        Column {
            id: sections
            width: flick.width - (scroller.visible ? scroller.width : 0)
            spacing: Math.round(theme.fontSize * 1.5)

            WritingPreferences {
                width: parent.width
            }

            WritingAidsPreferences {
                width: parent.width
            }

            KeysPreferences {
                width: parent.width
            }

            MarkdownPreferences {
                width: parent.width
            }
        }

        ScrollBar.vertical: ThemedDialog.Scroller {
            id: scroller
            Accessible.name: qsTr("Scroll the preferences")
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: closeButton
            objectName: "closePreferences"
            text: qsTr("Close")
            onClicked: dialog.close()
        }
    }
}
