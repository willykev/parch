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
// menu key offers what can be done with the row: a folder of the library can
// be taken out of it, and what lies inside one can be made, renamed (F2),
// moved, also by dragging it onto a folder, or moved to the trash (Delete).
// A recent file can be taken out of Recent (Delete), and Recent cleared; a
// draft can be deleted (Delete), which the notice that says so can undo.
// Escape anywhere in the library empties the field and goes back to the text.
//
// Beneath the folders, the outline of the document in front can be shown.
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
    // A heading of the outline was chosen, at this source offset.
    signal headingChosen(int position)
    // A heading an include brought in, in its own file.
    signal includedHeadingChosen(string path, int line)

    // Where the caret is in the document in front, for its outline.
    property int caret: 0
    // One of the library's dialogs is open, and has the keys.
    readonly property bool dialogOpen: nameDialog.visible || moveDialog.visible || trashDialog.visible
                                       || linkDialog.visible
    // This window asked the organizer for what it is now looking into, so
    // that the question it then has is asked here and in no other window.
    property bool awaitingReview: false
    property bool awaitingTrash: false

    readonly property real gap: Math.round(theme.fontSize * 2 / 3)
    readonly property real rowHeight: Math.round(theme.fontSize * 2.4)
    readonly property real iconSize: Math.round(theme.fontSize * 1.15)
    // A level of the tree is indented by the room its chevron takes.
    readonly property real indent: iconSize + Math.round(theme.fontSize / 3)

    function focusSearch() {
        search.forceActiveFocus(Qt.TabFocusReason)
    }

    // The keys go to the outline of the document in front.
    function focusOutline() {
        outlineList.takeKeys()
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
            backend.openFromLibrary(rowData(row, LibraryModel.PathRole), rowData(row, LibraryModel.OutsideLinkRole))
            panel.opened()
        } else if (kind === "draft") {
            backend.activate(panel.context)
            backend.openDraft(rowData(row, LibraryModel.DocumentIdRole))
            panel.opened()
        } else if (kind === "folder" || kind === "section") {
            toggle(row)
        }
    }

    // What can be done with a row: a folder at the top of the tree can be
    // taken out of the library, and documents made in it; what lies inside
    // a library folder can be made in, renamed, moved and trashed.
    function offerMenu(row, item) {
        const kind = rowData(row, LibraryModel.KindRole)
        const path = rowData(row, LibraryModel.PathRole)
        let menu = null
        if (inRecent(row) || (kind === "section" && rowData(row, LibraryModel.SectionRole) === "recent")) {
            menu = listMenu
            listMenu.kind = kind === "section" ? "recentSection" : "recent"
        } else if (kind === "draft") {
            menu = listMenu
            listMenu.kind = "draft"
            listMenu.documentId = rowData(row, LibraryModel.DocumentIdRole)
        } else if (kind === "folder" && tree.depth(row) === 0) {
            menu = folderMenu
        } else if ((kind === "folder" || kind === "document") && backend.organizer.organizes(path)) {
            menu = entryMenu
            entryMenu.isFolder = kind === "folder"
        }
        if (!menu)
            return
        menu.folder = path
        // Placed in the panel, since the tree may reuse the row's item.
        const below = item.mapToItem(panel, item.leftInset || 0, item.height)
        menu.x = below.x
        menu.y = below.y
        menu.open()
    }

    // Whether the row is a file in the Recent section.
    function inRecent(row) {
        const index = tree.index(row, 0)
        return index.valid && rowData(row, LibraryModel.KindRole) === "document"
               && panel.library.data(index.parent, LibraryModel.SectionRole) === "recent"
    }

    // What Delete does on a row: a recent file leaves Recent, a draft is
    // deleted, and what lies inside a library folder is offered to the trash.
    function deleteRow(row) {
        if (row < 0)
            return
        backend.activate(panel.context)
        if (inRecent(row))
            backend.forgetRecent(rowData(row, LibraryModel.PathRole))
        else if (rowData(row, LibraryModel.KindRole) === "draft")
            backend.deleteDraft(rowData(row, LibraryModel.DocumentIdRole))
        else
            askTrash(rowData(row, LibraryModel.PathRole))
    }

    function nameOf(path) {
        return path.substring(path.lastIndexOf("/") + 1)
    }

    // Each question below gives the keys back to the tree once it closes;
    // asked for the document in front, from the File menu or the palette,
    // it gives them to the text.
    property bool keysToText: false

    function askNewFile(folder) {
        panel.keysToText = false
        nameDialog.task = { kind: "file", path: folder }
        nameDialog.ask(qsTr("New document in “%1”").arg(nameOf(folder)), qsTr("Make"), "",
                       qsTr("A name without .md, .markdown or .txt gets .md."))
    }

    function askNewFolder(folder) {
        panel.keysToText = false
        nameDialog.task = { kind: "folder", path: folder }
        nameDialog.ask(qsTr("New folder in “%1”").arg(nameOf(folder)), qsTr("Make"), "", "")
    }

    function askRename(path) {
        if (!backend.organizer.organizes(path))
            return
        panel.keysToText = false
        nameDialog.task = { kind: "rename", path: path }
        nameDialog.ask(qsTr("Rename “%1”").arg(nameOf(path)), qsTr("Rename"), nameOf(path), "")
    }

    function askMove(path) {
        if (!backend.organizer.organizes(path))
            return
        panel.keysToText = false
        moveDialog.ask(path)
    }

    // Moves what is at the path into the folder. The review it may need is
    // asked here while the organizer looks for references.
    function moveInto(path, folder) {
        panel.keysToText = false
        backend.activate(panel.context)
        panel.awaitingReview = true
        backend.organizer.move(path, folder)
        panel.awaitingReview = backend.organizer.busy
    }

    function askTrash(path) {
        if (!backend.organizer.organizes(path))
            return
        panel.keysToText = false
        backend.activate(panel.context)
        panel.awaitingTrash = true
        backend.organizer.askToTrash(path)
        panel.awaitingTrash = backend.organizer.busy
    }

    // The same for the document in front.
    function renameDocument(path) {
        askRename(path)
        panel.keysToText = true
    }
    function moveDocument(path) {
        askMove(path)
        panel.keysToText = true
    }
    function trashDocument(path) {
        askTrash(path)
        panel.keysToText = true
    }
    function newDocumentBeside(path) {
        askNewFile(path.substring(0, path.lastIndexOf("/")))
        panel.keysToText = true
    }

    // Where the keys go once a question is closed, the review of links a
    // move may ask next among them.
    function keysBack() {
        if (panel.keysToText)
            panel.leave()
        else
            tree.forceActiveFocus(Qt.TabFocusReason)
    }

    // The row the keyboard is on, as {kind, path}.
    function cursorEntry() {
        const row = cursorRow()
        return row < 0 ? { kind: "", path: "" } : { kind: rowData(row, LibraryModel.KindRole), path: rowData(row, LibraryModel.PathRole) }
    }

    // What is dragged onto a folder, and where it is.
    property string dragPath
    property string dropFolder

    // Whether what is dragged can go into this folder: not into itself,
    // what it holds, or the folder it is in already.
    function canDrop(folder) {
        const path = panel.dragPath
        return path.length > 0 && folder !== path && !folder.startsWith(path + "/")
               && folder !== path.substring(0, path.lastIndexOf("/"))
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
        anchors.bottom: outlineList.top
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
            case Qt.Key_F2:
                panel.askRename(panel.cursorEntry().path)
                break
            case Qt.Key_Delete:
                panel.deleteRow(row)
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
            required property bool outsideLink
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
            Accessible.description: outsideLink ? qsTr("Link outside the library: %1").arg(path)
                                    : kind === "draft" ? qsTr("Draft")
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

            // A document or folder inside a library folder can be dragged
            // onto another folder, and moves there.
            DragHandler {
                id: dragger
                target: null
                enabled: !entry.outsideLink && (entry.kind === "document" || entry.kind === "folder") && entry.section.length === 0
                         && backend.organizer.organizes(entry.path)
                onActiveChanged: {
                    if (active) {
                        panel.dragPath = entry.path
                        dragProxy.label = entry.name
                        dragProxy.Drag.active = true
                    } else {
                        if (panel.dropFolder.length > 0)
                            dragProxy.Drag.drop()
                        dragProxy.Drag.active = false
                        panel.dragPath = ""
                        panel.dropFolder = ""
                    }
                }
                onCentroidChanged: {
                    if (!active)
                        return
                    const at = entry.mapToItem(panel, centroid.position.x, centroid.position.y)
                    dragProxy.x = at.x + Math.round(theme.fontSize / 2)
                    dragProxy.y = at.y + Math.round(theme.fontSize / 2)
                }
            }

            DropArea {
                anchors.fill: parent
                keys: ["parch-library-entry"]
                enabled: !entry.outsideLink && entry.kind === "folder"
                onEntered: (drag) => {
                    drag.accepted = panel.canDrop(entry.path)
                    if (drag.accepted)
                        panel.dropFolder = entry.path
                }
                onExited: if (panel.dropFolder === entry.path) panel.dropFolder = ""
                onDropped: (drop) => {
                    const path = panel.dragPath
                    panel.dropFolder = ""
                    if (panel.canDrop(entry.path)) {
                        drop.accept()
                        panel.moveInto(path, entry.path)
                    }
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
                          : entry.outsideLink ? Icons.link
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
                readonly property bool dropping: panel.dropFolder.length > 0 && panel.dropFolder === entry.path

                radius: theme.radius
                color: entry.down || dropping ? theme.pressedFill : entry.hovered || entry.keyboard ? theme.hoverFill : "transparent"
                border.width: entry.keyboard || dropping ? Math.max(1, theme.controlBorderWidth) : 0
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

    // What is dragged, beside the pointer.
    Rectangle {
        id: dragProxy
        objectName: "libraryDragProxy"

        property string label

        visible: Drag.active
        z: 10
        width: Math.min(dragLabel.implicitWidth + theme.fontSize, panel.width / 2)
        height: panel.rowHeight
        radius: theme.radius
        color: theme.menuBackground
        border.width: Math.max(1, theme.controlBorderWidth)
        border.color: theme.menuBorder
        Drag.keys: ["parch-library-entry"]
        Drag.hotSpot.x: 0
        Drag.hotSpot.y: 0

        Text {
            id: dragLabel
            anchors.fill: parent
            anchors.leftMargin: Math.round(theme.fontSize / 2)
            anchors.rightMargin: Math.round(theme.fontSize / 2)
            text: dragProxy.label
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.fontSize
            color: theme.foreground
            elide: Text.ElideMiddle
            verticalAlignment: Text.AlignVCenter
        }
    }

    OutlineList {
        id: outlineList
        objectName: "outlineList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footLine.top
        anchors.leftMargin: Math.round(theme.fontSize / 4)
        anchors.rightMargin: Math.round(theme.fontSize / 4)
        height: implicitHeight
        maxListHeight: Math.round(panel.height * 0.4)
        document: panel.context.tabs.activeDocument
        caret: panel.caret
        onChosen: (position) => panel.headingChosen(position)
        onIncludedChosen: (path, line) => panel.includedHeadingChosen(path, line)
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
            implicitWidth: Math.max(removeEntry.implicitWidth, topNewDocument.implicitWidth, topNewFolder.implicitWidth)
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

        MenuEntry {
            id: removeEntry
            text: qsTr("Remove from library")
            onTriggered: panel.library.removeFolder(folderMenu.folder)
        }
        MenuEntry {
            id: topNewDocument
            text: qsTr("New document…")
            onTriggered: panel.askNewFile(folderMenu.folder)
        }
        MenuEntry {
            id: topNewFolder
            text: qsTr("New folder…")
            onTriggered: panel.askNewFolder(folderMenu.folder)
        }
    }

    // What can be done with a document or folder inside a library folder.
    Menu {
        id: entryMenu
        objectName: "libraryEntryMenu"

        property string folder
        property bool isFolder: false

        padding: Math.max(1, theme.controlBorderWidth)
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize

        contentItem: ListView {
            implicitWidth: Math.max(newDocumentEntry.implicitWidth, newFolderEntry.implicitWidth, renameEntry.implicitWidth,
                                    moveEntry.implicitWidth, trashEntry.implicitWidth)
            implicitHeight: contentHeight
            model: entryMenu.contentModel
            interactive: false
            clip: true
            currentIndex: entryMenu.currentIndex
        }

        background: Rectangle {
            color: theme.menuBackground
            radius: theme.radius
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: theme.menuBorder
        }

        MenuEntry {
            id: newDocumentEntry
            objectName: "menuNewDocument"
            shown: entryMenu.isFolder
            text: qsTr("New document…")
            onTriggered: panel.askNewFile(entryMenu.folder)
        }
        MenuEntry {
            id: newFolderEntry
            objectName: "menuNewFolder"
            shown: entryMenu.isFolder
            text: qsTr("New folder…")
            onTriggered: panel.askNewFolder(entryMenu.folder)
        }
        MenuEntry {
            id: renameEntry
            objectName: "menuRename"
            text: qsTr("Rename…")
            treeKey: "F2"
            onTriggered: panel.askRename(entryMenu.folder)
        }
        MenuEntry {
            id: moveEntry
            objectName: "menuMove"
            text: qsTr("Move to…")
            onTriggered: panel.askMove(entryMenu.folder)
        }
        MenuEntry {
            id: trashEntry
            objectName: "menuTrash"
            text: qsTr("Move to trash…")
            treeKey: "Del"
            danger: true
            onTriggered: panel.askTrash(entryMenu.folder)
        }
    }

    // What can be done with a recent file, with Recent, or with a draft.
    Menu {
        id: listMenu
        objectName: "libraryListMenu"

        // "recent", "recentSection" or "draft".
        property string kind
        property string folder
        property string documentId

        padding: Math.max(1, theme.controlBorderWidth)
        font.family: theme.fontFamily
        font.pixelSize: theme.fontSize

        // Which entries show changes with the row, so the list is laid out
        // again as it opens.
        onAboutToShow: listEntries.forceLayout()

        contentItem: ListView {
            id: listEntries
            implicitWidth: Math.max(forgetRecentEntry.implicitWidth, clearRecentEntry.implicitWidth,
                                    deleteDraftEntry.implicitWidth)
            implicitHeight: forgetRecentEntry.height + clearRecentEntry.height + deleteDraftEntry.height
            model: listMenu.contentModel
            interactive: false
            clip: true
            currentIndex: listMenu.currentIndex
        }

        background: Rectangle {
            color: theme.menuBackground
            radius: theme.radius
            border.width: Math.max(1, theme.controlBorderWidth)
            border.color: theme.menuBorder
        }

        MenuEntry {
            id: forgetRecentEntry
            objectName: "menuForgetRecent"
            shown: listMenu.kind === "recent"
            text: qsTr("Remove from recent")
            treeKey: "Del"
            onTriggered: {
                backend.activate(panel.context)
                backend.forgetRecent(listMenu.folder)
            }
        }
        MenuEntry {
            id: clearRecentEntry
            objectName: "menuClearRecent"
            shown: listMenu.kind === "recent" || listMenu.kind === "recentSection"
            text: qsTr("Clear recent files")
            onTriggered: {
                backend.activate(panel.context)
                backend.clearRecent()
            }
        }
        MenuEntry {
            id: deleteDraftEntry
            objectName: "menuDeleteDraft"
            shown: listMenu.kind === "draft"
            text: qsTr("Delete draft")
            treeKey: "Del"
            danger: true
            onTriggered: {
                backend.activate(panel.context)
                backend.deleteDraft(listMenu.documentId)
            }
        }
    }

    // An entry of the library's menus, in the window menu's look.
    component MenuEntry: MenuItem {
        id: menuEntry

        // Hidden entries take no room.
        property bool shown: true
        // The key that does the same in the tree, shown at its end; the
        // tree's own, not a shortcut of the window's.
        property string treeKey
        property bool danger: false

        visible: shown
        height: shown ? implicitHeight : 0
        implicitHeight: panel.rowHeight
        implicitWidth: label.implicitWidth + keyLabel.implicitWidth + 3 * panel.gap
        leftPadding: panel.gap
        rightPadding: panel.gap

        contentItem: Item {
            Text {
                id: label
                anchors.left: parent.left
                anchors.right: keyLabel.left
                anchors.verticalCenter: parent.verticalCenter
                text: menuEntry.text
                textFormat: Text.PlainText
                font: menuEntry.font
                color: menuEntry.danger ? theme.danger : menuEntry.highlighted ? theme.bright : theme.foreground
                elide: Text.ElideRight
            }
            Text {
                id: keyLabel
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: menuEntry.treeKey
                textFormat: Text.PlainText
                font.family: theme.fontFamily
                font.pixelSize: theme.sizes.small
                color: theme.dim
            }
        }

        background: Rectangle {
            radius: theme.radius
            color: menuEntry.down ? theme.pressedFill : menuEntry.highlighted ? theme.hoverFill : "transparent"
        }
    }

    // The library's questions, each giving the keys back to the tree.
    NameDialog {
        id: nameDialog
        objectName: "libraryNameDialog"

        property var task: ({})

        onChosen: (name) => {
            backend.activate(panel.context)
            const task = nameDialog.task
            if (task.kind === "file") {
                backend.organizer.newFile(task.path, name)
            } else if (task.kind === "folder") {
                backend.organizer.newFolder(task.path, name)
            } else if (task.kind === "rename") {
                panel.awaitingReview = true
                backend.organizer.rename(task.path, name)
                panel.awaitingReview = backend.organizer.busy
            }
        }
        onClosed: panel.keysBack()
    }

    MoveDialog {
        id: moveDialog
        objectName: "libraryMoveDialog"
        onChosen: (folder) => {
            // A move asked for the document in front keeps the keys for its text.
            const toText = panel.keysToText
            panel.moveInto(moveDialog.path, folder)
            panel.keysToText = toText
        }
        onClosed: panel.keysBack()
    }

    TrashDialog {
        id: trashDialog
        objectName: "libraryTrashDialog"
        onClosed: panel.keysBack()
    }

    LinkUpdateDialog {
        id: linkDialog
        objectName: "libraryLinkDialog"
        onClosed: panel.keysBack()
    }

    // The organizer's questions come to the window that asked.
    Connections {
        target: backend.organizer
        function onReviewChanged() {
            if (backend.organizer.review.title && panel.awaitingReview) {
                panel.awaitingReview = false
                linkDialog.open()
            } else if (!backend.organizer.review.title && linkDialog.visible) {
                linkDialog.close()
            }
        }
        function onTrashingChanged() {
            if (backend.organizer.trashing.path && panel.awaitingTrash) {
                panel.awaitingTrash = false
                trashDialog.open()
            }
        }
        // A move that needed no review is done; this window waits for no question.
        function onBusyChanged() {
            if (!backend.organizer.busy && !backend.organizer.review.title)
                panel.awaitingReview = false
        }
    }
}
