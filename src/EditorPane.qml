import QtQuick
import QtQuick.Shapes
import QtQuick.Window
import Parch

// The writing surface for one view of a document: one TextEdit over the whole
// text in a clipped Flickable, centred in a column at most maxLineLength
// characters wide. The EditorBinding keeps the TextEdit a projection of the
// document's source and turns every input into an edit of that source, so this
// file only lays the text out and draws it, every colour from the theme.
Rectangle {
    id: pane

    property Document document: null
    // The name its tab shows, which assistive technology hears for the text:
    // a draft standing for a file not made yet has that file's name. Alone,
    // the pane is named as its document is.
    property string title: document ? document.title : ""
    // Characters per line at most; a wider pane widens the margins, not the column.
    property int maxLineLength: 72
    // The window passes its writing settings in; these defaults let the pane stand alone.
    property int fontPixelSize: theme.fontSize + 4
    property real lineHeight: 1.5
    property string fontFamily: theme.fontFamily
    // While the window shows a dialog the text takes no keys or input, even
    // should it be given the focus.
    property bool inputBlocked: false
    // Where pasted images and dropped files are placed; without it, a drop
    // of files is refused.
    property Attachments attachments: null

    // A dropped Markdown file can be linked or included, and large files
    // copied: the window asks, and answers through answerInclude() and answerCopy().
    signal includeAsked(string name, bool canInclude)
    signal copyAsked(var names, real bytes)
    function answerInclude(choice) { binding.answerInclude(choice) }
    function answerCopy(copy) { binding.answerCopy(copy) }
    // The heading commands underline levels one and two rather than marking them with #.
    property bool setextHeadings: false
    // The text takes Vim keys; their mode and command line are `vim`'s.
    property bool vimKeys: false
    readonly property VimMode vim: binding.vim
    // Outside Insert mode the caret is a block on the character it is on.
    readonly property bool blockCaret: vimKeys && vim.mode !== VimMode.Insert
    // A window command Vim keys ask for, such as :w's "save".
    signal vimCommand(string command)
    // The view's selection, from anchor to cursor, as source offsets, and
    // whether the text takes edits.
    readonly property int cursorPosition: binding.cursorSource
    readonly property int anchorPosition: binding.anchorSource
    readonly property bool editable: binding.editable
    // The application's spelling, which underlines the words it does not
    // know; none, alone, checks nothing.
    property SpellChecker spellChecker: null
    // Focus mode dims all but the sentence or the paragraph with the caret.
    property bool focusMode: false
    property string focusUnit: "sentence"
    // How strongly: "off", "light" or "strong".
    property string focusDimming: "strong"
    // Live formatting styles the Markdown as it is written, its punctuation
    // kept and dimmed; without it the source is plain text.
    property bool liveFormatting: true

    // The advance of "0", the width CSS calls one character, as the editor draws it.
    readonly property real characterWidth: zero.advanceWidth
    readonly property real columnWidth: Math.max(0, Math.min(width - 2 * margin, maxLineLength * characterWidth))
    // Where the column starts, from the pane's left edge.
    readonly property real columnLeft: Math.round((width - columnWidth) / 2)
    // The text has the keys.
    readonly property bool hasKeys: editor.activeFocus
    // What keeps the text a projection of its source, and what it scrolls
    // in: for the preview beside it, which scrolls with it.
    readonly property alias binding: binding
    readonly property alias viewport: flickable
    // Laying out a document longer than this takes long enough to drop frames,
    // so while the column is being resized its new width waits until the width
    // has settled.
    readonly property int largeDocumentUnits: 100000
    readonly property int rewrapSettleMs: 80
    // The space beside the column at its narrowest, and above and below the text.
    readonly property real margin: 24
    // Native rendering hints glyphs to the pixel grid, which is crisp at whole
    // device-pixel ratios and uneven at fractional ones, where Qt's own
    // renderer is used instead.
    readonly property bool nativeRendering: Screen.devicePixelRatio % 1 === 0

    color: theme.background

    // Applies the font, line height and column width together and keeps the
    // first visible line where it was.
    function applyLayout() {
        settle.stop()
        if (editor.width === columnWidth && editor.font.pixelSize === fontPixelSize
                && editor.font.family === fontFamily && binding.lineHeight === lineHeight)
            return
        const top = binding.topSourcePosition()
        editor.font.family = fontFamily
        editor.font.pixelSize = fontPixelSize
        binding.lineHeight = lineHeight
        editor.width = columnWidth
        binding.restore(binding.cursorSource, binding.anchorSource, top)
    }

    // The window reaches this view's undo and focus through these, so that a
    // key or a menu acts on this view wherever focus is.
    function undo() { binding.undo() }
    function redo() { binding.redo() }
    function focusText() { editor.forceActiveFocus() }
    // A formatting command, by id, on this view's selection.
    function format(command) { return binding.format(command) }

    // Where this view is, as source offsets, and putting it back there. The
    // binding keeps positions inside the text and at grapheme boundaries.
    function viewState() {
        return { cursor: binding.cursorSource, anchor: binding.anchorSource, top: binding.topSourcePosition() }
    }
    function restoreView(cursor, anchor, top) { binding.restore(cursor, anchor, top) }

    // The selection in source offsets, and selecting there, scrolled only as
    // far as shows the caret, as a find does; or with the line at the top of
    // the view, as going to a heading does.
    readonly property int cursorSource: binding.cursorSource
    readonly property int anchorSource: binding.anchorSource
    readonly property string selectedText: editor.selectedText
    function selectSource(anchor, cursor) { binding.select(anchor, cursor) }
    function showAtTop(position) { binding.restore(position, position, position) }

    // Opens the spelling menu on the misspelled word at a position of the
    // editor's, at a point of it; false when no misspelled word is there.
    function offerSpelling(position, point) {
        const found = spelling.misspellingAt(binding.editorToSource(position))
        if (found.word === undefined || !spellChecker)
            return false
        spellingMenu.offer(found.start, found.end, found.word, spellChecker.suggestions(found.word))
        spellingMenu.popup(editor, point.x, point.y)
        return true
    }

    // Opens the menu on a note reference that nothing defines, at a
    // position of the editor's; false when there is none there.
    function offerNote(position, point) {
        const source = binding.editorToSource(position)
        const label = binding.undefinedNoteAt(source)
        if (label.length === 0)
            return false
        noteMenu.position = source
        noteMenu.label = label
        noteMenu.inline = binding.acts("inlineNote", source)
        noteMenu.popup(editor, point.x, point.y)
        return true
    }

    // The cursor moved or the text scrolled.
    signal viewMoved()
    // Escape was pressed in the text, with nothing being composed.
    signal escapePressed()

    // The caret is parch's own, and Qt's text control never blinks one: its
    // timer would draw the whole editor again at every blink. The caret
    // blinks at the desktop's cursor flash time, and stays lit when that is
    // 0, when motion is reduced, and once the writer has paused for
    // caretRestMs, after which a window left alone draws nothing at all.
    // Typing, moving the caret, a click or the focus lights it again at once.
    property int caretRestMs: CaretRest.RestMs
    readonly property bool caretFocused: editor.activeFocus && !editor.readOnly
    readonly property bool caretBlinks: !theme.reduceMotion && Qt.styleHints.cursorFlashTime >= 2
    property bool caretLit: true
    function wakeCaret() {
        caretLit = true
        if (caretFocused && caretBlinks) {
            caretBlink.restart()
            caretRest.restart()
        } else {
            caretBlink.stop()
            caretRest.stop()
        }
    }
    onCaretFocusedChanged: wakeCaret()
    onCaretBlinksChanged: wakeCaret()

    Timer {
        id: caretBlink
        objectName: "caretBlink"
        interval: Math.max(1, Qt.styleHints.cursorFlashTime / 2)
        repeat: true
        onTriggered: pane.caretLit = !pane.caretLit
    }
    Timer {
        id: caretRest
        objectName: "caretRest"
        interval: pane.caretRestMs
        onTriggered: {
            caretBlink.stop()
            pane.caretLit = true
        }
    }

    onColumnWidthChanged: {
        // The first width is applied at once: a document waiting for it is not
        // being re-wrapped, and it is not laid out at all until it has one.
        if (editor.width > 0 && document && document.buffer.length > largeDocumentUnits)
            settle.restart()
        else
            applyLayout()
    }
    // A new font changes the column width too, so both are applied once, after
    // the width has followed the font.
    onFontPixelSizeChanged: Qt.callLater(applyLayout)
    onFontFamilyChanged: Qt.callLater(applyLayout)
    onLineHeightChanged: Qt.callLater(applyLayout)
    Component.onCompleted: applyLayout()

    TextMetrics {
        id: zero
        text: "0"
        font.family: pane.fontFamily
        font.pixelSize: pane.fontPixelSize
        renderType: pane.nativeRendering ? Text.NativeRendering : Text.QtRendering
    }

    Timer {
        id: settle
        objectName: "settle"
        interval: pane.rewrapSettleMs
        onTriggered: pane.applyLayout()
    }

    Flickable {
        id: flickable
        objectName: "flickable"
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: editor.y + editor.height + pane.margin
        // Dragging with the mouse selects text; the wheel and touch still scroll.
        acceptedButtons: Qt.NoButton
        boundsBehavior: Flickable.StopAtBounds
        onContentYChanged: {
            pane.viewMoved()
            decorations.viewMoved()
        }
        // A notice bar, the tab strip or the window changes the height: a
        // caret that was in view stays in view.
        property real formerHeight: 0
        onHeightChanged: {
            binding.keepCursorInView(formerHeight)
            formerHeight = height
            decorations.viewMoved()
        }

        TextEdit {
            id: editor
            objectName: "editor"
            x: Math.round((flickable.width - width) / 2)
            y: pane.margin
            // Clicking below the text still reaches the editor.
            height: Math.max(implicitHeight, flickable.height - 2 * pane.margin)
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.Wrap
            readOnly: !binding.editable
            selectByMouse: true
            // A document that cannot be edited can still be selected and copied from the keyboard.
            selectByKeyboard: true
            persistentSelection: true
            focus: true
            // Tab reaches the text from the window's controls; inside it, Tab types.
            activeFocusOnTab: true
            // The binding takes Escape first while an input method composes.
            Keys.onEscapePressed: pane.escapePressed()
            // The Menu key, or Shift+F10, opens the spelling menu on the word at the caret.
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && event.modifiers === Qt.ShiftModifier)) {
                    const caret = editor.cursorRectangle
                    const point = Qt.point(caret.x, caret.y + caret.height)
                    event.accepted = pane.offerSpelling(editor.cursorPosition, point) || pane.offerNote(editor.cursorPosition, point)
                }
            }
            Accessible.name: pane.title.length > 0 ? pane.title : qsTr("Text")
            // Focus mode draws the text dim and brightens the focus.
            color: decorations.focusing ? focus.dim : theme.foreground
            selectionColor: theme.selection
            selectedTextColor: theme.foreground
            renderType: pane.nativeRendering ? TextEdit.NativeRendering : TextEdit.QtRendering
            cursorDelegate: Rectangle {
                objectName: "caret"
                width: Math.max(1, Math.round(pane.fontPixelSize / 8))
                color: theme.accent
                visible: pane.caretFocused && pane.caretLit && !pane.blockCaret
            }

            // Vim's block caret, over the character the cursor is on, or
            // a character wide at the end of a line. It follows the layout
            // as the cursor's rectangle does.
            Rectangle {
                id: block
                objectName: "blockCaret"
                // A rectangle only for a position the text shows: while an
                // edit is being followed, the source can be ahead of it.
                function boxAt(source) {
                    const shown = binding.sourceToEditor(source)
                    return shown < 0 ? Qt.rect(0, 0, 0, 0) : editor.positionToRectangle(Math.min(shown, editor.length))
                }
                // Worked out only while the block shows, so that typing
                // without it costs nothing here.
                readonly property rect from: {
                    if (!pane.blockCaret)
                        return Qt.rect(0, 0, 0, 0)
                    editor.cursorRectangle
                    editor.width
                    editor.font
                    return boxAt(pane.vim.cursor)
                }
                readonly property rect to: {
                    if (!pane.blockCaret)
                        return Qt.rect(0, 0, 0, 0)
                    editor.cursorRectangle
                    editor.width
                    editor.font
                    return boxAt(pane.vim.cursorEnd)
                }
                visible: pane.blockCaret && pane.caretFocused && pane.caretLit
                x: from.x
                y: from.y
                width: to.y === from.y && to.x > from.x ? to.x - from.x : pane.characterWidth
                height: from.height
                color: theme.accent
                opacity: 0.45
            }

            // The TextEdit's own editing methods edit the source, as typing does.
            function paste() { binding.paste() }
            function cut() { binding.cut() }
            function copy() { binding.copy() }
            function undo() { binding.undo() }
            function redo() { binding.redo() }
            function insert(position: int, text: string) { binding.replaceEditorRange(position, position, text) }
            function remove(start: int, end: int) { binding.replaceEditorRange(start, end, "") }
            function append(text: string) { binding.appendParagraph(text) }
            function clear() { binding.replaceEditorRange(0, length, "") }

            // Where a drop would land, while something is dragged over the text.
            Rectangle {
                id: dropMark
                objectName: "dropMark"
                visible: false
                width: Math.max(1, Math.round(pane.fontPixelSize / 8))
                color: theme.accent

                function place(x, y) {
                    const line = editor.positionToRectangle(editor.positionAt(x, y))
                    dropMark.x = line.x
                    dropMark.y = line.y
                    dropMark.height = line.height
                    dropMark.visible = true
                }
            }

            // A right click on a misspelled word opens the spelling menu, and
            // on a note reference that nothing defines the note's; a left
            // one goes to the text as ever.
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: (point) => {
                    const position = editor.positionAt(point.position.x, point.position.y)
                    if (!pane.offerSpelling(position, point.position))
                        pane.offerNote(position, point.position)
                }
            }

            // The wavy underlines in and near the view, which Qt would draw
            // straight, drawn here where the text has them.
            Repeater {
                model: decorations.waves

                delegate: Shape {
                    id: wave

                    required property real markX
                    required property real markY
                    required property real markWidth
                    required property real markHeight
                    required property color markColor

                    objectName: "wave"
                    x: markX
                    y: markY
                    width: markWidth
                    height: markHeight

                    ShapePath {
                        strokeColor: wave.markColor
                        strokeWidth: Math.max(1, Math.round(pane.fontPixelSize / 14))
                        fillColor: "transparent"
                        joinStyle: ShapePath.RoundJoin
                        PathPolyline {
                            path: {
                                const points = []
                                const step = Math.max(2, wave.height)
                                for (let x = 0, up = false; x < wave.width + step; x += step, up = !up)
                                    points.push(Qt.point(Math.min(x, wave.width), up ? 0 : wave.height))
                                return points
                            }
                        }
                    }
                }
            }
        }
    }

    // Files, rich text, links and text dragged in from other applications.
    // Files are always copied or linked, never moved, whatever the source proposes.
    DropArea {
        id: dropArea
        objectName: "dropArea"
        anchors.fill: flickable
        enabled: binding.editable && !pane.inputBlocked
        onEntered: (drag) => {
            if (drag.hasUrls || drag.hasHtml || drag.hasText)
                drag.accept(Qt.CopyAction)
            else
                drag.accepted = false
        }
        onPositionChanged: (drag) => {
            const point = editor.mapFromItem(dropArea, drag.x, drag.y)
            dropMark.place(point.x, point.y)
        }
        onExited: dropMark.visible = false
        onDropped: (drop) => {
            dropMark.visible = false
            const point = editor.mapFromItem(dropArea, drop.x, drop.y)
            if (binding.dropAt(point.x, point.y, drop.hasUrls ? drop.urls : [], drop.hasHtml ? drop.html : "",
                               drop.hasText ? drop.text : ""))
                drop.accept(Qt.CopyAction)
            else
                drop.accepted = false
        }
    }

    // What the view draws over its text: the layers of formatting the
    // window's tools add, and the focus.
    EditorDecorations {
        id: decorations
        objectName: "decorations"
        binding: binding
    }

    // The Markdown, styled from the parsed text on a worker: headings,
    // emphasis, links, code, math and the rest, every colour the theme's.
    MarkdownStyling {
        id: styling
        objectName: "styling"
        decorations: decorations
        enabled: pane.liveFormatting
        font: editor.font
        foreground: theme.foreground
        background: theme.background
        markerColor: theme.dim
        linkColor: theme.accent
        codeColor: theme.good
        codeFill: theme.raised
        mathColor: theme.warning
        markColor: theme.warning
    }

    SpellingLayer {
        id: spelling
        objectName: "spelling"
        decorations: decorations
        checker: pane.spellChecker
        color: theme.danger
    }

    FocusMode {
        id: focus
        objectName: "focus"
        decorations: decorations
        enabled: pane.focusMode
        unit: pane.focusUnit
        background: theme.background
        strength: pane.focusDimming
        normal: theme.foreground
    }

    // The menu on a note reference that nothing defines: the note made
    // inline, or a definition for it at the end, each one step to undo.
    ThemedMenu {
        id: noteMenu
        objectName: "noteMenu"

        // The reference's place in the source, and its label.
        property int position: 0
        property string label
        // The dialect has inline notes.
        property bool inline: false

        function run(command) {
            binding.select(noteMenu.position, noteMenu.position)
            binding.format(command)
        }

        title: qsTr("Note “%1”").arg(noteMenu.label)
        onOpened: noteMenu.currentIndex = 0
        onClosed: editor.forceActiveFocus()

        MenuEntry {
            objectName: "inlineNoteEntry"
            text: qsTr("Turn into an inline note")
            enabled: noteMenu.inline
            onTriggered: noteMenu.run("inlineNote")
        }
        MenuEntry {
            objectName: "defineNoteEntry"
            text: qsTr("Add a definition for the note")
            onTriggered: noteMenu.run("defineNote")
        }
    }

    SpellingMenu {
        id: spellingMenu
        objectName: "spellingMenu"
        onReplace: (start, end, word, replacement) => spelling.replaceWord(start, end, word, replacement)
        onAddWord: (word) => pane.spellChecker.addToDictionary(word)
        onIgnoreWord: (word) => pane.spellChecker.ignore(word)
        onClosed: editor.forceActiveFocus()
    }

    EditorBinding {
        id: binding
        objectName: "binding"
        document: pane.document
        editor: editor
        blocked: pane.inputBlocked
        attachments: pane.attachments
        setextHeadings: pane.setextHeadings
        vimKeys: pane.vimKeys
        onCursorChanged: {
            pane.viewMoved()
            pane.wakeCaret()
        }
        onActivity: pane.wakeCaret()
        onIncludeAsked: (name, canInclude) => pane.includeAsked(name, canInclude)
        onCopyAsked: (names, bytes) => pane.copyAsked(names, bytes)
    }

    Connections {
        target: binding.vim
        function onCommandRequested(command) { pane.vimCommand(command) }
        // A key in Normal or Visual mode lights the caret as typing does.
        function onCursorChanged() { pane.wakeCaret() }
    }
}
