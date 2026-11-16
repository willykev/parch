import QtQuick
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

    // The advance of "0", the width CSS calls one character, as the editor draws it.
    readonly property real characterWidth: zero.advanceWidth
    readonly property real columnWidth: Math.max(0, Math.min(width - 2 * margin, maxLineLength * characterWidth))
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

    // Where this view is, as source offsets, and putting it back there. The
    // binding keeps positions inside the text and at grapheme boundaries.
    function viewState() {
        return { cursor: binding.cursorSource, anchor: binding.anchorSource, top: binding.topSourcePosition() }
    }
    function restoreView(cursor, anchor, top) { binding.restore(cursor, anchor, top) }

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
        onContentYChanged: pane.viewMoved()
        // A notice bar, the tab strip or the window changes the height: a
        // caret that was in view stays in view.
        property real formerHeight: 0
        onHeightChanged: {
            binding.keepCursorInView(formerHeight)
            formerHeight = height
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
            Accessible.name: pane.title.length > 0 ? pane.title : qsTr("Text")
            color: theme.foreground
            selectionColor: theme.selection
            selectedTextColor: theme.foreground
            renderType: pane.nativeRendering ? TextEdit.NativeRendering : TextEdit.QtRendering
            cursorDelegate: Rectangle {
                objectName: "caret"
                width: Math.max(1, Math.round(pane.fontPixelSize / 8))
                color: theme.accent
                visible: pane.caretFocused && pane.caretLit
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

    EditorBinding {
        id: binding
        objectName: "binding"
        document: pane.document
        editor: editor
        blocked: pane.inputBlocked
        attachments: pane.attachments
        onCursorChanged: {
            pane.viewMoved()
            pane.wakeCaret()
        }
        onActivity: pane.wakeCaret()
        onIncludeAsked: (name, canInclude) => pane.includeAsked(name, canInclude)
        onCopyAsked: (names, bytes) => pane.copyAsked(names, bytes)
    }
}
