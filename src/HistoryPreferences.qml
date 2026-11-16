import QtQuick
import Parch

// How much history keeps, as a section of the Preferences dialog: how many
// versions of each document, for how many days, and how much space all of
// them take. Each change applies in every window, and is kept. A higher
// limit applies at once. A lower one removes versions, which cannot be
// brought back, so it waits until the limits have stopped changing for
// settleMs; the versions it would remove are then counted, and a limit that
// removes some applies only once the writer agrees, with Keep them putting
// the limits back. A lower limit not agreed to as Preferences close is
// dropped. A version the writer named is never removed.
PreferenceSection {
    id: section
    objectName: "historyPreferences"

    title: qsTr("History")

    // How long the limits stay as they are before a lower one is counted.
    readonly property int settleMs: 1000
    // The limits the boxes show. Each follows the settings unless the
    // writer has lowered it and not yet agreed to what that removes.
    property int versions: settings.historyVersions
    property int days: settings.historyDays
    property int tenths: Math.round(settings.historyGigabytes * 10)
    readonly property int lowered: (versions < settings.historyVersions ? 1 : 0)
                                   + (days < settings.historyDays ? 1 : 0)
                                   + (tenths < Math.round(settings.historyGigabytes * 10) ? 1 : 0)
    // How many versions the lower limits remove, once counted; -1 until then.
    property int removals: -1

    // The writer chose a value: a higher or equal one is the setting at
    // once, a lower one waits.
    function choose(name, value) {
        section.removals = -1
        switch (name) {
        case "versions":
            if (value >= settings.historyVersions) {
                settings.setHistoryVersions(value)
                section.versions = Qt.binding(() => settings.historyVersions)
            } else {
                section.versions = value
            }
            break
        case "days":
            if (value >= settings.historyDays) {
                settings.setHistoryDays(value)
                section.days = Qt.binding(() => settings.historyDays)
            } else {
                section.days = value
            }
            break
        case "space":
            if (value >= Math.round(settings.historyGigabytes * 10)) {
                settings.setHistoryGigabytes(value / 10)
                section.tenths = Qt.binding(() => Math.round(settings.historyGigabytes * 10))
            } else {
                section.tenths = value
            }
            break
        }
        if (section.lowered > 0)
            settle.restart()
        else
            settle.stop()
    }

    // The lower limits become the settings, which prunes history to them.
    function lower() {
        settle.stop()
        section.removals = -1
        if (section.versions < settings.historyVersions)
            settings.setHistoryVersions(section.versions)
        if (section.days < settings.historyDays)
            settings.setHistoryDays(section.days)
        if (section.tenths < Math.round(settings.historyGigabytes * 10))
            settings.setHistoryGigabytes(section.tenths / 10)
        section.follow()
    }

    // The boxes show the settings again, and nothing is removed.
    function keep() {
        settle.stop()
        section.removals = -1
        section.follow()
    }

    function follow() {
        section.versions = Qt.binding(() => settings.historyVersions)
        section.days = Qt.binding(() => settings.historyDays)
        section.tenths = Qt.binding(() => Math.round(settings.historyGigabytes * 10))
    }

    // Closing Preferences, or the section going out of sight, keeps the versions.
    onVisibleChanged: if (!visible && section.lowered > 0) section.keep()

    Timer {
        id: settle
        interval: section.settleMs
        onTriggered: {
            if (section.lowered > 0)
                backend.countHistoryRemovals(section.versions, section.days, section.tenths / 10)
        }
    }

    Connections {
        target: backend
        function onHistoryRemovalsCounted(versions, days, gigabytes, count) {
            // An answer about limits the boxes no longer show is old.
            if (section.lowered === 0 || settle.running || versions !== section.versions || days !== section.days
                    || Math.round(gigabytes * 10) !== section.tenths)
                return
            if (count === 0)
                section.lower()
            else
                section.removals = count
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Versions kept of each document")

        ThemedSpinBox {
            objectName: "historyVersionsChoice"
            from: settings.limits.minHistoryVersions
            to: settings.limits.maxHistoryVersions
            value: section.versions
            Accessible.name: qsTr("Versions kept of each document")
            onValueModified: section.choose("versions", value)
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Days versions are kept")

        ThemedSpinBox {
            objectName: "historyDaysChoice"
            from: settings.limits.minHistoryDays
            to: settings.limits.maxHistoryDays
            value: section.days
            Accessible.name: qsTr("Days versions are kept")
            onValueModified: section.choose("days", value)
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
            value: section.tenths
            Accessible.name: qsTr("Space for all history, in gigabytes")
            onValueModified: section.choose("space", value)
        }
    }

    // What the lower limits remove, before they remove it.
    Column {
        id: removalsRow
        objectName: "historyRemovals"
        width: parent.width
        visible: section.lowered > 0 && section.removals > 0
        spacing: Math.round(theme.fontSize / 4)

        Text {
            objectName: "historyRemovalsText"
            width: parent.width
            text: section.removals === 1
                  ? (section.lowered > 1 ? qsTr("The lower limits remove 1 older version for good.")
                                         : qsTr("The lower limit removes 1 older version for good."))
                  : (section.lowered > 1 ? qsTr("The lower limits remove %1 older versions for good.").arg(section.removals)
                                         : qsTr("The lower limit removes %1 older versions for good.").arg(section.removals))
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.foreground
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        Row {
            spacing: Math.round(theme.fontSize / 2)

            ThemedButton {
                objectName: "historyRemovalsApply"
                framed: true
                text: qsTr("Remove them")
                textColor: theme.danger
                onClicked: section.lower()
            }

            ThemedButton {
                objectName: "historyRemovalsKeep"
                framed: true
                text: qsTr("Keep them")
                onClicked: section.keep()
            }
        }
    }

    Text {
        width: parent.width
        text: qsTr("Versions you named are never removed. A lower limit applies once you stop changing it; when it would remove older versions, it says how many and asks first.")
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.sizes.small
        color: theme.dim
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }
}
