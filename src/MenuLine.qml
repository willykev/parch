import QtQuick
import QtQuick.Controls.Basic

// The line between groups of a ThemedMenu.
MenuSeparator {
    topPadding: Math.round(theme.fontSize / 4)
    bottomPadding: Math.round(theme.fontSize / 4)

    contentItem: Rectangle {
        implicitHeight: Math.max(1, theme.controlBorderWidth)
        color: theme.controlBorder
    }
}
