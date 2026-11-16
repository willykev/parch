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
                                       || attachmentDialog.visible || linkCheckDialog.visible
                                       || commandPalette.visible || preferencesDialog.visible || shortcutsDialog.visible
                                       || headingsDialog.visible || folderSearchDialog.visible || libraryPanel.dialogOpen
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

    // The formatting toolbar shows while the setting has it shown and the
    // writing area is wide enough for its buttons on one line; a narrower
    // window shows it, wrapped, once asked to.
    readonly property bool toolbarFits: writing.width >= toolbar.naturalWidth
    property bool toolbarAsked: false
    readonly property bool toolbarShown: document !== null && settings.showToolbar && (toolbarFits || toolbarAsked)

    function toggleToolbar() {
        if (toolbarShown) {
            // Hidden here alone where it was only asked for; everywhere otherwise.
            if (toolbarFits)
                settings.setShowToolbar(false)
            toolbarAsked = false
        } else {
            settings.setShowToolbar(true)
            toolbarAsked = !toolbarFits
        }
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
    // field; the notice bar, at its first button; the find bar, at its
    // field; and the text. A library, notice bar or find bar not shown, or a
    // notice bar with no button, is passed over.
    readonly property int libraryRegion: 0
    readonly property int noticeRegion: 1
    readonly property int findRegion: 2
    readonly property int textRegion: 3
    // Where the keys go once the library over the editor has closed.
    property int regionAfterDrawer: textRegion

    function regionOf(item) {
        for (let at = item; at; at = at.parent) {
            if (at === libraryPanel)
                return libraryRegion
            if (at === noticeBar)
                return noticeRegion
            if (at === findBar)
                return findRegion
        }
        return textRegion
    }

    // Gives the keys to the next region shown, or the one before for a step of -1.
    function stepRegion(step) {
        const shown = [library.visible || drawer.visible, noticeBar.reachable, findBar.visible, true]
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
        if (region === findRegion && findBar.visible)
            findBar.focusFind()
        else if (region !== noticeRegion || !noticeBar.focusFirst())
            focusText()
    }

    function showHistory() {
        if (document)
            historyDialog.review(document)
    }

    // The document in front's local links and images that lead nowhere.
    function checkLinks() {
        if (document)
            linkCheckDialog.check(document)
    }

    // Find, and replace, in the document in front, in the bar above the
    // text: opened on the selection when it is one line, with the keys in
    // the find field, or in the replace field once there is something to
    // find.
    function openFind(replacing) {
        if (!document)
            return
        const selected = editorPane.selectedText
        findBar.replacing = replacing
        findBar.visible = true
        documentFind.setSelection(editorPane.anchorSource, editorPane.cursorSource)
        if (selected.length > 0 && selected.indexOf("\u2029") < 0 && selected.indexOf("\n") < 0)
            findBar.setPattern(selected)
        if (replacing && documentFind.pattern.length > 0)
            findBar.focusReplace()
        else
            findBar.focusFind()
    }

    // Closed, the bar looks for nothing and the keys go back to the text.
    function closeFind() {
        findBar.visible = false
        focusText()
    }

    // The next match, or the one before; the bar shows again, without the
    // keys, should it be closed.
    function findStep(forward) {
        if (!document)
            return
        if (!findBar.visible) {
            findBar.visible = true
            documentFind.setSelection(editorPane.anchorSource, editorPane.cursorSource)
        }
        if (documentFind.pattern.length === 0) {
            findBar.focusFind()
            return
        }
        if (forward)
            documentFind.next()
        else
            documentFind.previous()
    }

    function goToHeading() {
        if (document)
            headingsDialog.show(document, editorPane.cursorSource)
    }

    // A heading's line at the top of the view, with the caret on it.
    function showHeading(position) {
        editorPane.showAtTop(position)
        focusText()
    }

    // Search across the library, starting from the selection when it is one line.
    function searchLibrary() {
        const selected = document ? editorPane.selectedText : ""
        folderSearchDialog.show(selected.indexOf("\u2029") < 0 && selected.indexOf("\n") < 0 ? selected : "")
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
        attachmentDialog.close()
        linkCheckDialog.close()
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
        function onViewShowRequested() { window.showView() }
    }

    // After the editor has its width, so that the first line can be found.
    Component.onCompleted: {
        Qt.callLater(window.showView)
        Qt.callLater(window.showFolder)
    }

    // Everything the window can do, from the one list the menu, the
    // palette, the toolbar and the shortcuts all come from.
    readonly property CommandRegistry commands: backend.commands
    // The text of each command's keys, by command id, for the menu and the tooltips.
    readonly property var keys: commands.keys

    // Whether a command can run in this window now: one that edits needs
    // text that takes edits, one about a document needs one in front.
    function canRun(command) {
        switch (commands.needs(command)) {
        case "text":
            return document !== null && editorPane.editable
        case "document":
            return document !== null
        default:
            return true
        }
    }

    // What this window does for the commands whose action is not the
    // application's. A formatting command acts on the editor's selection.
    readonly property var windowActions: ({
        save: () => window.save(),
        history: () => window.showHistory(),
        checkLinks: () => window.checkLinks(),
        findText: () => window.openFind(false),
        replace: () => window.openFind(true),
        findNext: () => window.findStep(true),
        findPrevious: () => window.findStep(false),
        searchLibrary: () => window.act(() => window.searchLibrary()),
        goToHeading: () => window.goToHeading(),
        findDocument: () => window.findDocument(),
        library: () => window.toggleLibrary(),
        toolbar: () => window.toggleToolbar(),
        menu: () => window.openMenu(),
        // Tabs step round the ends, and so do the window's regions.
        nextTab: () => window.stepTab(1),
        previousTab: () => window.stepTab(-1),
        nextRegion: () => window.stepRegion(1),
        previousRegion: () => window.stepRegion(-1),
        undo: () => editorPane.undo(),
        redo: () => editorPane.redo(),
        palette: () => commandPalette.show(),
        preferences: () => preferencesDialog.open(),
        shortcuts: () => shortcutsDialog.show()
    })

    // Runs a command in this window, however it was asked for.
    function run(command) {
        if (!canRun(command) || commands.run(command, window.context))
            return
        const action = windowActions[command]
        if (action) {
            action()
        } else if (editorPane.format(command)) {
            // Chosen from a button or the menu, the text has the keys again.
            window.focusText()
        }
    }

    // Every key of every command is a shortcut of every window, and only the
    // window in front answers it, while none of its dialogs is open. The
    // editor takes its own editing keys, such as Ctrl+Z, while it has focus,
    // so those act here only when it does not: one step for each press.
    Instantiator {
        model: window.commands.bound
        delegate: Shortcut {
            required property string commandId
            required property string keySequence

            sequence: keySequence
            context: Qt.ApplicationShortcut
            enabled: window.shortcutsOn && window.canRun(commandId)
            onActivated: window.run(commandId)
        }
    }

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
        commands: window.commands
        keys: window.keys
        libraryShown: header.libraryShown
        toolbarShown: window.toolbarShown
        canRun: window.canRun
        // Each command is this window's, whichever window the backend last
        // heard was in front. Finding a document gives the keys to the
        // library, so it waits until the menu has closed and given them back
        // to the text.
        onCommandChosen: (command) => command === "findDocument" ? Qt.callLater(window.run, command) : window.run(command)
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

            // Under the tabs, in the layout, its buttons before the editor
            // for Tab; a click on them leaves the keys in the text.
            FormatToolbar {
                id: toolbar
                objectName: "toolbar"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: tabStrip.bottom
                visible: window.toolbarShown
                height: visible ? implicitHeight : 0
                commands: window.commands
                keys: window.keys
                canRun: window.canRun
                onCommandChosen: (command) => window.run(command)
            }

            // Above the editor and in the layout, so it never covers the
            // text. Its buttons come before the editor for Tab, which types
            // in the editor; a click on them leaves the keys in the text.
            NoticeBar {
                id: noticeBar
                objectName: "noticeBar"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: toolbar.bottom
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

            // Find and replace, above the text and in the layout, before the
            // editor so that Tab reaches it from the window's controls.
            FindBar {
                id: findBar
                objectName: "findBar"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: noticeBar.bottom
                height: implicitHeight
                visible: false
                find: documentFind
                keys: window.keys
                onCloseRequested: window.closeFind()
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
                selectionStart: editorPane.anchorPosition
                selectionEnd: editorPane.cursorPosition
            }

            EditorPane {
                id: editorPane
                objectName: "editorPane"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: findBar.bottom
                anchors.bottom: statusLine.top
                visible: window.document !== null
                document: window.document
                title: window.tabs.activeTitle
                maxLineLength: settings.maxLineLength
                fontPixelSize: settings.fontPixelSize
                lineHeight: settings.lineHeight
                fontFamily: settings.editorFontFamily
                setextHeadings: settings.setextHeadings
                inputBlocked: window.dialogOpen
                attachments: backend.attachments
                onIncludeAsked: (name, canInclude) => attachmentDialog.askInclude(name, canInclude)
                onCopyAsked: (names, bytes) => attachmentDialog.askCopy(names, bytes)
                onViewMoved: window.context.noteViewMoved()
                // Escape in the text closes the find bar, or else dismisses a
                // notice that has nothing behind it.
                onEscapePressed: {
                    if (findBar.visible)
                        window.closeFind()
                    else if (backend.notice)
                        backend.dismissNotice()
                }
                // The find follows where the selection is.
                onCursorSourceChanged: documentFind.setSelection(anchorSource, cursorSource)
                onAnchorSourceChanged: documentFind.setSelection(anchorSource, cursorSource)
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
        caret: editorPane.cursorSource
        onOpened: drawer.visible ? drawer.close() : window.focusText()
        onLeave: drawer.visible ? drawer.close() : window.focusText()
        onHeadingChosen: (position) => {
            if (drawer.visible)
                drawer.close()
            window.showHeading(position)
        }
    }

    // What the find bar finds in the document in front, while it is shown.
    DocumentFind {
        id: documentFind
        objectName: "documentFind"
        document: window.document
        active: findBar.visible
        onSelectRequested: (anchor, cursor) => editorPane.selectSource(anchor, cursor)
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

    // What a drop of files on the text asks, answered to the editor that asked.
    AttachmentDialog {
        id: attachmentDialog
        objectName: "attachmentDialog"
        onIncludeChosen: (choice) => editorPane.answerInclude(choice)
        onCopyChosen: (copy) => editorPane.answerCopy(copy)
        onClosed: window.dialogClosed()
    }

    LinkCheckDialog {
        id: linkCheckDialog
        objectName: "linkCheckDialog"
        onClosed: window.dialogClosed()
    }

    CommandPalette {
        id: commandPalette
        objectName: "commandPalette"
        commands: window.commands
        canRun: window.canRun
        runCommand: window.run
        onClosed: window.dialogClosed()
    }

    PreferencesDialog {
        id: preferencesDialog
        objectName: "preferencesDialog"
        onClosed: window.dialogClosed()
    }

    ShortcutsDialog {
        id: shortcutsDialog
        objectName: "shortcutsDialog"
        commands: window.commands
        onClosed: window.dialogClosed()
    }

    HeadingsDialog {
        id: headingsDialog
        objectName: "headingsDialog"
        onChosen: (position) => editorPane.showAtTop(position)
        onClosed: window.dialogClosed()
    }

    FolderSearchDialog {
        id: folderSearchDialog
        objectName: "folderSearchDialog"
        context: window.context
        onClosed: window.dialogClosed()
    }
}
