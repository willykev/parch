import QtQuick
import QtQuick.Controls.Basic
import Parch

// The menu of a table shown as a grid, from a row's or a column's handle,
// a right click in a cell or the Menu key there: what the rows, the columns
// or the cell under the caret can do. Each entry runs one of the view's
// table commands, as Format → Table and the palette run them, and is there
// only when it can act now.
ThemedMenu {
    id: tableMenu

    property TableGrids tables: null
    // "row", "column" or "cell": which entries the menu has.
    property string kind: "cell"

    // A command's entry, asked again whether it can run each time the menu opens.
    component TableEntry: MenuEntry {
        required property string command
        enabled: tableMenu.opens >= 0 && tableMenu.tables !== null && tableMenu.tables.can(command)
        onTriggered: tableMenu.tables.runCommand(command)
    }

    property int opens: 0
    onAboutToShow: opens += 1
    onOpened: tableMenu.currentIndex = 0

    title: kind === "row" ? qsTr("Row") : kind === "column" ? qsTr("Column") : qsTr("Table")

    // The rows' entries.
    TableEntry { objectName: "insertRowAboveEntry"; command: "insertTableRowAbove"; text: qsTr("Insert row above"); visible: tableMenu.kind !== "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "insertRowBelowEntry"; command: "insertTableRowBelow"; text: qsTr("Insert row below"); visible: tableMenu.kind !== "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "duplicateRowEntry"; command: "duplicateTableRow"; text: qsTr("Duplicate row"); visible: tableMenu.kind !== "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "moveRowUpEntry"; command: "moveTableRowUp"; text: qsTr("Move up"); visible: tableMenu.kind === "row"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "moveRowDownEntry"; command: "moveTableRowDown"; text: qsTr("Move down"); visible: tableMenu.kind === "row"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "deleteRowEntry"; command: "deleteTableRow"; text: qsTr("Delete row"); visible: tableMenu.kind !== "column"; height: visible ? implicitHeight : 0 }
    // The columns' entries.
    TableEntry { objectName: "insertColumnLeftEntry"; command: "insertTableColumnLeft"; text: qsTr("Insert column left"); visible: tableMenu.kind !== "row"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "insertColumnRightEntry"; command: "insertTableColumnRight"; text: qsTr("Insert column right"); visible: tableMenu.kind !== "row"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "moveColumnLeftEntry"; command: "moveTableColumnLeft"; text: qsTr("Move left"); visible: tableMenu.kind === "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "moveColumnRightEntry"; command: "moveTableColumnRight"; text: qsTr("Move right"); visible: tableMenu.kind === "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "alignDefaultEntry"; command: "alignTableColumnDefault"; text: qsTr("Align as written"); visible: tableMenu.kind === "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "alignLeftEntry"; command: "alignTableColumnLeft"; text: qsTr("Align left"); visible: tableMenu.kind === "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "alignCenterEntry"; command: "alignTableColumnCenter"; text: qsTr("Align center"); visible: tableMenu.kind === "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "alignRightEntry"; command: "alignTableColumnRight"; text: qsTr("Align right"); visible: tableMenu.kind === "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "sortAscendingEntry"; command: "sortTableAscending"; text: qsTr("Sort ascending"); visible: tableMenu.kind === "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "sortDescendingEntry"; command: "sortTableDescending"; text: qsTr("Sort descending"); visible: tableMenu.kind === "column"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "deleteColumnEntry"; command: "deleteTableColumn"; text: qsTr("Delete column"); visible: tableMenu.kind !== "row"; height: visible ? implicitHeight : 0 }
    // The cell's, and the table's as a whole.
    TableEntry { objectName: "mergeCellsEntry"; command: "mergeTableCells"; text: qsTr("Merge cells"); visible: tableMenu.kind === "cell"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "splitCellEntry"; command: "splitTableCell"; text: qsTr("Split cell"); visible: tableMenu.kind === "cell"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "showMarkdownEntry"; command: "showTableMarkdown"; text: qsTr("Show as Markdown"); visible: tableMenu.kind === "cell"; height: visible ? implicitHeight : 0 }
    TableEntry { objectName: "deleteTableEntry"; command: "deleteTable"; text: qsTr("Delete table"); visible: tableMenu.kind === "cell"; height: visible ? implicitHeight : 0 }
}
