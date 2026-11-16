import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import Parch

// One window: the header, the library beside the editor or over it, the
// tabs, the editor and the status line, every colour, size and corner taken
// from the live theme and every measure from font metrics. The window draws
// no title bar, border or shadow of its own; the compositor draws its frame.
ApplicationWindow {
    id: window

    // What this window owns: its tabs, its library and its size.
    required property WindowContext context
    readonly property TabModel tabs: context.tabs
    // The document this window shows: the front tab's, or null for a tab
    // whose file is not found. Everything here reads it from this one property.
    readonly property Document document: tabs.activeDocument
    // The height of the header, the tab strip and the status line.
    readonly property int rowHeight: Math.round(theme.fontSize * 2.6)
    // The library docks beside the editor while the editor keeps room for 45
    // characters and its margins; otherwise it opens over the editor.
    readonly property bool libraryDocked: width - settings.libraryWidth >= 45 * editorPane.characterWidth + 2 * editorPane.margin
    // Whether the docked library is shown. The drawer opens only when asked.
    readonly property bool libraryOpen: context.libraryOpen

    // A dialog of the window is open. It has the keys until it closes:
    // nothing moves them out of it, the editor behind it takes no input, and
    // the window's shortcuts wait.
    readonly property bool dialogOpen: conflictDialog.visible || historyDialog.visible || closeConfirmation.visible
    // The window's shortcuts answer only in the window in front, and not over a dialog.
    readonly property bool shortcutsOn: active && !dialogOpen

    // The one way the keys go back to the text: never while a dialog is
    // open. A dialog that closes calls it in turn, so that the keys then go
    // to the text, which may be a tab the dialog's choice opened.
    function focusText() {
        if (!dialogOpen)
            editorPane.focusText()
    }

    // The one way the keys go to the library, shown beside the editor or
    // over it: to a folder's row when a path is given, and otherwise to its
    // field. Never while a dialog is open, when it returns false and the
    // keys stay with the dialog.
    function focusLibrary(path) {
        if (dialogOpen)
            return false
        if (libraryDocked) {
            context.libraryOpen = true
            // A folder's row, once the library has its rows.
            if (path)
                Qt.callLater(window.revealFolder, path)
            else
                libraryPanel.focusSearch()
        } else if (!drawer.visible) {
            folderPending = path || ""
            drawer.open()
        } else if (path) {
            folderPending = path
            if (drawer.opened)
                drawer.showPending()
        } else {
            libraryPanel.focusSearch()
        }
        return true
    }

    // A folder's row in the docked library, with the keys unless a dialog
    // opened meanwhile.
    function revealFolder(path) {
        libraryPanel.showFolder(path, !dialogOpen)
    }

    // Where the keys go as a dialog closes: to the library over the text,
    // while it is open there, and otherwise to the text.
    function dialogClosed() {
        if (dialogOpen)
            return
        if (drawer.opened)
            drawer.takeKeys()
        else
            focusText()
    }

    // Shown, the library has the keys, in its field; hidden, the text has them back.
    function toggleLibrary() {
        if (libraryDocked && context.libraryOpen) {
            context.libraryOpen = false
            window.focusText()
        } else if (!libraryDocked && drawer.visible) {
            drawer.close()
        } else {
            focusLibrary("")
        }
    }

    // The library, shown beside the editor or over it, with the keys in its
    // field, to find a document by name.
    function findDocument() {
        focusLibrary("")
    }

    // The window's menu, from the keyboard: its first entry is highlighted
    // and has the keys, and the arrows and Enter go on from there.
    function openMenu() {
        windowMenu.open()
        windowMenu.currentIndex = 0
    }

    // Moves to the next tab, or the previous one for a step of -1, round the ends.
    function stepTab(step) {
        if (tabs.count > 1)
            tabs.activeIndex = (tabs.activeIndex + step + tabs.count) % tabs.count
    }

    // Keeps where the editor is in the front tab, for the session and for
    // coming back to the tab.
    function captureView() {
        if (tabs.activeIndex < 0 || !document || editorPane.document !== document)
            return
        const view = editorPane.viewState()
        tabs.setView(tabs.activeIndex, view.cursor, view.anchor, view.top)
    }

    // Shows the front tab where its view was.
    function showView() {
        if (tabs.activeIndex < 0 || !document)
            return
        const view = tabs.viewAt(tabs.activeIndex)
        editorPane.restoreView(view.cursor, view.anchor, view.top)
    }

    // Does what the backend does for the window in front, in this window: a
    // request from its menu or keys can come before the backend has heard
    // that this window came to the front.
    function act(action) {
        backend.activate(window.context)
        action()
    }

    // The document in front's conflict, side by side with its file.
    function reviewConflict() {
        if (document && document.state === Document.Conflict)
            conflictDialog.review(document)
    }

    // Saves the document in front; one in conflict waits for a choice, so
    // its review opens instead.
    function save() {
        if (document && document.state === Document.Conflict)
            reviewConflict()
        else
            act(() => backend.save())
    }

    // The window's regions, in the order F6 visits them: the library, at its
    // field; the notice bar, at its first button; and the text. A library or
    // notice bar not shown, or a notice bar with no button, is passed over.
    readonly property int libraryRegion: 0
    readonly property int noticeRegion: 1
    readonly property int textRegion: 2
    // Where the keys go once the library over the editor has closed.
    property int regionAfterDrawer: textRegion

    function regionOf(item) {
        for (let at = item; at; at = at.parent) {
            if (at === libraryPanel)
                return libraryRegion
            if (at === noticeBar)
                return noticeRegion
        }
        return textRegion
    }

    // Gives the keys to the next region shown, or the one before for a step of -1.
    function stepRegion(step) {
        const shown = [library.visible || drawer.visible, noticeBar.reachable, true]
        let region = regionOf(activeFocusItem)
        do {
            region = (region + step + shown.length) % shown.length
        } while (!shown[region])
        if (region === libraryRegion) {
            focusLibrary("")
        } else if (drawer.visible) {
            // Leaving the library over the editor closes it, and the keys go
            // on once it has.
            regionAfterDrawer = region
            drawer.close()
        } else {
            focusRegion(region)
        }
    }

    function focusRegion(region) {
        if (region !== noticeRegion || !noticeBar.focusFirst())
            focusText()
    }

    function showHistory() {
        if (document)
            historyDialog.review(document)
    }

    // A folder the library is to show: the library opens on it, beside the
    // editor or over it, with the keys on its row. Shown with a document
    // opened at the same time, or while a dialog is open, the keys stay
    // where they are: the docked library shows the folder without them, and
    // the library over the editor waits until it is asked for.
    property string folderPending
    function showFolder() {
        const path = context.folderToShow
        if (!path)
            return
        const keys = context.folderTakesKeys
        context.folderToShow = ""
        if (keys && focusLibrary(path))
            return
        if (libraryDocked)
            Qt.callLater(libraryPanel.showFolder, path, false)
    }

    // The size the session kept, set once: afterwards the window's size is its own.
    width: context.initialSize.width > 0 ? context.initialSize.width : 960
    height: context.initialSize.height > 0 ? context.initialSize.height : 640
    visible: true
    title: tabs.activeTitle ? qsTr("%1 — parch").arg(tabs.activeTitle) : qsTr("parch")
    color: theme.background
    font.family: theme.fontFamily
    font.pixelSize: theme.fontSize
    // What a Basic control draws with wherever parch does not draw it itself.
    palette {
        window: theme.background
        windowText: theme.foreground
        base: theme.background
        text: theme.foreground
        button: theme.controlFill
        buttonText: theme.foreground
        highlight: theme.selection
        highlightedText: theme.foreground
        light: theme.hoverFill
        midlight: theme.pressedFill
        mid: theme.controlBorder
        dark: theme.controlBorder
        shadow: theme.scrim
        toolTipBase: theme.menuBackground
        toolTipText: theme.foreground
        placeholderText: theme.dim
    }

    onLibraryDockedChanged: if (libraryDocked) drawer.close()
    // A dialog speaks of the document in front; another in front ends it.
    onDocumentChanged: {
        conflictDialog.close()
        historyDialog.close()
        closeConfirmation.close()
    }
    onWidthChanged: context.size = Qt.size(width, height)
    onHeightChanged: context.size = Qt.size(width, height)
    // The window in front is the one the backend's actions act on.
    onActiveChanged: if (active) backend.activate(context)

    // The backend closes the window once its text is kept; closing the last
    // window is quitting, which waits until every text is safe.
    onClosing: (close) => {
        close.accepted = false
        backend.closeWindow(context)
    }

    Connections {
        target: window.tabs
        function onActiveAboutToChange() { window.captureView() }
        function onActiveViewChanged() { window.showView() }
    }

    Connections {
        target: window.context
        function onViewCaptureRequested() { window.captureView() }
        function onRaiseRequested() { window.requestActivate() }
        function onFolderToShowChanged() { window.showFolder() }
        // At once, so that a dialog opened after it keeps the keys. The
        // dialogs about the document in front have closed with its change;
        // one still open is about the document in front, and keeps them.
        function onTextFocusRequested() {
            if (window.dialogOpen)
                return
            if (drawer.visible)
                drawer.close()
            window.focusText()
        }
        function onCloseConfirmationRequested(document, discarding) { closeConfirmation.ask(document, discarding) }
    }

    // After the editor has its width, so that the first line can be found.
    Component.onCompleted: {
        Qt.callLater(window.showView)
        Qt.callLater(window.showFolder)
    }

    // The text of the shortcuts, by action, for the menu and the tooltips.
    readonly property var keys: ({
        newDraft: newDraftKey.nativeText,
        newWindow: newWindowKey.nativeText,
        open: openKey.nativeText,
        save: saveKey.nativeText,
        saveAs: saveAsKey.nativeText,
        closeTab: closeTabKey.nativeText,
        find: findKey.nativeText,
        menu: menuKey.nativeText,
        library: libraryKey.nativeText,
        zoomIn: zoomInKey.nativeText,
        zoomOut: zoomOutKey.nativeText,
        resetZoom: resetZoomKey.nativeText,
        quit: quitKey.nativeText
    })

    // Every window has these shortcuts, and only the one in front answers
    // them, while none of its dialogs is open.
    Shortcut { id: newDraftKey; sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.act(() => backend.newDraft()) }
    Shortcut { id: newWindowKey; sequence: "Ctrl+Shift+N"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.act(() => backend.newWindow()) }
    Shortcut { id: openKey; sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.act(() => backend.open()) }
    Shortcut { id: saveKey; sequence: "Ctrl+S"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.save() }
    Shortcut { id: saveAsKey; sequence: "Ctrl+Shift+S"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.act(() => backend.saveAs()) }
    Shortcut {
        id: closeTabKey
        sequence: "Ctrl+W"
        context: Qt.ApplicationShortcut
        enabled: window.shortcutsOn
        onActivated: window.act(() => backend.requestCloseTab(window.tabs.activeIndex))
    }
    // The editor takes these keys itself while it has focus, so the window's
    // shortcuts act only when it does not: one step for each press either way.
    Shortcut { sequence: "Ctrl+Z"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: editorPane.undo() }
    Shortcut { sequence: "Ctrl+Shift+Z"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: editorPane.redo() }
    Shortcut { sequence: "Ctrl+Y"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: editorPane.redo() }
    // Tabs step with Ctrl+Tab and Ctrl+Shift+Tab, or Ctrl+PageDown and
    // Ctrl+PageUp, as in other tabbed applications.
    Shortcut { sequence: "Ctrl+Tab"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.stepTab(1) }
    Shortcut { sequence: "Ctrl+Shift+Tab"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.stepTab(-1) }
    Shortcut { sequence: "Ctrl+PgDown"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.stepTab(1) }
    Shortcut { sequence: "Ctrl+PgUp"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.stepTab(-1) }
    // Ctrl+= is Ctrl and + without Shift on a US keyboard; Ctrl++ is the same
    // on keyboards with a + key of its own.
    Shortcut { id: zoomInKey; sequence: "Ctrl+="; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: settings.zoom(1) }
    Shortcut { sequence: "Ctrl++"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: settings.zoom(1) }
    Shortcut { id: zoomOutKey; sequence: "Ctrl+-"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: settings.zoom(-1) }
    Shortcut { id: resetZoomKey; sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: settings.resetZoom() }
    Shortcut { id: findKey; sequence: "Ctrl+Shift+O"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.findDocument() }
    Shortcut { id: menuKey; sequence: "F10"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.openMenu() }
    Shortcut { id: libraryKey; sequence: "Ctrl+B"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.toggleLibrary() }
    // F6 and Shift+F6 move the keys between the window's regions, as GTK
    // applications do.
    Shortcut { sequences: ["F6"]; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.stepRegion(1) }
    Shortcut { sequences: ["Shift+F6"]; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.stepRegion(-1) }
    Shortcut { id: quitKey; sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; enabled: window.shortcutsOn; onActivated: window.act(() => backend.quit()) }

    HeaderBar {
        id: header
        objectName: "header"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: window.rowHeight
        document: window.document
        title: window.tabs.activeTitle
        place: window.tabs.count > 0 ? backend.folderLabel(window.tabs.activePath, settings.libraryFolders) : ""
        libraryWidth: library.visible ? library.width : 0
        libraryShown: library.visible || drawer.visible
        keys: window.keys
        onToggleLibrary: window.toggleLibrary()
        onMenuRequested: windowMenu.open()
    }

    WindowMenu {
        id: windowMenu
        objectName: "windowMenu"
        parent: header
        x: header.width - width - Math.round(theme.fontSize / 4)
        y: header.height
        keys: window.keys
        libraryShown: header.libraryShown
        canNewView: window.document !== null
        hasDocument: window.document !== null
        // Each action is this window's, whichever the backend last heard was in front.
        onNewDraft: window.act(() => backend.newDraft())
        onNewWindow: window.act(() => backend.newWindow())
        onNewView: window.act(() => backend.newView())
        onOpenFiles: window.act(() => backend.open())
        onOpenFolder: window.act(() => backend.openFolder())
        onSave: window.save()
        onSaveAs: window.act(() => backend.saveAs())
        onShowHistory: window.showHistory()
        onCloseTab: window.act(() => backend.requestCloseTab(window.tabs.activeIndex))
        // Once the menu has closed and given the keys back to the text.
        onFindDocument: Qt.callLater(window.findDocument)
        onToggleLibrary: window.toggleLibrary()
        onZoomIn: settings.zoom(1)
        onZoomOut: settings.zoom(-1)
        onResetZoom: settings.resetZoom()
        onQuit: window.act(() => backend.quit())
        onClosed: window.focusText()
    }

    Item {
        id: body
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: header.bottom
        anchors.bottom: parent.bottom

        // The library's place beside the editor. Its panel moves into the
        // drawer while the window is too narrow to keep it here.
        Rectangle {
            id: library
            objectName: "library"

            readonly property bool docked: window.libraryDocked

            visible: docked && window.libraryOpen
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: settings.libraryWidth
            color: theme.panel
        }

        Rectangle {
            id: divider
            visible: library.visible
            anchors.left: library.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: theme.controlBorderWidth
            color: theme.controlBorder
        }

        Item {
            id: writing
            anchors.left: library.visible ? divider.right : parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom

            TabStrip {
                id: tabStrip
                objectName: "tabStrip"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: visible ? window.rowHeight : 0
                model: window.tabs
                currentIndex: window.tabs.activeIndex
                keys: window.keys
                onActivated: (index) => window.tabs.activeIndex = index
                // A click can come before the window has said it is in front.
                onCloseRequested: (index) => {
                    backend.activate(window.context)
                    backend.requestCloseTab(index)
                }
                onNewRequested: {
                    backend.activate(window.context)
                    backend.newDraft()
                }
            }

            // Above the editor and in the layout, so it never covers the
            // text. Its buttons come before the editor for Tab, which types
            // in the editor; a click on them leaves the keys in the text.
            NoticeBar {
                id: noticeBar
                objectName: "noticeBar"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: tabStrip.bottom
                height: implicitHeight
                document: window.document
                historyPaused: backend.historyPaused
                historyFull: backend.historyFull
                recoveryFailure: window.document ? backend.recoveryFailures[window.document.id] || "" : ""
                notice: backend.notice ? backend.problem : ""
                noticeDetail: backend.notice ? backend.problemDetail : ""
                noticeTone: backend.noticeTone
                onRetrySave: {
                    backend.activate(window.context)
                    backend.retrySave()
                    window.focusText()
                }
                onReadAgain: {
                    backend.activate(window.context)
                    backend.readAgain()
                    window.focusText()
                }
                onSaveAs: {
                    backend.activate(window.context)
                    backend.saveAs()
                    window.focusText()
                }
                onRetryRecovery: {
                    backend.activate(window.context)
                    backend.retryRecovery()
                    window.focusText()
                }
                onReviewConflict: window.reviewConflict()
                onReviewHistory: window.showHistory()
                onDismiss: {
                    backend.dismissNotice()
                    window.focusText()
                }
            }

            // Before the editor, so that Tab reaches it from the window's
            // controls: in the editor, Tab types. It names a refused quit;
            // notices are the notice bar's.
            StatusLine {
                id: statusLine
                objectName: "statusLine"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: window.rowHeight
                document: window.document
                status: window.tabs.activeStatus
                problem: backend.notice ? "" : backend.problem
                problemDetail: backend.notice ? "" : backend.shownText(backend.problemDetail)
            }

            EditorPane {
                id: editorPane
                objectName: "editorPane"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: noticeBar.bottom
                anchors.bottom: statusLine.top
                visible: window.document !== null
                document: window.document
                title: window.tabs.activeTitle
                maxLineLength: settings.maxLineLength
                fontPixelSize: settings.fontPixelSize
                lineHeight: settings.lineHeight
                fontFamily: settings.editorFontFamily
                inputBlocked: window.dialogOpen
                onViewMoved: window.context.noteViewMoved()
                // Escape in the text dismisses a notice that has nothing behind it.
                onEscapePressed: if (backend.notice) backend.dismissNotice()
            }

            // In place of the editor, for a tab whose file is not there: what
            // the tab says and the file it looked for.
            Item {
                id: missingFile
                objectName: "missingFile"
                visible: !editorPane.visible && window.tabs.count > 0
                anchors.fill: editorPane

                Column {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 2 * editorPane.margin, 60 * editorPane.characterWidth)
                    spacing: Math.round(theme.fontSize / 2)

                    Text {
                        objectName: "missingFileState"
                        width: parent.width
                        text: window.tabs.activeStatus
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizes.title
                        color: theme.bright
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    Text {
                        objectName: "missingFilePath"
                        width: parent.width
                        text: backend.shownText(window.tabs.activePath)
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.fontSize
                        color: theme.dim
                        horizontalAlignment: Text.AlignHCenter
                        // Between folders where it can, inside a long name only where it must.
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }
            }
        }
    }

    // The library over the editor, in a window too narrow to keep it beside,
    // sliding in from the left edge like a drawer. It is the template's
    // dialog, whose title names it for assistive technology, rather than a
    // drawer, which can be given no name, and it slides by its own animation,
    // which reduced motion stops.
    T.Dialog {
        id: drawer
        objectName: "libraryDrawer"
        parent: T.Overlay.overlay
        x: 0
        y: header.height
        width: Math.min(settings.libraryWidth, window.width - window.rowHeight)
        height: window.height - header.height
        padding: 0
        // The content stops short of the edge toward the editor, so nothing
        // in it can cover that edge.
        rightPadding: theme.controlBorderWidth
        modal: true
        dim: true
        title: qsTr("Library")
        enter: Transition { NumberAnimation { property: "x"; from: -drawer.width; to: 0; duration: theme.reduceMotion ? 0 : 150; easing.type: Easing.OutCubic } }
        exit: Transition { NumberAnimation { property: "x"; from: 0; to: -drawer.width; duration: theme.reduceMotion ? 0 : 120; easing.type: Easing.InCubic } }
        // Opened from the keyboard or the button, the library is there to
        // find something, so the field that finds it has focus; opened on
        // a folder, the keys are on that folder's row.
        function showPending() {
            libraryPanel.showFolder(window.folderPending)
            window.folderPending = ""
        }
        function takeKeys() {
            if (window.folderPending)
                showPending()
            else
                libraryPanel.focusSearch()
        }
        // A dialog that opened while the library slid in keeps the keys: the
        // folder the library is to show is shown without them, as the
        // docked library shows one, and has them once the dialog closes.
        onOpened: {
            if (!window.dialogOpen)
                takeKeys()
            else if (window.folderPending)
                libraryPanel.showFolder(window.folderPending, false)
        }
        onClosed: {
            const region = window.regionAfterDrawer
            window.regionAfterDrawer = window.textRegion
            window.focusRegion(region)
        }

        // A panel rounded and edged on the side toward the editor. Its other
        // sides lie past the window's edge and the header, where they are
        // cut off, so that it meets both square.
        background: Item {
            clip: true

            Rectangle {
                objectName: "libraryDrawerShape"
                x: -theme.radius - border.width
                y: -border.width
                width: parent.width - x
                height: parent.height + 2 * border.width
                radius: theme.radius
                color: theme.panel
                border.width: theme.controlBorderWidth
                border.color: theme.controlBorder
            }
        }

        T.Overlay.modal: Rectangle { color: theme.scrim }
    }

    // What the library holds: one panel, beside the editor or in the drawer,
    // drawn on the panel colour of either. Opening a document from it, or
    // leaving it with Escape, goes back to the text, closing the drawer.
    LibraryPanel {
        id: libraryPanel
        objectName: "libraryPanel"
        parent: window.libraryDocked ? library : drawer.contentItem
        anchors.fill: parent
        context: window.context
        onOpened: drawer.visible ? drawer.close() : window.focusText()
        onLeave: drawer.visible ? drawer.close() : window.focusText()
    }

    // The window's dialogs. Each gives the keys back as it closes, to the
    // text or to the library open over it, unless another has opened meanwhile.
    ConflictDialog {
        id: conflictDialog
        objectName: "conflictDialog"
        context: window.context
        onClosed: window.dialogClosed()
        onHistoryRequested: (document) => historyDialog.review(document)
    }

    HistoryDialog {
        id: historyDialog
        objectName: "historyDialog"
        onClosed: window.dialogClosed()
    }

    CloseConfirmation {
        id: closeConfirmation
        objectName: "closeConfirmation"
        context: window.context
        onClosed: window.dialogClosed()
        onReview: (document) => conflictDialog.review(document)
    }
}
