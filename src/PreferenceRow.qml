import QtQuick

// A setting: its name on the left and its control on the right, or the name
// above the control where the row is too narrow for both side by side. The
// control is the row's child, and names itself for assistive technology.
Item {
    id: row

    property string label
    default property alias controls: holder.data

    readonly property real characterWidth: zero.advanceWidth
    readonly property bool stacked: width < 44 * characterWidth
    readonly property real labelWidth: stacked ? width : Math.round(width * 0.4)

    implicitHeight: stacked ? name.implicitHeight + holder.implicitHeight + Math.round(theme.fontSize / 4)
                            : Math.max(name.implicitHeight, holder.implicitHeight)

    TextMetrics {
        id: zero
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        text: "0"
    }

    Text {
        id: name
        width: row.labelWidth - (row.stacked ? 0 : Math.round(theme.fontSize))
        height: row.stacked ? implicitHeight : row.height
        text: row.label
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        color: theme.foreground
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        // The control carries the name.
        Accessible.ignored: true
    }

    Row {
        id: holder
        x: row.stacked ? 0 : row.labelWidth
        y: row.stacked ? name.height + Math.round(theme.fontSize / 4) : 0
        width: row.width - x
        spacing: Math.round(theme.fontSize / 2)
    }
}
