import QtQuick
import Parch

// How much history keeps, as a section of the Preferences dialog: how many
// versions of each document, for how many days, and how much space all of
// them take. Each change applies at once, in every window, and is kept; a
// lower limit removes the versions past it at once, but never one the
// writer named.
PreferenceSection {
    id: section

    title: qsTr("History")

    PreferenceRow {
        width: parent.width
        label: qsTr("Versions kept of each document")

        ThemedSpinBox {
            objectName: "historyVersionsChoice"
            from: settings.limits.minHistoryVersions
            to: settings.limits.maxHistoryVersions
            value: settings.historyVersions
            Accessible.name: qsTr("Versions kept of each document")
            onValueModified: settings.setHistoryVersions(value)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Days versions are kept")

        ThemedSpinBox {
            objectName: "historyDaysChoice"
            from: settings.limits.minHistoryDays
            to: settings.limits.maxHistoryDays
            value: settings.historyDays
            Accessible.name: qsTr("Days versions are kept")
            onValueModified: settings.setHistoryDays(value)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Space for all history")

        ThemedSpinBox {
            objectName: "historySpaceChoice"
            decimals: 1
            unit: qsTr("GB")
            from: Math.round(settings.limits.minHistoryGigabytes * 10)
            to: Math.round(settings.limits.maxHistoryGigabytes * 10)
            value: Math.round(settings.historyGigabytes * 10)
            Accessible.name: qsTr("Space for all history, in gigabytes")
            onValueModified: settings.setHistoryGigabytes(value / 10)
        }
    }

    Text {
        width: parent.width
        text: qsTr("Versions you named are never removed. A lower limit removes the older versions past it at once.")
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.sizes.small
        color: theme.dim
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
}
