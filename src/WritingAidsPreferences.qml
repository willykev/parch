import QtQuick
import QtQuick.Templates as T
import Parch

// The writing aids, as a section of the Preferences dialog: whether spelling
// is checked, with what the dictionary is or why there is none, what focus
// mode keeps bright and how much it dims the rest. Each choice applies at
// once, in every window, and is kept.
PreferenceSection {
    id: section

    title: qsTr("Spelling and focus")

    PreferenceRow {
        width: parent.width
        label: qsTr("Spelling")

        Column {
            width: parent.width
            spacing: Math.round(theme.fontSize / 4)

            ThemedCheck {
                id: spellingCheck
                objectName: "spellingCheck"
                width: Math.min(implicitWidth, parent.width)
                text: qsTr("Check spelling as I write")
                checked: backend.spelling.wanted
                Accessible.name: text
                // The checkbox follows the setting again, whoever changes it next.
                onToggled: {
                    backend.setSpellchecking(checked)
                    checked = Qt.binding(() => backend.spelling.wanted)
                }
            }

            Note {
                objectName: "dictionaryState"
                text: !backend.spelling.settled ? qsTr("Reading the English dictionary…")
                      : backend.spelling.available ? qsTr("English (United States). Words you add are kept in your personal dictionary.")
                      : backend.spelling.dictionaryFound ? qsTr("The English dictionary could not be read.")
                      : qsTr("No English dictionary is installed. Install the hunspell-en_us package to check spelling.")
            }
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Focus mode")

        Column {
            width: parent.width
            spacing: Math.round(theme.fontSize / 4)

            Note { text: qsTr("Dims all but the part you are writing:") }

            Choice {
                objectName: "focusSentence"
                text: qsTr("The sentence")
                checked: settings.focusUnit === "sentence"
                onToggled: settings.setFocusUnit("sentence")
            }

            Choice {
                objectName: "focusParagraph"
                text: qsTr("The paragraph")
                checked: settings.focusUnit === "paragraph"
                onToggled: settings.setFocusUnit("paragraph")
            }
        }
    }

    // How far the rest is dimmed: a group of its own, so that its choices
    // and the ones above are chosen apart.
    PreferenceRow {
        width: parent.width
        label: qsTr("Dimming")

        Column {
            width: parent.width
            spacing: Math.round(theme.fontSize / 4)

            Note { text: qsTr("Dimmed text always stays readable against the background.") }

            Choice {
                objectName: "focusDimOff"
                text: qsTr("Off")
                checked: settings.focusDimming === "off"
                onToggled: settings.setFocusDimming("off")
            }

            Choice {
                objectName: "focusDimLight"
                text: qsTr("Light")
                checked: settings.focusDimming === "light"
                onToggled: settings.setFocusDimming("light")
            }

            Choice {
                objectName: "focusDimStrong"
                text: qsTr("Strong")
                checked: settings.focusDimming === "strong"
                onToggled: settings.setFocusDimming("strong")
            }
        }
    }

    component Note: Text {
        width: parent ? parent.width : implicitWidth
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.sizes.small
        color: theme.dim
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // A radio button as the shell draws its controls: a mark in the accent
    // when chosen, and keyboard focus drawn with the hover fill and a ring in
    // the accent, as ThemedCheck draws it.
    component Choice: T.RadioButton {
        id: choice

        // The arrows choose the next or the previous choice of the group, as
        // in any group of radio buttons.
        function choose(forward) {
            const next = choice.nextItemInFocusChain(forward)
            if (!next || next.parent !== choice.parent || next.autoExclusive === undefined)
                return
            next.forceActiveFocus(Qt.TabFocusReason)
            next.click()
        }
        Keys.onDownPressed: choose(true)
        Keys.onRightPressed: choose(true)
        Keys.onUpPressed: choose(false)
        Keys.onLeftPressed: choose(false)

        width: parent ? Math.min(implicitWidth, parent.width) : implicitWidth
        implicitWidth: implicitContentWidth + leftPadding + rightPadding
        implicitHeight: Math.max(implicitContentHeight, implicitIndicatorHeight) + topPadding + bottomPadding
        padding: Math.round(theme.fontSize / 3)
        leftPadding: indicator.width + 2 * padding
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        hoverEnabled: true

        indicator: Rectangle {
            x: choice.padding
            anchors.verticalCenter: parent.verticalCenter
            width: Math.round(theme.fontSize * 1.1)
            height: width
            radius: width / 2
            color: "transparent"
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: choice.checked ? theme.accent : theme.controlBorder

            Rectangle {
                anchors.centerIn: parent
                width: Math.round(parent.width / 2)
                height: width
                radius: width / 2
                visible: choice.checked
                color: theme.accent
            }
        }

        contentItem: Text {
            text: choice.text
            textFormat: Text.PlainText
            font: choice.font
            color: choice.enabled ? theme.foreground : theme.faint
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: theme.radius
            color: choice.down ? theme.pressedFill : choice.hovered || choice.visualFocus ? theme.hoverFill : "transparent"
            border.width: choice.visualFocus ? Math.max(1, theme.controlBorderWidth) : 0
            border.color: theme.accent
        }
    }
}
