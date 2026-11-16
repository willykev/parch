import QtQuick
import QtQuick.Controls.Basic
import Parch

// Search across the library: what is typed is looked for in every document
// of the library's folders and in every draft, and the matches stream in
// beneath, grouped by document, each with its line and the match marked.
// Down goes from the field to the results and Up back from the first;
// Enter, a click or Space opens the document with the match selected. The
// dialog keeps its query and results until the next search, so that it
// opens again where it was left.
ThemedDialog {
    id: dialog

    required property WindowContext context
    readonly property FolderSearch search: context.folderSearch

    readonly property real rowHeight: Math.round(theme.fontSize * 2.2)
    readonly property real gap: Math.round(theme.fontSize / 2)

    function show(text) {
        if (text.length > 0 && text !== field.text)
            field.text = text
        open()
        field.selectAll()
    }

    // Searches now for what the field holds, unless that has been searched for.
    function searchNow() {
        settle.stop()
        if (search.pattern.length > 0)
            search.start()
    }

    function openRow(row) {
        const found = search.row(row)
        if (!found.section)
            return
        dialog.accept()
        backend.activate(dialog.context)
        backend.openSearchResult(found.path, found.documentId, found.start, found.length, found.line, found.match)
    }

    function move(rows) {
        if (list.count === 0)
            return
        list.currentIndex = Math.max(0, Math.min(list.count - 1, list.currentIndex + rows))
        list.positionViewAtIndex(list.currentIndex, ListView.Contain)
    }

    function formatted(number) {
        return Number(number).toLocaleString(Qt.locale(), "f", 0)
    }

    // What the search has done, in a line.
    readonly property string status: {
        if (search.error.length > 0)
            return ""
        if (search.pattern.length === 0)
            return search.folderCount === 0 ? qsTr("The library has no folders yet; drafts are searched.") : ""
        const documents = search.filesSearched === 1 ? qsTr("1 document") : qsTr("%1 documents").arg(formatted(search.filesSearched))
        let line
        if (search.searching)
            line = qsTr("Searching… %1 so far").arg(documents)
        else if (search.matchCount === 0 && search.finished)
            line = qsTr("No matches in %1").arg(documents)
        else if (search.finished || search.matchCount > 0)
            line = qsTr("%1 in %2 of %3").arg(search.matchCount === 1 ? qsTr("1 match") : qsTr("%1 matches").arg(formatted(search.matchCount)))
                                          .arg(search.filesMatched === 1 ? qsTr("1 document") : qsTr("%1 documents").arg(formatted(search.filesMatched)))
                                          .arg(documents)
        else
            return ""
        if (search.unreadable > 0)
            line += " · " + (search.unreadable === 1 ? qsTr("1 could not be read") : qsTr("%1 could not be read").arg(formatted(search.unreadable)))
        if (search.truncated)
            line += " · " + qsTr("not every match is listed")
        return line
    }

    title: qsTr("Search the library")
    preferredCharacters: 84
    preferredHeight: parent ? parent.height : implicitHeight
    initialFocus: field

    // Typing searches once it pauses; Enter searches at once.
    Timer {
        id: settle
        interval: 250
        onTriggered: dialog.searchNow()
    }

    contentItem: Item {
        implicitHeight: searchRow.height + dialog.rowHeight * 10

        Item {
            id: searchRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: Math.round(theme.fontSize * 2.4)

            TextField {
                id: field
                objectName: "librarySearchField"
                anchors.left: parent.left
                anchors.right: toggles.left
                anchors.rightMargin: dialog.gap
                height: parent.height
                leftPadding: dialog.gap
                rightPadding: dialog.gap
                topPadding: 0
                bottomPadding: 0
                placeholderText: qsTr("Search the library")
                placeholderTextColor: theme.dim
                color: theme.foreground
                selectionColor: theme.selection
                selectedTextColor: theme.foreground
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                verticalAlignment: TextInput.AlignVCenter
                cursorDelegate: FieldCaret { rest: fieldCaret }
                Accessible.name: placeholderText
                Accessible.searchEdit: true
                Accessible.description: dialog.search.error.length > 0 ? dialog.search.error : dialog.status

                CaretRest {
                    id: fieldCaret
                    objectName: "caretRest"
                    target: field
                    steady: theme.reduceMotion
                }

                onTextChanged: {
                    if (text === dialog.search.pattern)
                        return
                    dialog.search.pattern = text
                    if (text.length === 0)
                        dialog.search.cancel()
                    else
                        settle.restart()
                }
                Keys.onPressed: (event) => {
                    switch (event.key) {
                    case Qt.Key_Down:
                        if (list.count > 0) {
                            list.currentIndex = Math.max(0, list.currentIndex)
                            list.forceActiveFocus(Qt.TabFocusReason)
                        }
                        break
                    case Qt.Key_Return:
                    case Qt.Key_Enter:
                        if (settle.running || (!dialog.search.searching && !dialog.search.finished))
                            dialog.searchNow()
                        else if (list.count > 0)
                            dialog.openRow(Math.max(0, list.currentIndex))
                        break
                    default:
                        return
                    }
                    event.accepted = true
                }

                background: Rectangle {
                    radius: theme.radius
                    color: theme.background
                    border.width: Math.max(1, theme.controlBorderWidth)
                    border.color: field.activeFocus ? theme.accent : theme.controlBorder
                }
            }

            Row {
                id: toggles
                anchors.right: parent.right
                height: parent.height

                Repeater {
                    model: [
                        { key: "caseSensitive", label: qsTr("Aa"), name: qsTr("Match case") },
                        { key: "wholeWord", label: qsTr("W"), name: qsTr("Whole words") },
                        { key: "regex", label: qsTr(".*"), name: qsTr("Regular expression") }
                    ]

                    delegate: ThemedButton {
                        required property var modelData

                        objectName: "librarySearchToggle"
                        width: searchRow.height
                        height: searchRow.height
                        padding: 0
                        checkable: true
                        checked: dialog.search[modelData.key]
                        framed: checked
                        textColor: checked ? theme.accent : theme.foreground
                        text: modelData.label
                        detail: modelData.name
                        Accessible.name: modelData.name
                        Accessible.checkable: true
                        Accessible.checked: checked
                        onToggled: {
                            dialog.search[modelData.key] = checked
                            dialog.searchNow()
                        }
                    }
                }
            }
        }

        Text {
            id: statusLine
            objectName: "librarySearchStatus"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: searchRow.bottom
            anchors.topMargin: dialog.gap
            text: dialog.search.error.length > 0 ? dialog.search.error : dialog.status
            textFormat: Text.PlainText
            font.family: theme.fontFamily
            font.pixelSize: theme.sizes.small
            color: dialog.search.error.length > 0 ? theme.danger : theme.dim
            wrapMode: Text.Wrap
            Accessible.role: Accessible.StaticText
            Accessible.name: text
        }

        ListView {
            id: list
            objectName: "librarySearchResults"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: statusLine.bottom
            anchors.topMargin: dialog.gap
            anchors.bottom: parent.bottom
            clip: true
            model: dialog.search
            boundsBehavior: Flickable.StopAtBounds
            highlightMoveDuration: 0
            activeFocusOnTab: count > 0
            keyNavigationEnabled: true
            reuseItems: true
            section.property: "section"
            section.criteria: ViewSection.FullString
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Search results")
            Keys.onReturnPressed: dialog.openRow(currentIndex)
            Keys.onEnterPressed: dialog.openRow(currentIndex)
            Keys.onSpacePressed: dialog.openRow(currentIndex)
            Keys.onUpPressed: (event) => {
                if (currentIndex <= 0)
                    field.forceActiveFocus(Qt.TabFocusReason)
                else
                    event.accepted = false
            }

            section.delegate: Item {
                id: header

                required property string section
                readonly property var about: dialog.search.section(section)

                width: list.width
                height: dialog.rowHeight

                Text {
                    id: name
                    anchors.left: parent.left
                    anchors.leftMargin: dialog.gap
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(implicitWidth, parent.width / 2)
                    text: header.about.title
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    font.weight: Font.DemiBold
                    color: theme.bright
                    elide: Text.ElideRight
                    Accessible.role: Accessible.Heading
                    Accessible.name: qsTr("%1, in %2, %3").arg(header.about.title).arg(header.about.folder)
                                     .arg(header.about.matches === 1 ? qsTr("1 match") : qsTr("%1 matches").arg(header.about.matches))
                }

                Text {
                    anchors.left: name.right
                    anchors.leftMargin: dialog.gap
                    anchors.right: parent.right
                    anchors.rightMargin: dialog.gap
                    anchors.verticalCenter: parent.verticalCenter
                    text: header.about.matches === 1 ? qsTr("%1 · 1 match").arg(header.about.folder)
                                                     : qsTr("%1 · %2 matches").arg(header.about.folder).arg(header.about.matches)
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.sizes.small
                    color: theme.dim
                    elide: Text.ElideMiddle
                    Accessible.ignored: true
                }
            }

            delegate: AbstractButton {
                id: result
                objectName: "librarySearchResult"

                required property int index
                required property int line
                required property string before
                required property string match
                required property string after
                required property string lineText
                required property string title

                readonly property bool current: ListView.isCurrentItem

                width: list.width
                height: dialog.rowHeight
                focusPolicy: Qt.NoFocus
                hoverEnabled: true
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                Accessible.role: Accessible.ListItem
                Accessible.name: qsTr("%1, line %2: %3").arg(title).arg(line).arg(lineText)
                Accessible.selected: current
                onClicked: {
                    list.currentIndex = index
                    dialog.openRow(index)
                }

                contentItem: Item {
                    Text {
                        id: number
                        x: dialog.gap
                        width: Math.round(theme.fontSize * 3.5)
                        anchors.verticalCenter: parent.verticalCenter
                        text: result.line
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizes.small
                        color: theme.dim
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideLeft
                    }

                    // The line around the match, the match marked, cut at
                    // the edges of the row rather than running past them.
                    Item {
                        id: snippet
                        anchors.left: number.right
                        anchors.leftMargin: dialog.gap
                        anchors.right: parent.right
                        anchors.rightMargin: dialog.gap
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        clip: true

                        readonly property real beforeRoom: Math.min(beforeText.implicitWidth, Math.max(0, width / 3))

                        Text {
                            id: beforeText
                            width: snippet.beforeRoom
                            anchors.verticalCenter: parent.verticalCenter
                            text: result.before
                            textFormat: Text.PlainText
                            font: result.font
                            color: theme.foreground
                            elide: Text.ElideLeft
                        }

                        Rectangle {
                            x: beforeText.width
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.min(matchText.implicitWidth, Math.max(0, snippet.width - x))
                            height: matchText.implicitHeight
                            radius: Math.min(theme.radius, 3)
                            color: theme.selection

                            Text {
                                id: matchText
                                width: parent.width
                                text: result.match
                                textFormat: Text.PlainText
                                font: result.font
                                color: theme.bright
                                elide: Text.ElideRight
                            }
                        }

                        Text {
                            x: beforeText.width + Math.min(matchText.implicitWidth, Math.max(0, snippet.width - beforeText.width))
                            width: Math.max(0, snippet.width - x)
                            anchors.verticalCenter: parent.verticalCenter
                            text: result.after
                            textFormat: Text.PlainText
                            font: result.font
                            color: theme.foreground
                            elide: Text.ElideRight
                        }
                    }
                }

                background: Rectangle {
                    radius: theme.radius
                    color: result.down ? theme.pressedFill : result.current || result.hovered ? theme.hoverFill : "transparent"
                    border.width: result.current && list.activeFocus ? Math.max(1, theme.controlBorderWidth) : 0
                    border.color: theme.accent
                }
            }
        }
    }
}
