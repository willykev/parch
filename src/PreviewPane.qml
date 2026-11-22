import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Window
import Parch

// The reading surface for one view of a document: what the document reads
// as, in a column at most maxLineLength characters wide, centred in the
// pane. A Preview builds the text from the Markdown engine's tree into one
// read-only TextEdit, which can be selected and copied from, and names what
// is placed over it: the bars beside quotes, rules, and the tables, code
// blocks and display formulas, each of which scrolls sideways by itself
// when it is wider than the column. Every colour is the theme's.
Rectangle {
    id: pane

    property Document document: null
    // The name its tab shows, which assistive technology hears for the preview.
    property string title: document ? document.title : ""
    // Nothing is parsed or built while the pane is not shown.
    property bool active: visible
    // Characters per line at most; a wider pane widens the margins, not the column.
    property int maxLineLength: 68
    // The window passes its reading settings in; these defaults let the pane stand alone.
    property string fontFamily: ""
    property int fontPixelSize: theme.fontSize + 6
    property real lineHeight: 1.6
    property bool remoteImages: false

    readonly property alias preview: preview
    readonly property alias viewport: flickable
    readonly property alias surface: surface
    // The space beside the column at its narrowest, and above and below the text.
    readonly property real margin: 24
    readonly property real columnWidth: Math.max(0, Math.min(width - 2 * margin, maxLineLength * zero.advanceWidth))
    // Where the column starts, from the pane's left edge.
    readonly property real columnLeft: Math.round((width - columnWidth) / 2)
    // The keys came to the preview from the keyboard rather than a click:
    // then it shows that it has them.
    property bool keyFocus: false
    readonly property bool hasKeys: surface.activeFocus
    // The application's backend and theme, which the preview is handed:
    // inside it, the names are its own properties'.
    readonly property Backend application: backend
    readonly property Theme look: theme

    // A double click asks for the editor's caret at this source offset; in
    // text an include brought in, for the included file at that place.
    signal sourceRequested(int position)
    signal includedRequested(string path, int line, int column)
    function requestSource(position, island) {
        const included = preview.includedAt(position, island)
        if (included.path !== undefined) {
            pane.includedRequested(included.path, included.line, included.column)
            return
        }
        const source = preview.sourceAt(position, island)
        if (source >= 0)
            pane.sourceRequested(source)
    }
    // A link names a document to open, and perhaps a heading of it by its slug.
    signal openRequested(string path, string heading)
    // A link leads out of parch in a way that is asked about first, as Preview's linkAsked().
    signal linkAsked(string target, string shown, string scheme, bool local, string origin, bool userInfo)
    signal escapePressed()

    // Gives the keys to the preview; `byKeys` when a key asked for it, as
    // moving between the parts of the window does.
    property bool quietFocus: false
    function focusText(byKeys) {
        quietFocus = true
        surface.forceActiveFocus()
        quietFocus = false
        keyFocus = byKeys === true
    }
    // The source at the top of the view, and showing a source offset there.
    function topSource() { return preview.topSource() }
    function showSource(position) { preview.showSource(position) }
    function scrollBy(distance) {
        const most = Math.max(0, flickable.contentHeight - flickable.height)
        flickable.contentY = Math.max(0, Math.min(most, flickable.contentY + distance))
    }
    // Keeps a rectangle of the surface in view, by the least scroll that shows it.
    function reveal(rectangle) {
        const top = surface.y + rectangle.y, bottom = top + rectangle.height
        if (top < flickable.contentY)
            flickable.contentY = Math.max(0, top - pane.margin)
        else if (bottom > flickable.contentY + flickable.height)
            flickable.contentY = bottom - flickable.height + pane.margin
    }

    color: theme.background

    Preview {
        id: preview
        objectName: "preview"
        document: pane.document
        backend: pane.application
        theme: pane.look
        active: pane.active
        surface: surface
        viewport: flickable
        family: pane.fontFamily
        fontSize: pane.fontPixelSize
        lineHeight: pane.lineHeight
        scale: Screen.devicePixelRatio
        remoteImages: pane.remoteImages
        onOpenRequested: (path, heading) => pane.openRequested(path, heading)
        onLinkAsked: (target, shown, scheme, local, origin, userInfo) => pane.linkAsked(target, shown, scheme, local, origin, userInfo)
    }

    // The advance of "0" in the reading face, the width CSS calls one character.
    TextMetrics {
        id: zero
        text: "0"
        font.family: preview.bodyFamily
        font.pixelSize: preview.bodySize
    }

    // A quiet row over the text when the document names pictures on the
    // web that are not shown: they are fetched only once the writer asks,
    // for this document here, or for every document in Preferences.
    Rectangle {
        id: webNotice
        objectName: "webPicturesNotice"
        readonly property real pad: Math.round(theme.fontSize / 2)
        visible: pane.document !== null && preview.heldWebPictures
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: visible ? Math.max(webText.implicitHeight, showWeb.implicitHeight) + 2 * pad : 0
        color: theme.panel
        Accessible.role: Accessible.StaticText
        Accessible.name: webText.text

        Text {
            id: webText
            objectName: "webPicturesText"
            anchors.left: parent.left
            anchors.leftMargin: pane.margin
            anchors.right: showWeb.left
            anchors.rightMargin: webNotice.pad
            anchors.verticalCenter: parent.verticalCenter
            text: preview.hasApprovedHosts ? qsTr("This document shows pictures from places it did not before.")
                                           : qsTr("This document shows pictures from the web.")
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.small
            color: theme.dim
            elide: Text.ElideRight
            Accessible.ignored: true
        }

        ThemedButton {
            id: showWeb
            objectName: "showWebPictures"
            anchors.right: parent.right
            anchors.rightMargin: pane.margin
            anchors.verticalCenter: parent.verticalCenter
            framed: true
            text: qsTr("Show them")
            font.pixelSize: theme.sizes.small
            Accessible.name: text
            Accessible.description: qsTr("Fetch pictures from the hosts this document names, now and whenever it is opened.")
            onClicked: preview.showWebPictures()
        }
    }

    Flickable {
        id: flickable
        objectName: "previewFlickable"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.top: webNotice.visible ? webNotice.bottom : parent.top
        clip: true
        contentWidth: width
        contentHeight: surface.y + surface.height + pane.margin
        // Dragging with the mouse selects text; the wheel and touch still scroll.
        acceptedButtons: Qt.NoButton
        boundsBehavior: Flickable.StopAtBounds
        // Text that grew shorter leaves the view no further down than its end.
        onContentHeightChanged: {
            const most = Math.max(0, contentHeight - height)
            if (contentY > most)
                contentY = most
        }

        TextEdit {
            id: surface
            objectName: "previewSurface"
            x: Math.round((flickable.width - width) / 2)
            y: pane.margin
            width: pane.columnWidth
            readOnly: true
            selectByMouse: true
            selectByKeyboard: true
            persistentSelection: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            activeFocusOnTab: true
            font.family: preview.bodyFamily
            font.pixelSize: preview.bodySize
            color: preview.textColor
            selectionColor: theme.selection
            selectedTextColor: preview.textColor
            // Nothing is typed here, so there is no caret to show.
            cursorDelegate: Item {}
            Accessible.name: pane.title.length > 0 ? qsTr("Preview of %1").arg(pane.title) : qsTr("Preview")
            Accessible.description: preview.unresolved.length > 0
                                    ? qsTr("The document as it reads. It cannot be edited here. What leads nowhere: %1").arg(preview.unresolved)
                                    : qsTr("The document as it reads. It cannot be edited here.")

            onActiveFocusChanged: if (activeFocus && !pointer.pressed && !pane.quietFocus) pane.keyFocus = true
            // The place that was clicked is where the cursor now is.
            onLinkActivated: (link) => preview.activate(link, cursorPosition, 0)
            // A selection made from the keyboard stays in view as it grows.
            onCursorRectangleChanged: if (activeFocus && pane.keyFocus && selectedText.length > 0) pane.reveal(cursorRectangle)

            // The keys read rather than write: they scroll, and with Shift
            // they select from where the last click or selection was.
            Keys.onPressed: (event) => {
                if (event.matches(StandardKey.Copy)) {
                    preview.copy(surface)
                    event.accepted = true
                    return
                }
                if (event.modifiers & (Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier))
                    return
                const line = preview.bodySize * pane.lineHeight
                const page = Math.max(line, flickable.height - 2 * line)
                const control = event.modifiers & Qt.ControlModifier
                if (control && event.key !== Qt.Key_Home && event.key !== Qt.Key_End)
                    return
                switch (event.key) {
                case Qt.Key_Up: pane.scrollBy(-line); break
                case Qt.Key_Down: pane.scrollBy(line); break
                case Qt.Key_PageUp: pane.scrollBy(-page); break
                case Qt.Key_PageDown:
                case Qt.Key_Space: pane.scrollBy(page); break
                case Qt.Key_Home: flickable.contentY = 0; break
                case Qt.Key_End: flickable.contentY = Math.max(0, flickable.contentHeight - flickable.height); break
                default: return
                }
                event.accepted = true
            }
            Keys.onEscapePressed: pane.escapePressed()

            HoverHandler {
                id: hover
                cursorShape: surface.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
            }

            // What the pointer rests on says more: where a link leads, what
            // a picture shows, why a formula could not be drawn.
            ToolTip {
                id: hint
                objectName: "previewHint"
                readonly property string said: hover.hovered ? preview.hintAt(hover.point.position.x, hover.point.position.y, 0) : ""
                visible: said.length > 0
                delay: 600
                x: Math.max(0, Math.min(surface.width - width, hover.point.position.x))
                y: hover.point.position.y + preview.bodySize * 1.5
                width: Math.min(implicitWidth, surface.width)
                text: said
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                padding: Math.round(theme.fontSize / 2)

                contentItem: Text {
                    text: hint.text
                    textFormat: Text.PlainText
                    font: hint.font
                    color: theme.foreground
                    wrapMode: Text.Wrap
                }

                background: Rectangle {
                    color: theme.menuBackground
                    radius: theme.radius
                    border.width: Math.max(1, theme.controlBorderWidth)
                    border.color: theme.menuBorder
                }
            }

            TapHandler {
                id: pointer
                onPressedChanged: if (pressed) pane.keyFocus = false
                onDoubleTapped: (point) => pane.requestSource(surface.positionAt(point.position.x, point.position.y), 0)
            }

            // What the preview places over the text, for the part near the view.
            Repeater {
                model: preview.marks

                delegate: Item {
                    id: mark
                    objectName: "previewMark"

                    required property string kind
                    required property rect rect
                    required property string label
                    required property int level
                    required property var island
                    required property string islandKind
                    required property bool wraps
                    required property real contentWidth
                    required property bool centered
                    required property bool tinted
                    required property var rules
                    required property int headerRule
                    required property int position

                    readonly property bool spoken: kind === "heading" || kind === "image"

                    x: rect.x
                    y: rect.y
                    width: rect.width
                    height: rect.height
                    // A heading and a picture are told to assistive technology
                    // where they are; the text itself is the surface's.
                    Accessible.role: kind === "heading" ? Accessible.Heading : kind === "image" ? Accessible.Graphic : Accessible.NoRole
                    Accessible.name: spoken ? label : ""
                    Accessible.description: kind === "heading" ? qsTr("Heading level %1").arg(level) : ""
                    Accessible.ignored: !spoken

                    // Only what this kind of mark draws is made; a heading
                    // and a picture draw nothing.
                    Loader {
                        anchors.fill: parent
                        sourceComponent: {
                            switch (mark.kind) {
                            case "bar": return quoteBar
                            case "rule":
                            case "notesRule": return rule
                            case "pageBreak": return pageBreak
                            case "island": return island
                            default: return null
                            }
                        }
                    }

                    Component {
                        id: quoteBar
                        Rectangle {
                            radius: width / 2
                            color: preview.barColor
                        }
                    }

                    Component {
                        id: rule
                        Item {
                            Rectangle {
                                width: parent.width
                                height: Math.max(1, theme.controlBorderWidth)
                                color: preview.ruleColor
                            }
                        }
                    }

                    // A page break is said quietly: on screen the text flows on.
                    Component {
                        id: pageBreak
                        Item {
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width
                                height: Math.max(1, theme.controlBorderWidth)
                                color: preview.ruleColor
                                opacity: 0.6
                            }

                            Rectangle {
                                anchors.centerIn: parent
                                width: Math.min(parent.width, breakLabel.implicitWidth + theme.fontSize)
                                height: parent.height
                                color: theme.background

                                Text {
                                    id: breakLabel
                                    anchors.fill: parent
                                    text: qsTr("Page break")
                                    textFormat: Text.PlainText
                                    font.family: theme.fontFamily
                                    font.pixelSize: theme.sizes.caption
                                    color: preview.dimColor
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                    Accessible.role: Accessible.StaticText
                                    Accessible.name: text
                                }
                            }
                        }
                    }

                    // A table, a code block or a formula: a document of its
                    // own, which scrolls sideways when it is wider than the column.
                    Component {
                        id: island

                        Rectangle {
                            id: box
                            objectName: "previewIsland"

                            readonly property real pad: mark.tinted ? preview.padding : 0
                            readonly property bool overflows: sideways.contentWidth > sideways.width + 0.5
                            // A selection of the text around it takes the island in: all of it is copied with the text.
                            readonly property bool covered: surface.selectionStart <= mark.position && mark.position < surface.selectionEnd

                            color: mark.tinted ? preview.codeTint : "transparent"
                            radius: mark.tinted ? theme.radius : 0
                            border.width: text.activeFocus && pane.keyFocus ? Math.max(1, theme.controlBorderWidth) : 0
                            border.color: theme.accent

                            Rectangle {
                                objectName: "islandCovered"
                                visible: box.covered
                                anchors.fill: parent
                                radius: box.radius
                                color: theme.selection
                            }

                            Flickable {
                                id: sideways
                                objectName: "islandFlickable"
                                anchors.fill: parent
                                clip: true
                                contentWidth: text.width + 2 * box.pad
                                contentHeight: height
                                flickableDirection: Flickable.HorizontalFlick
                                boundsBehavior: Flickable.StopAtBounds
                                acceptedButtons: Qt.NoButton

                                TextEdit {
                                    id: text
                                    objectName: "islandText"
                                    x: mark.centered && width + 2 * box.pad < sideways.width ? Math.round((sideways.width - width) / 2) : box.pad
                                    y: box.pad
                                    width: mark.wraps ? mark.contentWidth : implicitWidth
                                    readOnly: true
                                    selectByMouse: true
                                    selectByKeyboard: true
                                    textFormat: TextEdit.RichText
                                    wrapMode: mark.wraps ? TextEdit.WordWrap : TextEdit.NoWrap
                                    activeFocusOnTab: true
                                    font.family: mark.islandKind === "code" ? preview.codeFamily : preview.bodyFamily
                                    font.pixelSize: mark.islandKind === "code" ? preview.codeSize : preview.bodySize
                                    tabStopDistance: preview.tabWidth
                                    color: preview.textColor
                                    selectionColor: theme.selection
                                    selectedTextColor: preview.textColor
                                    cursorDelegate: Item {}
                                    Accessible.name: mark.label
                                    Accessible.description: box.overflows ? qsTr("Wider than the column: the Left and Right keys scroll it.") : ""

                                    Component.onCompleted: preview.adopt(mark.island, text)
                                    onActiveFocusChanged: {
                                        if (activeFocus && !islandPointer.pressed)
                                            pane.keyFocus = true
                                        if (activeFocus)
                                            pane.reveal(Qt.rect(mark.x, mark.y, mark.width, Math.min(mark.height, flickable.height / 2)))
                                    }
                                    onLinkActivated: (link) => preview.activate(link, cursorPosition, mark.island)

                                    Keys.onPressed: (event) => {
                                        if (event.matches(StandardKey.Copy)) {
                                            preview.copy(text)
                                            event.accepted = true
                                            return
                                        }
                                        if (event.modifiers & (Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier | Qt.ControlModifier))
                                            return
                                        const line = preview.bodySize * pane.lineHeight
                                        const most = Math.max(0, sideways.contentWidth - sideways.width)
                                        switch (event.key) {
                                        case Qt.Key_Left: sideways.contentX = Math.max(0, sideways.contentX - 3 * line); break
                                        case Qt.Key_Right: sideways.contentX = Math.min(most, sideways.contentX + 3 * line); break
                                        case Qt.Key_Up: pane.scrollBy(-line); break
                                        case Qt.Key_Down: pane.scrollBy(line); break
                                        case Qt.Key_PageUp: pane.scrollBy(-(flickable.height - 2 * line)); break
                                        case Qt.Key_PageDown:
                                        case Qt.Key_Space: pane.scrollBy(flickable.height - 2 * line); break
                                        default: return
                                        }
                                        event.accepted = true
                                    }
                                    Keys.onEscapePressed: surface.forceActiveFocus()

                                    HoverHandler {
                                        cursorShape: text.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
                                    }

                                    TapHandler {
                                        id: islandPointer
                                        onPressedChanged: if (pressed) pane.keyFocus = false
                                        onDoubleTapped: (point) => pane.requestSource(text.positionAt(point.position.x, point.position.y), mark.island)
                                    }
                                }

                                // A table's lines: one under each row, the one
                                // that ends the header plainer than the rest.
                                Repeater {
                                    model: mark.rules

                                    delegate: Rectangle {
                                        required property real modelData
                                        required property int index

                                        x: text.x
                                        y: text.y + modelData
                                        width: text.width
                                        height: Math.max(1, theme.controlBorderWidth)
                                        color: index === mark.headerRule ? preview.dimColor : preview.ruleColor
                                    }
                                }

                                ScrollBar.horizontal: ScrollBar {
                                    id: bar
                                    policy: box.overflows ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
                                    height: Math.max(3, Math.round(theme.fontSize / 3))
                                    Accessible.ignored: true

                                    contentItem: Rectangle {
                                        radius: height / 2
                                        color: preview.dimColor
                                        opacity: bar.active ? 0.7 : 0.25
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // A preview that has been reading and setting the text for more than a
    // moment without catching up says so, quietly, so that what it still
    // shows is never taken for the text as it is now. A quick update never
    // shows it, nor does the preview's wait for a pause in typing.
    Rectangle {
        id: behind
        readonly property real pad: Math.round(theme.fontSize / 3)
        visible: pane.active && pane.document !== null && preview.late
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Math.round(pane.margin / 2)
        width: Math.min(updating.implicitWidth + 2 * pad, parent.width - 2 * anchors.margins)
        height: updating.implicitHeight + pad
        radius: theme.radius
        color: theme.background
        opacity: 0.9

        Text {
            id: updating
            objectName: "previewUpdating"
            anchors.fill: parent
            anchors.leftMargin: behind.pad
            anchors.rightMargin: behind.pad
            text: qsTr("Updating…")
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.caption
            color: preview.dimColor
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }
    }

    // The preview has the keys, which came from the keyboard.
    Rectangle {
        objectName: "previewFocusMark"
        visible: pane.keyFocus && surface.activeFocus
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Math.max(2, 2 * theme.controlBorderWidth)
        color: theme.accent
    }
}
