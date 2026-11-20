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
    readonly property bool libraryDocked: width - context.libraryWidth >= 45 * editorPane.characterWidth + 2 * editorPane.margin
    // Whether the docked library is shown. The drawer opens only when asked.
    readonly property bool libraryOpen: context.libraryOpen

    // A dialog of the window is open. It has the keys until it closes:
    // nothing moves them out of it, the editor behind it takes no input, and
    // the window's shortcuts wait.
    readonly property bool dialogOpen: conflictDialog.visible || historyDialog.visible || closeConfirmation.visible
                                       || attachmentDialog.visible || checkDocumentDialog.visible
                                       || commandPalette.visible || preferencesDialog.visible || shortcutsDialog.visible
                                       || headingsDialog.visible || folderSearchDialog.visible || libraryPanel.dialogOpen
                                       || encodingDialog.visible
                                       || documentSettingsDialog.visible || exportDialog.visible || openLinkDialog.visible
                                       || exportLinkDialog.visible || exportReplacePrompt.visible
    // The window's shortcuts answer only in the window in front, and not over a dialog.
    readonly property bool shortcutsOn: active && !dialogOpen

    // How the tab in front shows its document: its source alone, the source
    // beside its preview, or the preview alone. Each tab keeps its own.
    readonly property int viewMode: tabs.activeMode
    // Side by side, each pane keeps this much; a window with less room for
    // them shows one pane of a split at a time: the text, or the preview
    // once it is asked for.
    readonly property real minimumPane: Math.round(theme.fontSize * 26)
    readonly property bool splitFits: writing.width >= 2 * minimumPane
    property bool narrowPreview: false
    readonly property bool splitBoth: document !== null && viewMode === TabModel.Split && splitFits
    readonly property bool editorShown: document !== null && (viewMode === TabModel.Source
                                        || (viewMode === TabModel.Split && (splitFits || !narrowPreview)))
    readonly property bool previewShown: document !== null && (viewMode === TabModel.Reading
                                         || (viewMode === TabModel.Split && (splitFits || narrowPreview)))

    // The source at the top of what is shown, and showing it there in
    // whatever is shown now: a view opens where the one before it was.
    function shownTop() {
        return editorShown ? editorPane.viewState().top : previewPane.topSource()
    }
    function showTop(top) {
        if (editorShown)
            editorPane.restoreView(editorPane.cursorSource, editorPane.anchorSource, top)
        if (previewShown)
            previewPane.showSource(top)
    }

    function setViewMode(mode) {
        if (!document || mode === viewMode)
            return
        const top = shownTop()
        // Wherever in the document's panes the keys are, also on the
        // divider or in a table of the preview.
        const region = regionOf(activeFocusItem)
        const keys = !dialogOpen && (region === textRegion || region === previewRegion)
        tabs.activeMode = mode
        showTop(top)
        // The keys stay with the document, in whichever pane now shows it.
        if (keys)
            focusText()
    }

    // The preview beside the text, or the text alone again.
    function togglePreview() {
        setViewMode(viewMode === TabModel.Source ? TabModel.Split : TabModel.Source)
    }

    // In a split, the keys go to the other pane; a split that shows one pane
    // at a time shows the other. From the text alone or the preview alone
    // it is the split that is shown, with the keys in the pane that came.
    function otherPane() {
        if (!document)
            return
        if (viewMode !== TabModel.Split) {
            const reading = viewMode === TabModel.Reading
            const top = shownTop()
            narrowPreview = !reading
            tabs.activeMode = TabModel.Split
            showTop(top)
            if (splitFits && reading)
                editorPane.focusText()
            else if (splitFits)
                previewPane.focusText(true)
            else
                focusText()
            return
        }
        if (splitFits) {
            if (previewKeys)
                editorPane.focusText()
            else
                previewPane.focusText(true)
            return
        }
        const top = shownTop()
        narrowPreview = !narrowPreview
        showTop(top)
        focusText()
    }

    // Shows one pane of a split that shows one at a time.
    function showPane(preview) {
        if (splitFits) {
            if (preview)
                previewPane.focusText()
            else
                editorPane.focusText()
        } else if (narrowPreview !== preview) {
            otherPane()
        } else {
            focusText()
        }
    }

    // What acts on the source needs it in view: the text comes back beside
    // the preview, or alone where there is no room for both.
    function showEditor() {
        if (!document || editorShown)
            return
        if (viewMode === TabModel.Reading)
            setViewMode(splitFits ? TabModel.Split : TabModel.Source)
        else
            otherPane()
    }

    // The outline in the library, shown with the keys on its headings, or hidden.
    property bool outlinePending: false
    function toggleOutline() {
        const shown = libraryDocked ? context.libraryOpen : drawer.visible
        if (settings.outlineShown && shown) {
            settings.outlineShown = false
            return
        }
        settings.outlineShown = true
        if (libraryDocked) {
            if (focusLibrary(""))
                Qt.callLater(libraryPanel.focusOutline)
        } else {
            outlinePending = true
            if (!focusLibrary(""))
                outlinePending = false
            else if (drawer.opened)
                drawer.takeKeys()
        }
    }
    // Focus mode dims all but the sentence or paragraph being written.
    property bool focusMode: false
    // Distraction-free writing shows the text alone: no library, tabs,
    // header, toolbar or status line, the column centred in the whole window.
    property bool distractionFree: false

    // The one way the keys go back to the text: never while a dialog is
    // open. A dialog that closes calls it in turn, so that the keys then go
    // to the text, which may be a tab the dialog's choice opened. Where the
    // preview is shown alone, the keys go to it.
    function focusText() {
        if (dialogOpen)
            return
        if (editorShown || !previewShown)
            editorPane.focusText()
        else
            previewPane.focusText()
    }

    // The one way the keys go to the library, shown beside the editor or
    // over it: to a folder's row when a path is given, and otherwise to its
    // field. Never while a dialog is open, when it returns false and the
    // keys stay with the dialog.
    function focusLibrary(path) {
        if (dialogOpen)
            return false
        // Asking for the library ends distraction-free writing.
        setDistractionFree(false)
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

    // Which of the document's two panes had the keys last: the preview, or
    // the text. A menu or a dialog that closes gives them back to it, and
    // the other pane is the one it is not.
    property bool previewKeys: false
    onActiveFocusItemChanged: {
        for (let at = activeFocusItem; at; at = at.parent) {
            if (at === previewPane) {
                previewKeys = true
                return
            }
            if (at === editorPane) {
                previewKeys = false
                return
            }
        }
    }

    // The keys back to the document as a menu or a dialog closes: to the
    // preview where it had them beside the text, and otherwise as ever.
    function returnKeys() {
        if (!dialogOpen && splitBoth && previewKeys)
            previewPane.focusText()
        else
            focusText()
    }

    // Where the keys go as a dialog closes: to the library over the text,
    // while it is open there, and otherwise to the document.
    function dialogClosed() {
        if (dialogOpen)
            return
        if (drawer.opened)
            drawer.takeKeys()
        else
            returnKeys()
    }

    function toggleFocusMode() {
        focusMode = !focusMode
    }

    // Enters or leaves distraction-free writing, keeping the caret, the
    // selection and the first line in view where they were. The view stays
    // what it was: the text, the preview, or both; the preview keeps its
    // place too, and the keys where it had them.
    function setDistractionFree(on) {
        if (on === distractionFree)
            return
        const view = editorPane.viewState()
        const previewTop = previewShown ? previewPane.topSource() : -1
        const inPreview = previewShown && previewKeys
        if (on && drawer.visible)
            drawer.close()
        distractionFree = on
        Qt.callLater(() => {
            if (window.document && editorPane.document === window.document) {
                editorPane.restoreView(view.cursor, view.anchor, view.top)
                // Beside the text it follows, the preview is brought along.
                if (previewTop >= 0 && window.previewShown && !(window.editorShown && scrollSync.enabled))
                    previewPane.showSource(previewTop)
            }
            if (inPreview && window.previewShown && !window.dialogOpen)
                previewPane.focusText()
            else
                window.focusText()
        })
    }
    function toggleDistractionFree() {
        setDistractionFree(!distractionFree)
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
    // Distraction-free writing hides it too.
    readonly property bool toolbarShown: document !== null && settings.showToolbar && (toolbarFits || toolbarAsked)
                                         && !distractionFree

    function toggleToolbar() {
        // Asking for the toolbar ends distraction-free writing, which shows
        // it again unless it was hidden before.
        if (distractionFree) {
            setDistractionFree(false)
            if (toolbarShown)
                return
        }
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
        const menu = windowMenu.made()
        menu.open()
        menu.currentIndex = 0
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
        // Where the preview is, while it alone shows the document.
        tabs.setView(tabs.activeIndex, view.cursor, view.anchor, editorShown ? view.top : previewPane.topSource())
    }

    // Shows the front tab where its view was.
    function showView() {
        if (tabs.activeIndex < 0 || !document)
            return
        const view = tabs.viewAt(tabs.activeIndex)
        editorPane.restoreView(view.cursor, view.anchor, view.top)
        if (previewShown)
            previewPane.showSource(view.top)
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
            conflictDialog.made().review(document)
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
    // field; the text; and the preview. A region not shown, or a notice bar
    // with no button, is passed over.
    readonly property int libraryRegion: 0
    readonly property int noticeRegion: 1
    readonly property int findRegion: 2
    readonly property int textRegion: 3
    readonly property int previewRegion: 4
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
            if (at === previewPane)
                return previewRegion
        }
        return textRegion
    }

    // Gives the keys to the next region shown, or the one before for a step of -1.
    function stepRegion(step) {
        // With no document the text's place still takes the keys, so that
        // there is always somewhere for them to go.
        const shown = [library.visible || drawer.visible, noticeBar.reachable, findBar.visible,
                       editorShown || !previewShown, previewShown]
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
        else if (region === previewRegion && previewShown && !dialogOpen)
            previewPane.focusText(true)
        else if (region === textRegion && editorShown && !dialogOpen)
            editorPane.focusText()
        else if (region !== noticeRegion || !noticeBar.focusFirst())
            focusText()
    }

    function showHistory() {
        if (document)
            historyDialog.made().review(document)
    }

    // Everything wrong in the document in front, its links and images
    // that lead nowhere among it.
    function checkDocument() {
        if (document)
            checkDocumentDialog.made().check(document)
    }

    // Find, and replace, in the document in front, in the bar above the
    // text: opened on the selection when it is one line, with the keys in
    // the find field, or in the replace field once there is something to
    // find.
    function openFind(replacing) {
        if (!document)
            return
        showEditor()
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

    // Escape in the text or the preview closes the find bar, or else leaves
    // distraction-free writing, or else dismisses a notice that has nothing
    // behind it.
    function escapeInText() {
        if (findBar.visible)
            closeFind()
        else if (distractionFree)
            setDistractionFree(false)
        else if (backend.notice)
            backend.dismissNotice()
    }

    // The next match, or the one before; the bar shows again, without the
    // keys, should it be closed.
    function findStep(forward) {
        if (!document)
            return
        showEditor()
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
            headingsDialog.made().show(document, editorPane.cursorSource)
    }

    // A heading's line at the top of the view, with the caret on it; the
    // preview shows the heading too, when it is not following the text.
    function showHeading(position) {
        editorPane.showAtTop(position)
        if (previewShown && !scrollSync.enabled)
            previewPane.showSource(position)
        focusText()
    }

    // The heading with this slug in the document in front, as a link to
    // another document names one; nothing when it has none.
    function showAnchor(slug) {
        if (document)
            anchorRequest = previewPane.preview.findHeading(slug)
    }
    // The heading showAnchor() asked for last, which is shown once found.
    property int anchorRequest: 0

    Connections {
        target: previewPane.preview
        function onHeadingFound(request, slug, position) {
            if (request === window.anchorRequest && position >= 0)
                window.showHeading(position)
        }
    }

    // Search across the library, starting from the selection when it is one line.
    function searchLibrary() {
        const selected = document ? editorPane.selectedText : ""
        folderSearchDialog.made().show(selected.indexOf("\u2029") < 0 && selected.indexOf("\n") < 0 ? selected : "")
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
        checkDocumentDialog.close()
        documentSettingsDialog.close()
        exportDialog.close()
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

    // Every view of the window that scrolls follows a touchpad swipe from its
    // first move, and glides on after the fingers lift.
    TouchpadScroll {
        objectName: "touchpadScroll"
        target: window
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
        function onCloseConfirmationRequested(document, discarding) { closeConfirmation.made().ask(document, discarding) }
        function onEncodingConfirmationRequested(document) { encodingDialog.made().confirm(document) }
        function onExportLinkQuestionRequested(output, target) { exportLinkDialog.made().ask(output, target) }
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
            // What edits at the caret needs the text in view; undoing and
            // redoing act on the document wherever it is shown.
            return document !== null && editorPane.editable
                    && (editorShown || command === "undo" || command === "redo") && fitsCaret(command)
        case "document":
            return document !== null && fitsDocument(command)
        default:
            return true
        }
    }

    // The commands about a note reference that nothing defines are there
    // while the caret is on one, and superscript and subscript where the
    // dialect reads what they write: after a letter or a digit.
    function fitsCaret(command) {
        if (command !== "inlineNote" && command !== "defineNote" && command !== "superscript" && command !== "subscript")
            return true
        return editorPane.cursorSource >= 0 && editorPane.binding.acts(command)
    }

    // What a command about the document's file needs of it besides being
    // there: to be in the library's folders, or open read-only because it
    // is not UTF-8.
    function fitsDocument(command) {
        switch (command) {
        case "renameDocument":
        case "moveDocument":
        case "trashDocument":
            return backend.organizer.organizes(tabs.activePath)
        case "newDocumentInFolder":
            return tabs.activePath.length > 0 && backend.organizer.libraryOf(tabs.activePath).length > 0
        case "reopenWithEncoding":
            return document.state === Document.ReadOnly && !document.scratch
        default:
            return true
        }
    }

    // What this window does for the commands whose action is not the
    // application's. A formatting command acts on the editor's selection.
    readonly property var windowActions: ({
        save: () => window.save(),
        history: () => window.showHistory(),
        checkLinks: () => window.checkDocument(),
        findText: () => window.openFind(false),
        replace: () => window.openFind(true),
        findNext: () => window.findStep(true),
        findPrevious: () => window.findStep(false),
        searchLibrary: () => window.act(() => window.searchLibrary()),
        goToHeading: () => window.goToHeading(),
        findDocument: () => window.findDocument(),
        library: () => window.toggleLibrary(),
        outline: () => window.toggleOutline(),
        viewSource: () => window.setViewMode(TabModel.Source),
        viewSplit: () => window.setViewMode(TabModel.Split),
        viewReading: () => window.setViewMode(TabModel.Reading),
        togglePreview: () => window.togglePreview(),
        otherPane: () => window.otherPane(),
        scrollSync: () => settings.setScrollSync(!settings.scrollSync),
        previewBack: () => previewPane.preview.goBack(),
        toolbar: () => window.toggleToolbar(),
        // Focus mode and distraction-free writing are each window's own.
        focusMode: () => window.toggleFocusMode(),
        distractionFree: () => window.toggleDistractionFree(),
        menu: () => window.openMenu(),
        // Tabs step round the ends, and so do the window's regions.
        nextTab: () => window.stepTab(1),
        previousTab: () => window.stepTab(-1),
        nextRegion: () => window.stepRegion(1),
        previousRegion: () => window.stepRegion(-1),
        undo: () => editorPane.undo(),
        redo: () => editorPane.redo(),
        palette: () => commandPalette.made().show(),
        preferences: () => preferencesDialog.made().open(),
        documentSettings: () => { if (window.document) documentSettingsDialog.made().show(window.document) },
        "export": () => { if (window.document) exportDialog.made().show(window.document, "export") },
        "print": () => { if (window.document) exportDialog.made().show(window.document, "print") },
        newDocumentInFolder: () => libraryPanel.newDocumentBeside(window.tabs.activePath),
        renameDocument: () => libraryPanel.renameDocument(window.tabs.activePath),
        moveDocument: () => libraryPanel.moveDocument(window.tabs.activePath),
        trashDocument: () => libraryPanel.trashDocument(window.tabs.activePath),
        reopenWithEncoding: () => encodingDialog.made().choose(window.document),
        shortcuts: () => shortcutsDialog.made().show()
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
        visible: !window.distractionFree
        height: visible ? window.rowHeight : 0
        document: window.document
        title: window.tabs.activeTitle
        // parch's own documents, such as its guide, are help, in no folder.
        place: window.tabs.count === 0 ? "" : window.document && window.document.scratch ? qsTr("Help")
               : backend.folderLabel(window.tabs.activePath, settings.libraryFolders)
        libraryWidth: library.visible ? library.width : 0
        libraryShown: library.visible || drawer.visible
        previewShown: window.viewMode !== TabModel.Source
        keys: window.keys
        onToggleLibrary: window.toggleLibrary()
        // The keys go back to the document, in whichever pane now shows it.
        onTogglePreview: {
            window.run("togglePreview")
            window.focusText()
        }
        onMenuRequested: windowMenu.made().open()
    }

    // The window's menu, made the first time it opens.
    LazyPopup {
        id: windowMenu
        owner: window
        component: Component {
            WindowMenu {
                objectName: "windowMenu"
                // The header holds it, or the window while the header is hidden.
                parent: header.visible ? header : window.contentItem
                x: header.width - width - Math.round(theme.fontSize / 4)
                y: header.height
                commands: window.commands
                keys: window.keys
                libraryShown: header.libraryShown
                toolbarShown: window.toolbarShown
                outlineShown: settings.outlineShown && header.libraryShown
                viewMode: window.viewMode
                focusMode: window.focusMode
                distractionFree: window.distractionFree
                spellingWanted: backend.spelling.wanted
                liveFormatting: settings.liveFormatting
                canRun: window.canRun
                // Each command is this window's, whichever window the backend last
                // heard was in front. Finding a document and showing the outline
                // give the keys to the library, so they wait until the menu has
                // closed and given them back to the document.
                onCommandChosen: (command) => command === "findDocument" || command === "outline"
                                              ? Qt.callLater(window.run, command) : window.run(command)
                onClosed: window.returnKeys()
            }
        }
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

            visible: docked && window.libraryOpen && !window.distractionFree
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: window.context.libraryWidth
            color: theme.panel
        }

        // The library's edge, which sets its width in this window: dragged,
        // or moved with the arrow keys once Tab reaches it, never so far that
        // the editor loses the room the library docks beside. The width
        // chosen last is where new windows start.
        LibrarySplitter {
            id: divider
            objectName: "librarySplitter"
            visible: library.visible
            // Over the library and the text, which its grip reaches into.
            z: 1
            anchors.left: library.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: theme.controlBorderWidth
            libraryWidth: window.context.libraryWidth
            minimum: settings.limits.minLibraryWidth
            maximum: Math.max(minimum, Math.min(settings.limits.maxLibraryWidth,
                Math.floor(window.width - 45 * editorPane.characterWidth - 2 * editorPane.margin)))
            onResized: (width) => window.context.libraryWidth = width
            onChosen: (width) => settings.setLibraryWidth(width)
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
                visible: !window.distractionFree && count > 1
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
            // for Tab; a click on them leaves the keys in the text. The
            // preview alone has no text to format: there it is hidden, its
            // room kept, so that what is below stays where it was.
            FormatToolbar {
                id: toolbar
                objectName: "toolbar"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: tabStrip.bottom
                visible: window.toolbarShown && window.viewMode !== TabModel.Reading
                height: window.toolbarShown ? implicitHeight : 0
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
                exportStatus: backend.exports.busy ? backend.exports.status : ""
                onCancelExport: backend.exports.cancel()
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
                noticeUndoable: backend.noticeUndoable
                onUndo: {
                    backend.activate(window.context)
                    backend.undoNotice()
                    window.focusText()
                }
                onReopenAs: {
                    backend.activate(window.context)
                    encodingDialog.made().choose(window.document)
                }
                // Saving asks first, in this window.
                onConfirmEncoding: {
                    backend.activate(window.context)
                    backend.save()
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
                visible: !window.distractionFree
                height: visible ? window.rowHeight : 0
                document: window.document
                status: window.tabs.activeStatus
                problem: backend.notice ? "" : backend.problem
                problemDetail: backend.notice ? "" : backend.shownText(backend.problemDetail)
                selectionStart: editorPane.anchorPosition
                selectionEnd: editorPane.cursorPosition
                syncShown: window.splitBoth
                syncOn: settings.scrollSync
                onSyncToggled: window.run("scrollSync")
                // Vim keys are the text's: no mode is shown where it is not.
                vim: settings.vimKeys && window.editorShown ? editorPane.vim : null
            }

            // The names of a split's two panes, each over its own; a click
            // gives a pane the keys. In a window too narrow for both panes
            // they stand together and choose which of the two is shown.
            // Distraction-free writing hides them with the rest.
            Item {
                id: paneNames
                objectName: "paneNames"
                visible: window.document !== null && window.viewMode === TabModel.Split && !window.distractionFree
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: findBar.bottom
                height: visible ? Math.round(theme.fontSize * 2.2) : 0

                ThemedButton {
                    id: textName
                    objectName: "textPaneName"
                    readonly property bool current: window.splitFits ? editorPane.hasKeys : !window.narrowPreview
                    x: (window.splitFits ? editorPane.x + editorPane.columnLeft : editorPane.margin) - leftPadding
                    anchors.verticalCenter: parent.verticalCenter
                    height: parent.height - 2 * Math.round(theme.fontSize / 4)
                    text: qsTr("Markdown")
                    topPadding: 0
                    bottomPadding: 0
                    font.pixelSize: theme.sizes.small
                    font.capitalization: Font.AllUppercase
                    textColor: current ? theme.foreground : theme.dim
                    Accessible.description: window.splitFits ? qsTr("Gives the keys to the text") : qsTr("Shows the text")
                    onClicked: window.showPane(false)
                }

                ThemedButton {
                    id: previewName
                    objectName: "previewPaneName"
                    readonly property bool current: window.splitFits ? previewPane.hasKeys : window.narrowPreview
                    x: window.splitFits ? previewPane.x + previewPane.columnLeft - leftPadding
                                        : textName.x + textName.width + Math.round(theme.fontSize / 2)
                    anchors.verticalCenter: parent.verticalCenter
                    height: textName.height
                    text: qsTr("Preview")
                    topPadding: 0
                    bottomPadding: 0
                    font.pixelSize: theme.sizes.small
                    font.capitalization: Font.AllUppercase
                    textColor: current ? theme.foreground : theme.dim
                    Accessible.description: window.splitFits ? qsTr("Gives the keys to the preview") : qsTr("Shows the preview")
                    onClicked: window.showPane(true)
                }
            }

            // The document: its text, its preview, or both with a divider
            // between them. The preview and the divider come before the
            // editor for Tab, which types in the editor.
            Item {
                id: panes
                objectName: "panes"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: paneNames.bottom
                anchors.bottom: statusLine.top

                PreviewPane {
                    id: previewPane
                    objectName: "previewPane"
                    x: window.splitBoth ? viewDivider.x + viewDivider.width : 0
                    width: parent.width - x
                    height: parent.height
                    visible: window.previewShown
                    document: window.document
                    title: window.tabs.activeTitle
                    maxLineLength: settings.previewLineLength
                    fontFamily: settings.previewFontFamily
                    fontPixelSize: settings.previewFontPixelSize
                    lineHeight: settings.previewLineHeight
                    // Pictures from the web, for every document or as this one chose.
                    remoteImages: settings.remoteImages
                                  || (backend.documentSettings.revision >= 0 && backend.documentSettings.remotePictures(window.document))
                    // The caret goes to what was double-clicked, and the keys
                    // with it where the text is shown.
                    onSourceRequested: (position) => {
                        editorPane.selectSource(position, position)
                        if (window.editorShown)
                            editorPane.focusText()
                    }
                    onIncludedRequested: (path, line, column) => window.act(() => backend.openAt(path, line, column))
                    // A link to another document opens it in a tab, at the
                    // heading the link names once the tab is in front.
                    // A document that could not be opened leaves this one
                    // where it is, whatever its own headings are called.
                    onOpenRequested: (path, heading) => {
                        const from = window.document
                        window.act(() => backend.openFiles([path]))
                        if (heading.length > 0 && (window.document !== from || window.tabs.activePath === path))
                            Qt.callLater(window.showAnchor, heading)
                    }
                    onLinkAsked: (target, shown, scheme, local) => openLinkDialog.made().ask(target, shown, scheme, local)
                    onEscapePressed: window.escapeInText()
                }

                ViewDivider {
                    id: viewDivider
                    objectName: "viewDivider"
                    visible: window.splitBoth
                    height: parent.height
                    room: parent.width
                    // Neither pane is made narrower than it can be read in.
                    from: Math.min(0.5, window.minimumPane / Math.max(1, parent.width))
                    to: 1 - from
                    value: Math.max(from, Math.min(to, window.tabs.activeSplit))
                    x: Math.round(parent.width * value - width / 2)
                    onMoved: (value) => window.tabs.activeSplit = value
                }

                EditorPane {
                    id: editorPane
                    objectName: "editorPane"
                    anchors.left: parent.left
                    anchors.right: window.splitBoth ? viewDivider.left : parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    visible: window.editorShown
                    document: window.document
                    title: window.tabs.activeTitle
                    maxLineLength: settings.maxLineLength
                    fontPixelSize: settings.fontPixelSize
                    lineHeight: settings.lineHeight
                    fontFamily: settings.editorFontFamily
                    setextHeadings: settings.setextHeadings
                    vimKeys: settings.vimKeys
                    inputBlocked: window.dialogOpen
                    attachments: backend.attachments
                    spellChecker: backend.spelling
                    focusMode: window.focusMode
                    focusUnit: settings.focusUnit
                    focusDimming: settings.focusDimming
                    liveFormatting: settings.liveFormatting
                    onIncludeAsked: (name, canInclude) => attachmentDialog.made().askInclude(name, canInclude)
                    onCopyAsked: (names, bytes) => attachmentDialog.made().askCopy(names, bytes)
                    // Where the view is goes into the session, but not in a
                    // document of parch's own, which the session never keeps.
                    onViewMoved: if (!window.document || !window.document.scratch) window.context.noteViewMoved()
                    // :w saves and :q closes the tab, as their commands do.
                    onVimCommand: (command) => window.run(command)
                    onEscapePressed: window.escapeInText()
                    // The find follows where the selection is.
                    onCursorSourceChanged: documentFind.setSelection(anchorSource, cursorSource)
                    onAnchorSourceChanged: documentFind.setSelection(anchorSource, cursorSource)
                }
            }

            // Keeps the text and the preview beside it at the same place.
            ScrollSync {
                id: scrollSync
                objectName: "scrollSync"
                preview: previewPane.preview
                binding: editorPane.binding
                editorViewport: editorPane.viewport
                enabled: window.splitBoth && settings.scrollSync
            }

            // In place of the editor, for a tab whose file is not there: what
            // the tab says and the file it looked for.
            Item {
                id: missingFile
                objectName: "missingFile"
                visible: window.document === null && window.tabs.count > 0
                anchors.fill: panes

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
        width: Math.min(window.context.libraryWidth, window.width - window.rowHeight)
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
            if (window.folderPending) {
                showPending()
            } else if (window.outlinePending) {
                window.outlinePending = false
                libraryPanel.focusOutline()
            } else {
                libraryPanel.focusSearch()
            }
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
        onIncludedHeadingChosen: (path, line) => {
            if (drawer.visible)
                drawer.close()
            window.act(() => backend.openAt(path, line, 0))
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

    // The window's menu or one of its dialogs by its object name, made now
    // if it was not yet; null for any other name.
    function popupNamed(name) {
        const popups = {
            windowMenu: windowMenu, conflictDialog: conflictDialog, historyDialog: historyDialog,
            encodingDialog: encodingDialog, closeConfirmation: closeConfirmation, attachmentDialog: attachmentDialog,
            linkDialog: openLinkDialog, checkDocumentDialog: checkDocumentDialog, commandPalette: commandPalette,
            preferencesDialog: preferencesDialog, documentSettingsDialog: documentSettingsDialog,
            exportDialog: exportDialog, exportLinkDialog: exportLinkDialog, exportReplacePrompt: exportReplacePrompt,
            shortcutsDialog: shortcutsDialog, headingsDialog: headingsDialog, folderSearchDialog: folderSearchDialog
        }
        return popups[name] ? popups[name].made() : null
    }

    // The window's dialogs, each made the first time it is asked for. Each
    // gives the keys back as it closes, to the text or to the library open
    // over it, unless another has opened meanwhile.
    LazyPopup {
        id: conflictDialog
        owner: window
        component: Component {
            ConflictDialog {
                objectName: "conflictDialog"
                context: window.context
                onClosed: window.dialogClosed()
                onHistoryRequested: (document) => historyDialog.made().review(document)
            }
        }
    }

    LazyPopup {
        id: historyDialog
        owner: window
        component: Component {
            HistoryDialog {
                objectName: "historyDialog"
                onClosed: window.dialogClosed()
            }
        }
    }

    // Reopen as…, for a file that is not UTF-8, and the question before
    // such a file is first written as UTF-8.
    LazyPopup {
        id: encodingDialog
        owner: window
        component: Component {
            EncodingDialog {
                objectName: "encodingDialog"
                onClosed: window.dialogClosed()
            }
        }
    }

    LazyPopup {
        id: closeConfirmation
        owner: window
        component: Component {
            CloseConfirmation {
                objectName: "closeConfirmation"
                context: window.context
                onClosed: window.dialogClosed()
                onReview: (document) => conflictDialog.made().review(document)
            }
        }
    }

    // What a drop of files on the text asks, answered to the editor that asked.
    LazyPopup {
        id: attachmentDialog
        owner: window
        component: Component {
            AttachmentDialog {
                objectName: "attachmentDialog"
                onIncludeChosen: (choice) => editorPane.answerInclude(choice)
                onCopyChosen: (copy) => editorPane.answerCopy(copy)
                onClosed: window.dialogClosed()
            }
        }
    }

    // What a link that leads out of parch asks before it is followed.
    LazyPopup {
        id: openLinkDialog
        owner: window
        component: Component {
            OpenLinkDialog {
                objectName: "linkDialog"
                onClosed: window.dialogClosed()
            }
        }
    }

    LazyPopup {
        id: checkDocumentDialog
        owner: window
        component: Component {
            CheckDocumentDialog {
                objectName: "checkDocumentDialog"
                onClosed: window.dialogClosed()
            }
        }
    }

    LazyPopup {
        id: commandPalette
        owner: window
        component: Component {
            CommandPalette {
                objectName: "commandPalette"
                commands: window.commands
                canRun: window.canRun
                runCommand: window.run
                onClosed: window.dialogClosed()
            }
        }
    }

    LazyPopup {
        id: preferencesDialog
        owner: window
        component: Component {
            PreferencesDialog {
                objectName: "preferencesDialog"
                onClosed: window.dialogClosed()
            }
        }
    }

    LazyPopup {
        id: documentSettingsDialog
        owner: window
        component: Component {
            DocumentSettingsDialog {
                objectName: "documentSettingsDialog"
                onClosed: window.dialogClosed()
            }
        }
    }

    LazyPopup {
        id: exportDialog
        owner: window
        component: Component {
            ExportDialog {
                objectName: "exportDialog"
                context: window.context
                onClosed: window.dialogClosed()
            }
        }
    }

    // What an export to a file that is a link asks.
    LazyPopup {
        id: exportLinkDialog
        owner: window
        component: Component {
            ExportLinkDialog {
                objectName: "exportLinkDialog"
                onClosed: window.dialogClosed()
            }
        }
    }

    // An export the file dialog was given a name for without its suffix
    // would write over the file of that name with it, which the file dialog
    // never asked about; this asks, with Cancel in front.
    LazyPopup {
        id: exportReplacePrompt
        owner: window
        component: Component {
            ThemedDialog {
                id: prompt
                objectName: "exportReplacePrompt"

                property string path
                property bool answered: false
                readonly property string name: path.substring(path.lastIndexOf("/") + 1)

                function ask(path) {
                    prompt.path = path
                    prompt.answered = false
                    open()
                }

                function answer(replace) {
                    prompt.answered = true
                    backend.exports.answerReplace(replace)
                    close()
                }

                title: qsTr("Replace “%1”?").arg(prompt.name)
                preferredCharacters: 52
                initialFocus: exportReplaceCancel
                onClosed: {
                    if (!prompt.answered)
                        backend.exports.answerReplace(false)
                    window.dialogClosed()
                }

                contentItem: Text {
                    text: qsTr("“%1” is already in %2. Exporting writes over it.")
                              .arg(prompt.name).arg(backend.folderLabel(prompt.path))
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    color: theme.foreground
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                footer: ThemedDialog.Choices {
                    ThemedDialog.Choice {
                        objectName: "exportReplace"
                        text: qsTr("Replace")
                        textColor: theme.danger
                        onClicked: prompt.answer(true)
                    }
                    ThemedDialog.Choice {
                        id: exportReplaceCancel
                        objectName: "exportReplaceCancel"
                        text: qsTr("Cancel")
                        onClicked: prompt.answer(false)
                    }
                }
            }
        }
    }

    Connections {
        target: backend.exports
        function onReplaceAsked(asker, path) {
            if (asker === window.context)
                exportReplacePrompt.made().ask(path)
        }
    }

    LazyPopup {
        id: shortcutsDialog
        owner: window
        component: Component {
            ShortcutsDialog {
                objectName: "shortcutsDialog"
                commands: window.commands
                onClosed: window.dialogClosed()
            }
        }
    }

    LazyPopup {
        id: headingsDialog
        owner: window
        component: Component {
            HeadingsDialog {
                objectName: "headingsDialog"
                onChosen: (position) => window.showHeading(position)
                onIncludedChosen: (path, line) => window.act(() => backend.openAt(path, line, 0))
                onClosed: window.dialogClosed()
            }
        }
    }

    LazyPopup {
        id: folderSearchDialog
        owner: window
        component: Component {
            FolderSearchDialog {
                objectName: "folderSearchDialog"
                context: window.context
                onClosed: window.dialogClosed()
            }
        }
    }
}
