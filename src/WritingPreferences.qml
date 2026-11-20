import QtQuick
import Parch

// The writing surface: its font, text size, line height and line length,
// whether its Markdown is styled as it is written, the syntax headings are
// written in, the toolbar, and whether a launch starts with a new draft.
// Each change applies at once in every window and is kept.
PreferenceSection {
    id: section

    readonly property string themeFont: qsTr("Theme font")
    // Every installed family, after the theme's own.
    readonly property var families: [themeFont].concat(Qt.fontFamilies())
    readonly property real controlWidth: Math.round(theme.fontSize * 16)

    title: qsTr("Writing")

    PreferenceRow {
        width: parent.width
        label: qsTr("Font")

        ThemedComboBox {
            objectName: "fontFamilyChoice"
            width: Math.min(section.controlWidth * 1.5, parent.width)
            model: section.families
            currentIndex: settings.themeFontFamily ? 0 : Math.max(0, section.families.indexOf(settings.editorFontFamily))
            displayText: settings.themeFontFamily ? section.themeFont : settings.editorFontFamily
            Accessible.name: qsTr("Font")
            onActivated: (index) => settings.setEditorFontFamily(index === 0 ? "" : section.families[index])
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Text size, in pixels")

        ThemedSpinBox {
            objectName: "textSizeChoice"
            from: settings.limits.minFontPixelSize
            to: settings.limits.maxFontPixelSize
            value: settings.fontPixelSize
            Accessible.name: qsTr("Text size, in pixels")
            onValueModified: settings.setFontPixelSize(value)
        }

        ThemedButton {
            objectName: "themeTextSize"
            framed: true
            height: Math.round(theme.fontSize * 2.4)
            text: qsTr("Theme size")
            enabled: settings.zoomed
            onClicked: settings.resetZoom()
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Line height")

        ThemedSpinBox {
            objectName: "lineHeightChoice"
            decimals: 1
            from: Math.round(settings.limits.minLineHeight * 10)
            to: Math.round(settings.limits.maxLineHeight * 10)
            value: Math.round(settings.lineHeight * 10)
            Accessible.name: qsTr("Line height")
            onValueModified: settings.setLineHeight(value / 10)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Line length, in characters")

        ThemedSpinBox {
            objectName: "lineLengthChoice"
            from: settings.limits.minLineLength
            to: settings.limits.maxLineLength
            value: settings.maxLineLength
            Accessible.name: qsTr("Line length, in characters")
            onValueModified: settings.setMaxLineLength(value)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Live formatting")

        ThemedCheck {
            objectName: "liveFormattingChoice"
            width: Math.min(implicitWidth, parent.width)
            text: qsTr("Style the Markdown as I write")
            checked: settings.liveFormatting
            Accessible.name: text
            // The box follows the setting again, whoever changes it next.
            onToggled: {
                settings.setLiveFormatting(checked)
                checked = Qt.binding(() => settings.liveFormatting)
            }
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Tables")

        ThemedCheck {
            objectName: "tableGridsChoice"
            width: Math.min(implicitWidth, parent.width)
            text: qsTr("Show tables as grids to write in")
            checked: settings.tableGrids
            Accessible.name: text
            onToggled: {
                settings.setTableGrids(checked)
                checked = Qt.binding(() => settings.tableGrids)
            }
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Headings")

        ThemedComboBox {
            objectName: "headingSyntaxChoice"
            width: Math.min(section.controlWidth * 1.5, parent.width)
            model: [qsTr("Marked with #"), qsTr("Underlined, for levels 1 and 2")]
            currentIndex: settings.setextHeadings ? 1 : 0
            Accessible.name: qsTr("Headings")
            onActivated: (index) => settings.setSetextHeadings(index === 1)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Toolbar")

        ThemedCheck {
            objectName: "toolbarChoice"
            text: qsTr("Show the formatting toolbar")
            checked: settings.showToolbar
            Accessible.name: text
            onToggled: settings.setShowToolbar(checked)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("At launch")

        ThemedCheck {
            objectName: "freshDraftChoice"
            width: Math.min(implicitWidth, parent.width)
            text: qsTr("Start with a new draft")
            checked: settings.freshDraftOnLaunch
            Accessible.name: text
            onToggled: {
                settings.setFreshDraftOnLaunch(checked)
                checked = Qt.binding(() => settings.freshDraftOnLaunch)
            }
        }
    }
}
