import QtQuick
import QtQuick.Controls.Basic
import QtQml.Models
import Parch
import "icons.js" as Icons

// The library of one window: a field that narrows it by name, the tree of
// drafts, recent files and folders, and Open folder at its foot. A row opens
// what it names in the window's tabs, through the backend, and a folder or a
// section opens and closes under it.
//
// The field finds a document by name, in folders not opened yet too, which
// the library lists for it; a match is opened to as it is found, and once
// the field is emptied the tree is opened as it was before.
//
// From the field, Down moves into the tree and Enter opens the first match.
// In the tree, Up and Down move, Right and Left open and close a folder or
// go into and out of it, Enter or Space opens a row, and Shift+F10 or the
// menu key offers to take a folder out of the library. Escape anywhere in
// the library empties the field and goes back to the text.
Item {
    id: panel

    required property WindowContext context
    readonly property LibraryModel library: context.library
    // The window's front tab, whose row is marked.
    readonly property string activePath: context.tabs.activePath
    readonly property string activeDocumentId: context.tabs.activeDocument ? context.tabs.activeDocument.id : ""

    // A document was opened from a row.
    signal opened()
    // The person is done with the library and goes back to the text.
    signal leave()

    readonly property real gap: Math.round(theme.fontSize * 2 / 3)
    readonly property real rowHeight: Math.round(theme.fontSize * 2.4)
    readonly property real iconSize: Math.round(theme.fontSize * 1.15)
    // A level of the tree is indented by the room its chevron takes.
    readonly property real indent: iconSize + Math.round(theme.fontSize / 3)

    function focusSearch() {
        search.forceActiveFocus(Qt.TabFocusReason)
    }

    function rowData(row, role) {
        return panel.library.data(tree.index(row, 0), role)
    }

    // The first document or draft the field matches, by the library's own
    // rule, as the tree shows it; -1 for none.
    function firstResult() {
        let first = -1
        for (const index of panel.library.matches()) {
            const kind = panel.library.data(index, LibraryModel.KindRole)
            if (kind !== "document" && kind !== "draft")
                continue
            const row = tree.rowAtIndex(index)
            if (row >= 0 && (first < 0 || row < first))
                first = row
        }
        return first
    }

    // How many rows the tree has. Its own count waits for its next layout;
    // its index of a row follows the model at once, so the count is found
    // from that, by halving.
    function rowCount() {
        if (!tree.index(0, 0).valid)
            return 0
        let low = 1
        let high = 2
        while (tree.index(high - 1, 0).valid) {
            low = high
            high *= 2
        }
        while (high - low > 1) {
            const middle = Math.floor((low + high) / 2)
            if (tree.index(middle - 1, 0).valid)
                low = middle
            else
                high = middle
        }
        return low
    }

    // The row the keyboard is on, wherever rows come and go around it; -1
    // when it has none, or its row is not shown.
    function cursorRow() {
        return tree.rowAtIndex(cursor.currentIndex)
    }

    function moveTo(row) {
        const rows = rowCount()
        if (rows === 0)
            return
        cursor.setCurrentIndex(tree.index(Math.max(0, Math.min(row, rows - 1)), 0), ItemSelectionModel.NoUpdate)
        // Once the tree has laid out the rows it now has.
        Qt.callLater(panel.showCursor)
    }

    function showCursor() {
        const row = cursorRow()
        if (row >= 0 && row < tree.rows)
            tree.positionViewAtRow(row, TableView.Contain)
    }

    // Leaves the library for the text, emptying the field.
    function leaveLibrary() {
        search.clear()
        panel.leave()
    }

    function toggle(row) {
        if (tree.isExpanded(row))
            tree.collapseRecursively(row)
        else
            tree.expand(row)
    }

    // What a row does when it is chosen: a document or a draft opens, in
    // this window unless another already shows it, and a folder or a
    // section opens or closes.
    function activate(row) {
        const kind = rowData(row, LibraryModel.KindRole)
        if (kind === "document") {
            backend.activate(panel.context)
            backend.openFromLibrary(rowData(row, LibraryModel.PathRole))
            panel.opened()
        } else if (kind === "draft") {
            backend.activate(panel.context)
            backend.openDraft(rowData(row, LibraryModel.DocumentIdRole))
            panel.opened()
        } else if (kind === "folder" || kind === "section") {
            toggle(row)
        }
    }

    // A folder at the top of the tree can be taken out of the library.
    function offerMenu(row, item) {
        if (rowData(row, LibraryModel.KindRole) !== "folder" || tree.depth(row) !== 0)
            return
        folderMenu.folder = rowData(row, LibraryModel.PathRole)
        // Placed in the panel, since the tree may reuse the row's item.
        const below = item.mapToItem(panel, item.leftInset || 0, item.height)
        folderMenu.x = below.x
        folderMenu.y = below.y
        folderMenu.open()
    }

    // How many matches one search opens the library to show, nearest first;
    // the rest are there when their folders are opened.
    readonly property int revealLimit: 100
    property int revealed: 0

    // Expands a row and whatever holds it, from the top down, one row at a
    // time, so that the library hears of each folder opened and watches it.
    function openTo(index) {
        const holders = []
        for (let at = index; at.valid; at = at.parent)
            holders.unshift(at)
        for (const holder of holders) {
            const row = tree.rowAtIndex(holder)
            if (row >= 0 && !tree.isExpanded(row))
                tree.expand(row)
        }
    }

    // Expands what holds the nearest rows the filter matches, at most
    // revealLimit of them, so that those are in sight.
    function reveal() {
        if (search.text.length === 0)
            return
        const nearest = panel.library.nearestMatches(revealLimit)
        revealed = nearest.length
        for (const index of nearest)
            openTo(index.parent)
    }

    // Opens a folder at the top of the library and moves to it, with the
    // keys unless `takeKeys` is false.
    function showFolder(path, takeKeys) {
        for (let row = 0; tree.index(row, 0).valid; ++row) {
            if (tree.depth(row) !== 0 || rowData(row, LibraryModel.KindRole) !== "folder"
                    || rowData(row, LibraryModel.PathRole) !== path)
                continue
            if (!tree.isExpanded(row))
                tree.expand(row)
            moveTo(row)
            if (takeKeys !== false)
                tree.forceActiveFocus(Qt.TabFocusReason)
            return
        }
    }

    function openSection(section) {
        if (tree.closedSections[section])
            return
        for (let row = 0; tree.index(row, 0).valid; ++row) {
            if (rowData(row, LibraryModel.SectionRole) === section) {
                if (!tree.isExpanded(row))
                    tree.expand(row)
                return
            }
        }
    }

    // What a row is, whatever row it is at: a folder by its path, a section by its name.
    function keyOf(row) {
        const kind = rowData(row, LibraryModel.KindRole)
        return kind === "section" ? "section:" + rowData(row, LibraryModel.SectionRole)
                                  : kind + ":" + rowData(row, LibraryModel.PathRole)
    }

    // The rows open before a search began, which it may close by hiding
    // them; they open again once the search is cleared.
    property var openBeforeSearch: null

    function rememberOpen() {
        const open = {}
        for (let row = 0; tree.index(row, 0).valid; ++row)
            if (tree.isExpanded(row))
                open[keyOf(row)] = true
        openBeforeSearch = open
    }

    function reopen() {
        const open = openBeforeSearch
        openBeforeSearch = null
        if (!open)
            return
        // What the search opened closes again, from the last row up, so that
        // the rows above keep their places.
        for (let row = rowCount() - 1; row >= 0; --row)
            if (tree.isExpanded(row) && !open[keyOf(row)])
                tree.collapse(row)
        // Opening a row puts its children after it, so they are reached too.
        for (let row = 0; tree.index(row, 0).valid; ++row)
            if (open[keyOf(row)] && !tree.isExpanded(row))
                tree.expand(row)
    }

    TextField {
        id: search
        objectName: "librarySearch"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Math.round(theme.fontSize / 4)
        height: panel.rowHeight
        leftPadding: panel.gap + panel.iconSize + Math.round(theme.fontSize / 2)
        rightPadding: panel.gap
        topPadding: 0
        bottomPadding: 0
        placeholderText: qsTr("Find a document")
        placeholderTextColor: theme.dim
        color: theme.foreground
        selectionColor: theme.selection
        selectedTextColor: theme.foreground
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        verticalAlignment: TextInput.AlignVCenter
        cursorDelegate: FieldCaret { rest: searchCaret }
        Accessible.name: placeholderText
        Accessible.searchEdit: true

        CaretRest {
            id: searchCaret
            objectName: "caretRest"
            target: search
            steady: theme.reduceMotion
        }

        onTextChanged: {
            if (text.length > 0 && !panel.openBeforeSearch)
                panel.rememberOpen()
            panel.revealed = 0
            panel.library.filter = text
            if (text.length > 0)
                Qt.callLater(panel.reveal)
            else
                panel.reopen()
        }
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Escape) {
                panel.leaveLibrary()
                event.accepted = true
            } else if (event.key === Qt.Key_Down) {
                panel.moveTo(Math.max(0, panel.firstResult()))
                tree.forceActiveFocus(Qt.TabFocusReason)
                event.accepted = true
            } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && text.length > 0) {
                // Only a document or a draft opens; with none matching, nothing happens.
                const row = panel.firstResult()
                if (row >= 0) {
                    panel.moveTo(row)
                    panel.activate(row)
                }
                event.accepted = true
            }
        }

        Icon {
            x: panel.gap
            anchors.verticalCenter: parent.verticalCenter
            path: Icons.search
            size: panel.iconSize
            color: search.activeFocus ? theme.foreground : theme.dim
        }

        background: Rectangle {
            radius: theme.radius
            color: search.hovered && !search.activeFocus ? theme.hoverFill : "transparent"
            border.width: search.activeFocus ? Math.max(1, theme.controlBorderWidth) : 0
            border.color: theme.accent
        }
    }

    Rectangle {
        id: searchLine
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: search.bottom
        anchors.topMargin: Math.round(theme.fontSize / 4)
        height: theme.controlBorderWidth
        color: theme.controlBorder
    }

    Text {
        id: heading
        objectName: "libraryHeading"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: searchLine.bottom
        anchors.leftMargin: panel.gap
        anchors.rightMargin: panel.gap
        topPadding: Math.round(theme.fontSize)
        bottomPadding: Math.round(theme.fontSize / 2)
        text: qsTr("Library")
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.sizes.small
        font.capitalization: Font.AllUppercase
        font.letterSpacing: theme.sizes.small / 10
        color: theme.dim
        elide: Text.ElideRight
        Accessible.role: Accessible.Heading
        Accessible.name: text
    }

    TreeView {
        id: tree
        objectName: "libraryTree"

        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: heading.bottom
        anchors.bottom: footLine.top
        anchors.leftMargin: Math.round(theme.fontSize / 4)
        anchors.rightMargin: Math.round(theme.fontSize / 4)
        clip: true
        model: panel.library
        activeFocusOnTab: true
        // The keys and the pointer are handled here, so that every move is
        // the same however it is made, and none of them animates.
        keyNavigationEnabled: false
        pointerNavigationEnabled: false
        animate: false
        boundsBehavior: Flickable.StopAtBounds
        columnWidthProvider: () => tree.width
        // Where the keyboard is: a row of the library, which stays on the
        // same row as rows come and go above it.
        selectionModel: ItemSelectionModel {
            id: cursor
            model: panel.library
        }
        Accessible.role: Accessible.Tree
        Accessible.name: qsTr("Library")

        onWidthChanged: forceLayout()
        // Reached from the keyboard, the tree shows where the keys are at once.
        onActiveFocusChanged: if (activeFocus && panel.cursorRow() < 0) panel.moveTo(0)
        // The sections closed by hand, which stay closed.
        property var closedSections: ({})

        onExpanded: (row, depth) => {
            panel.library.setExpanded(index(row, 0), true)
            // A row opened more than one level deep tells only of itself;
            // the library hears of every row opened beneath it too.
            if (depth > 1) {
                const top = tree.depth(row)
                for (let below = row + 1; index(below, 0).valid && tree.depth(below) > top; ++below)
                    if (isExpanded(below))
                        panel.library.setExpanded(index(below, 0), true)
            }
            delete closedSections[panel.rowData(row, LibraryModel.SectionRole)]
        }
        onCollapsed: (row) => {
            panel.library.setExpanded(index(row, 0), false)
            const section = panel.rowData(row, LibraryModel.SectionRole)
            if (section)
                closedSections[section] = true
        }
        // The two sections start open, and one with nothing in it yet opens
        // when something comes, unless it was closed.
        Component.onCompleted: {
            expand(1)
            expand(0)
        }

        // Once the insertion is over: the tree's own list of rows takes it
        // in its own handler, which may not have run yet.
        Connections {
            target: panel.library
            function onRowsInserted(parent) {
                const section = panel.library.data(parent, LibraryModel.SectionRole)
                if (section)
                    Qt.callLater(panel.openSection, section)
            }
            // Matches a listing for the search adds are opened to as well,
            // while the search has opened to fewer than its limit.
            function onMatchesListed(folder, count) {
                if (search.text.length === 0 || panel.revealed >= panel.revealLimit)
                    return
                panel.revealed += count
                panel.openTo(folder)
            }
        }

        Keys.onPressed: (event) => {
            const count = panel.rowCount()
            const cursorRow = panel.cursorRow()
            // A keyboard whose row has gone, hidden by the filter or removed,
            // starts again from the first row.
            const fromRow = [Qt.Key_Down, Qt.Key_Up, Qt.Key_Right, Qt.Key_Left, Qt.Key_Return, Qt.Key_Enter, Qt.Key_Space]
            if (cursorRow < 0 && fromRow.includes(event.key)) {
                panel.moveTo(0)
                event.accepted = true
                return
            }
            const row = Math.max(0, cursorRow)
            const kind = row >= 0 && row < count ? panel.rowData(row, LibraryModel.KindRole) : ""
            const opens = kind === "folder" || kind === "section"
            switch (event.key) {
            case Qt.Key_Down:
                panel.moveTo(row + 1)
                break
            case Qt.Key_Up:
                if (row === 0)
                    panel.focusSearch()
                else
                    panel.moveTo(row - 1)
                break
            case Qt.Key_Home:
                panel.moveTo(0)
                break
            case Qt.Key_End:
                panel.moveTo(count - 1)
                break
            case Qt.Key_PageDown:
                panel.moveTo(row + Math.max(1, Math.floor(height / panel.rowHeight) - 1))
                break
            case Qt.Key_PageUp:
                panel.moveTo(row - Math.max(1, Math.floor(height / panel.rowHeight) - 1))
                break
            case Qt.Key_Right:
                if (opens && !isExpanded(row))
                    expand(row)
                else if (opens)
                    panel.moveTo(row + 1)
                break
            case Qt.Key_Left:
                if (opens && isExpanded(row)) {
                    collapseRecursively(row)
                } else {
                    // Out to the row it lies in.
                    for (let above = row - 1; above >= 0; --above) {
                        if (depth(above) < depth(row)) {
                            panel.moveTo(above)
                            break
                        }
                    }
                }
                break
            case Qt.Key_Return:
            case Qt.Key_Enter:
            case Qt.Key_Space:
                panel.activate(row)
                break
            case Qt.Key_Escape:
                panel.leaveLibrary()
                break
            case Qt.Key_Menu:
            case Qt.Key_F10:
                if (event.key === Qt.Key_F10 && !(event.modifiers & Qt.ShiftModifier)) {
                    event.accepted = false
                    return
                }
                panel.offerMenu(row, itemAtCell(Qt.point(0, row)) || tree)
                break
            default:
                event.accepted = false
                return
            }
            event.accepted = true
        }

        delegate: AbstractButton {
            id: entry
            objectName: "libraryRow"

            required property TreeView treeView
            required property bool expanded
            required property bool hasChildren
            required property int depth
            required property int row
            required property bool current
            required property string name
            required property string kind
            required property string path
            required property bool missing
            required property string documentId
            required property string section

            readonly property bool active: kind === "draft" ? documentId.length > 0 && documentId === panel.activeDocumentId
                                                             : kind === "document" && path.length > 0 && path === panel.activePath
            // A folder found empty while watched, or a section with nothing in
            // it, has nothing to open.
            readonly property bool opens: (kind === "folder" || kind === "section") && hasChildren
            readonly property bool keyboard: treeView.activeFocus && current

            implicitWidth: tree.width
            implicitHeight: panel.rowHeight
            // A row's fill starts where its level does, beside the chevron of
            // the row it lies in, and what it shows starts a little further in.
            leftInset: depth * panel.indent
            leftPadding: leftInset + Math.round(theme.fontSize / 3)
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            text: name
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            Accessible.role: Accessible.TreeItem
            Accessible.name: name
            Accessible.description: kind === "draft" ? qsTr("Draft")
                                    : kind === "folder" || kind === "document" ? (missing ? qsTr("%1, not found").arg(path) : path)
                                    : ""
            Accessible.expandable: opens
            Accessible.expanded: expanded
            Accessible.selected: active
            Accessible.focused: keyboard

            onClicked: {
                panel.moveTo(row)
                if (opens)
                    tree.forceActiveFocus(Qt.MouseFocusReason)
                panel.activate(row)
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: {
                    panel.moveTo(entry.row)
                    panel.offerMenu(entry.row, entry)
                }
            }

            contentItem: Item {
                Icon {
                    id: chevron
                    visible: entry.opens
                    x: 0
                    anchors.verticalCenter: parent.verticalCenter
                    path: Icons.chevronRight
                    size: Math.round(panel.iconSize * 0.8)
                    color: theme.dim
                    rotation: entry.expanded ? 90 : 0

                    Behavior on rotation {
                        NumberAnimation { duration: theme.reduceMotion ? 0 : 120; easing.type: Easing.OutCubic }
                    }
                }

                Icon {
                    id: kindIcon
                    x: panel.indent
                    anchors.verticalCenter: parent.verticalCenter
                    size: panel.iconSize
                    path: entry.section === "recent" ? Icons.recent
                          : entry.kind === "section" || entry.kind === "folder" ? Icons.folder
                          : entry.kind === "error" ? Icons.problem
                          : Icons.document
                    color: entry.active ? theme.accent
                           : entry.kind === "error" ? theme.danger
                           : entry.missing ? theme.faint
                           : theme.dim
                }

                Text {
                    id: label
                    anchors.left: kindIcon.right
                    anchors.leftMargin: Math.round(theme.fontSize * 2 / 3)
                    anchors.right: parent.right
                    anchors.rightMargin: Math.round(theme.fontSize / 3)
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.name
                    textFormat: Text.PlainText
                    font: entry.font
                    color: entry.active ? theme.accent
                           : entry.missing || entry.kind === "error" ? theme.dim
                           : theme.foreground
                    elide: Text.ElideRight
                }

                ToolTip {
                    objectName: "libraryRowTip"
                    visible: label.truncated && (entry.hovered || entry.keyboard)
                    delay: entry.keyboard ? 0 : 600
                    y: entry.height
                    width: Math.min(implicitWidth, tree.width)
                    text: entry.name
                    font: entry.font
                    padding: Math.round(theme.fontSize / 2)

                    contentItem: Text {
                        text: entry.name
                        textFormat: Text.PlainText
                        font: entry.font
                        color: theme.foreground
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }

                    background: Rectangle {
                        color: theme.menuBackground
                        radius: theme.radius
                        border.width: Math.max(1, theme.controlBorderWidth)
                        border.color: theme.menuBorder
                    }
                }
            }

            background: Rectangle {
                radius: theme.radius
                color: entry.down ? theme.pressedFill : entry.hovered || entry.keyboard ? theme.hoverFill : "transparent"
                border.width: entry.keyboard ? Math.max(1, theme.controlBorderWidth) : 0
                border.color: theme.accent

                // The front document's row: an accent tint and a bar at its edge.
                Rectangle {
                    visible: entry.active
                    anchors.fill: parent
                    radius: theme.radius
                    color: theme.accent
                    opacity: 0.12
                }

                Rectangle {
                    visible: entry.active
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Math.max(2, 2 * theme.controlBorderWidth)
                    color: theme.accent
                }
            }
        }
    }

    // A search that matches nothing listed says so, beneath the sections.
    Text {
        objectName: "libraryNoMatches"
        visible: search.text.length > 0 && panel.library.matchCount === 0
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: panel.gap
        anchors.rightMargin: panel.gap
        y: tree.y + Math.max(0, Math.min(tree.contentHeight + Math.round(theme.fontSize / 2), tree.height - height))
        // Not until every folder the search lists has been looked in.
        text: panel.library.searching ? qsTr("Searching…") : qsTr("No matches in the library")
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize
        color: theme.dim
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    Rectangle {
        id: footLine
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: openFolder.top
        anchors.bottomMargin: Math.round(theme.fontSize / 4)
        height: theme.controlBorderWidth
        color: theme.controlBorder
    }

    ThemedButton {
        id: openFolder
        objectName: "openFolderButton"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Math.round(theme.fontSize / 4)
        height: panel.rowHeight
        leftPadding: panel.gap
        iconPath: Icons.folder
        iconSize: panel.iconSize
        labelled: true
        text: qsTr("Open folder")
        onClicked: {
            backend.activate(panel.context)
            backend.openFolder()
        }
    }

    Menu {
        id: folderMenu
        objectName: "libraryFolderMenu"

        property string folder

        padding: Math.max(1, theme.controlBorderWidth)
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize

        // As the window's menu: no scroll indicator fading at a fixed pace.
        contentItem: ListView {
            implicitWidth: removeEntry.implicitWidth
            implicitHeight: contentHeight
            model: folderMenu.contentModel
            interactive: false
            clip: true
            currentIndex: folderMenu.currentIndex
        }

        background: Rectangle {
            color: theme.menuBackground
            radius: theme.radius
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: theme.menuBorder
        }

        MenuItem {
            id: removeEntry
            text: qsTr("Remove from library")
            implicitHeight: panel.rowHeight
            leftPadding: panel.gap
            rightPadding: panel.gap
            onTriggered: panel.library.removeFolder(folderMenu.folder)

            contentItem: Text {
                text: removeEntry.text
                textFormat: Text.PlainText
                font: removeEntry.font
                color: removeEntry.highlighted ? theme.bright : theme.foreground
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
            }

            background: Rectangle {
                radius: theme.radius
                color: removeEntry.down ? theme.pressedFill : removeEntry.highlighted ? theme.hoverFill : "transparent"
            }
        }
    }
}
