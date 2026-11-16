import QtQuick
import Parch

// Keys: whether the editor takes Vim keys, and how to leave them. The change
// applies at once in every window and is kept.
PreferenceSection {
    id: section

    title: qsTr("Keys")

    PreferenceRow {
        width: parent.width
        label: qsTr("Vim keys")

        ThemedCheck {
            objectName: "vimKeysChoice"
            text: qsTr("Edit with Vim keys")
            checked: settings.vimKeys
            Accessible.name: text
            onToggled: settings.setVimKeys(checked)
        }
    }

    Text {
        objectName: "vimKeysHelp"
        width: parent.width
        text: qsTr("Esc always returns to Normal mode, and i starts typing. The command palette and the menu "
                   + "work in every mode: Turn off Vim keys in either, or this box, ends them.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        color: theme.dim
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
}
