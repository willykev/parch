import QtQuick
import QtQuick.Controls.Basic
import Parch

// Chooses the size of a new table: by pointing at a grid of cells, with the
// arrow keys, or in the two number fields, the header counted among the
// rows. Enter or the Insert button makes the table, Escape leaves it.
Popup {
    id: picker

    property int columns: 3
    property int rows: 3
    // How many cells the pointing grid offers each way.
    readonly property int reach: 8

    signal chosen(int columns, int rows)

    function choose() {
        picker.close()
        picker.chosen(picker.columns, picker.rows)
    }

    padding: Math.round(theme.fontSize * 0.8)
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onAboutToShow: {
        picker.columns = 3
        picker.rows = 3
        cells.forceActiveFocus()
    }

    background: Rectangle {
        color: theme.menuBackground
        radius: theme.radius
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: theme.menuBorder
    }

    contentItem: Column {
        spacing: Math.round(theme.fontSize * 0.6)

        Text {
            objectName: "tableSizeTitle"
            text: qsTr("Insert table")
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            font.bold: true
            color: theme.bright
        }
        Text {
            objectName: "tableSizeLabel"
            text: qsTr("%1 columns × %2 rows").arg(picker.columns).arg(picker.rows)
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.foreground
        }

        // The cells to point at; the arrows change the size from here too.
        Grid {
            id: cells
            objectName: "tableSizeCells"
            columns: picker.reach
            spacing: Math.max(2, Math.round(theme.fontSize / 6))
            focus: true
            activeFocusOnTab: true
            Accessible.role: Accessible.Table
            Accessible.name: qsTr("Table size, %1 columns by %2 rows").arg(picker.columns).arg(picker.rows)
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Right) picker.columns = Math.min(64, picker.columns + 1)
                else if (event.key === Qt.Key_Left) picker.columns = Math.max(1, picker.columns - 1)
                else if (event.key === Qt.Key_Down) picker.rows = Math.min(1000, picker.rows + 1)
                else if (event.key === Qt.Key_Up) picker.rows = Math.max(1, picker.rows - 1)
                else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) picker.choose()
                else return
                event.accepted = true
            }

            Repeater {
                model: picker.reach * picker.reach

                delegate: Rectangle {
                    required property int index
                    readonly property int column: index % picker.reach + 1
                    readonly property int row: Math.floor(index / picker.reach) + 1
                    readonly property bool inside: column <= picker.columns && row <= picker.rows

                    width: Math.round(theme.fontSize * 1.3)
                    height: width
                    radius: Math.max(1, Math.round(theme.radius / 3))
                    color: inside ? theme.selection : theme.controlFill
                    border.width: Math.max(1, theme.controlBorderWidth)
                    border.color: inside ? theme.accent : theme.controlBorder

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        onEntered: {
                            picker.columns = parent.column
                            picker.rows = parent.row
                        }
                        onClicked: picker.choose()
                    }
                }
            }
        }

        Row {
            spacing: Math.round(theme.fontSize * 0.6)

            Column {
                Text {
                    text: qsTr("Columns")
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: theme.dim
                }
                ThemedSpinBox {
                    objectName: "tableColumnsField"
                    from: 1
                    to: 64
                    value: picker.columns
                    Accessible.name: qsTr("Columns")
                    onValueModified: picker.columns = value
                    Keys.onReturnPressed: picker.choose()
                    Keys.onEnterPressed: picker.choose()
                }
            }
            Column {
                Text {
                    text: qsTr("Rows, with the header")
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: theme.dim
                }
                ThemedSpinBox {
                    objectName: "tableRowsField"
                    from: 1
                    to: 1000
                    value: picker.rows
                    Accessible.name: qsTr("Rows, with the header")
                    onValueModified: picker.rows = value
                    Keys.onReturnPressed: picker.choose()
                    Keys.onEnterPressed: picker.choose()
                }
            }
        }

        Row {
            spacing: Math.round(theme.fontSize * 0.6)
            anchors.right: parent.right

            ThemedButton {
                objectName: "tableSizeCancel"
                text: qsTr("Cancel")
                framed: true
                onClicked: picker.close()
            }
            ThemedButton {
                objectName: "tableSizeInsert"
                text: qsTr("Insert table")
                framed: true
                onClicked: picker.choose()
            }
        }
    }
}
