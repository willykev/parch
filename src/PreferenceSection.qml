import QtQuick

// One section of the Preferences dialog: a heading and its rows, each a
// PreferenceRow. A module adds its settings as a section of its own.
Column {
    id: section

    property string title
    default property alias rows: body.data

    spacing: Math.round(theme.fontSize / 2)

    Text {
        width: parent.width
        text: section.title
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.sizes.subtitle
        font.weight: Font.DemiBold
        color: theme.bright
        elide: Text.ElideRight
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }

    Rectangle {
        width: parent.width
        height: theme.controlBorderWidth
        color: theme.controlBorder
    }

    Column {
        id: body
        width: parent.width
        spacing: Math.round(theme.fontSize / 2)
    }
}
