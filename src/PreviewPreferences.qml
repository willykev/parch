import QtQuick
import Parch

// The preview: its reading font, text size, line height and line length,
// whether it scrolls with the text beside it, and whether it fetches the
// pictures a document names by web address. Each change applies at once in
// every window and is kept.
PreferenceSection {
    id: section

    readonly property string defaultFont: qsTr("Default reading font")
    // Every installed family, after the default.
    readonly property var families: [defaultFont].concat(Qt.fontFamilies())
    readonly property bool defaultChosen: settings.previewFontFamily.length === 0
    readonly property real controlWidth: Math.round(theme.fontSize * 16)

    title: qsTr("Preview")

    PreferenceRow {
        width: parent.width
        label: qsTr("Font")

        ThemedComboBox {
            objectName: "previewFontChoice"
            width: Math.min(section.controlWidth * 1.5, parent.width)
            model: section.families
            currentIndex: section.defaultChosen ? 0 : Math.max(0, section.families.indexOf(settings.previewFontFamily))
            displayText: section.defaultChosen ? section.defaultFont : settings.previewFontFamily
            Accessible.name: qsTr("Preview font")
            onActivated: (index) => settings.setPreviewFontFamily(index === 0 ? "" : section.families[index])
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Text size, in pixels")

        ThemedSpinBox {
            objectName: "previewTextSizeChoice"
            from: settings.limits.minFontPixelSize
            to: settings.limits.maxFontPixelSize
            value: settings.previewFontPixelSize
            Accessible.name: qsTr("Preview text size, in pixels")
            onValueModified: settings.setPreviewFontPixelSize(value)
        }

        // The size is the text's and two pixels until one is chosen.
        ThemedButton {
            objectName: "previewFollowsTextSize"
            framed: true
            height: Math.round(theme.fontSize * 2.4)
            text: qsTr("Follow the text")
            enabled: settings.previewSizeChosen
            onClicked: settings.resetPreviewFontPixelSize()
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Line height")

        ThemedSpinBox {
            objectName: "previewLineHeightChoice"
            decimals: 1
            from: Math.round(settings.limits.minLineHeight * 10)
            to: Math.round(settings.limits.maxLineHeight * 10)
            value: Math.round(settings.previewLineHeight * 10)
            Accessible.name: qsTr("Preview line height")
            onValueModified: settings.setPreviewLineHeight(value / 10)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Line length, in characters")

        ThemedSpinBox {
            objectName: "previewLineLengthChoice"
            from: settings.limits.minLineLength
            to: settings.limits.maxLineLength
            value: settings.previewLineLength
            Accessible.name: qsTr("Preview line length, in characters")
            onValueModified: settings.setPreviewLineLength(value)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Scrolling")

        ThemedCheck {
            objectName: "scrollSyncChoice"
            text: qsTr("Scroll the text and the preview together")
            checked: settings.scrollSync
            Accessible.name: text
            onToggled: settings.setScrollSync(checked)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Pictures")

        ThemedCheck {
            objectName: "remoteImagesChoice"
            text: qsTr("Fetch pictures from the web")
            checked: settings.remoteImages
            Accessible.name: text
            onToggled: settings.setRemoteImages(checked)
        }
    }
}
