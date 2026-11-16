import QtQuick
import QtQuick.Controls.Basic
import Parch

// Export and Print: the format and its options, and before anything is
// written, what that format leaves out of this document, such as diagrams
// that become pictures or formulas that stay their TeX. Export… then asks
// the desktop's file dialog where to write it, and Print… the desktop's
// print dialog how to print it; either goes on in the background, its
// progress in the notice bar. The options are the format's own, as `parch
// export` takes them.
ThemedDialog {
    id: dialog

    property Document document: null
    // "export" or "print".
    property string mode: "export"
    property string format: "pdf"
    // The options chosen, by name; an option not here has its default.
    property var values: ({})

    readonly property var formats: backend.exports.formats
    readonly property var formatInfo: {
        const wanted = dialog.mode === "print" ? "pdf" : dialog.format
        for (let i = 0; i < formats.length; ++i)
            if (formats[i].format === wanted)
                return formats[i]
        return null
    }
    // The print dialog chooses the paper and its way round.
    readonly property var shownOptions: formatInfo
        ? formatInfo.options.filter(option => dialog.mode !== "print" || (option.name !== "page-size" && option.name !== "landscape"))
        : []
    readonly property var limitations: {
        const revision = backend.exports.revision
        return dialog.visible && revision >= 0
            ? backend.exports.limitations(dialog.mode === "print" ? "print" : dialog.format, dialog.values) : []
    }

    function show(document, mode) {
        dialog.document = document
        dialog.mode = mode
        const last = backend.exports.lastFormat(document)
        dialog.format = mode === "print" || last.length === 0 ? "pdf" : last
        dialog.values = mode === "export" && last.length > 0 ? backend.exports.lastOptions(document) : ({})
        backend.exports.read(document)
        open()
    }

    function setValue(name, value) {
        const next = Object.assign({}, dialog.values)
        next[name] = value
        dialog.values = next
    }

    function valueOf(option) {
        const given = dialog.values[option.name]
        if (given !== undefined)
            return option.kind === "switch" ? given === true || given === "" : given
        // A PDF's margins and text size are its template's until others are chosen.
        if (dialog.paged && (option.name === "margins" || option.name === "font-size"))
            return backend.exports.paperDefaults(dialog.values["template"] || "classic")[option.name]
        return option.kind === "switch" ? false : option.defaultValue
    }

    // The pages are shown as they will come out: for a PDF and for printing.
    readonly property bool paged: dialog.mode === "print" || dialog.format === "pdf"
    readonly property PrintPreview preview: backend.exports.preview

    function showPages() {
        if (dialog.visible && dialog.paged && dialog.document)
            dialog.preview.show(dialog.document, dialog.mode, dialog.values)
        else
            dialog.preview.stop()
    }

    onValuesChanged: showPages()
    onPagedChanged: showPages()
    onModeChanged: showPages()
    onVisibleChanged: showPages()

    function go() {
        if (!dialog.document || backend.exports.busy)
            return
        if (dialog.mode === "print")
            backend.exports.print(dialog.document, dialog.values)
        else
            backend.exports.exportDocument(dialog.document, dialog.format, dialog.values)
        dialog.close()
    }

    // Keeps the control with the keys in view.
    function reveal(item) {
        if (!item || !visible)
            return
        for (let at = item; at; at = at.parent) {
            if (at === body) {
                const box = item.mapToItem(body, 0, 0, item.width, item.height)
                if (box.y < flick.contentY)
                    flick.contentY = box.y
                else if (box.y + box.height > flick.contentY + flick.height)
                    flick.contentY = Math.min(box.y + box.height - flick.height, flick.contentHeight - flick.height)
                return
            }
        }
    }

    title: dialog.mode === "print"
           ? qsTr("Print “%1”").arg(dialog.document ? dialog.document.title : "")
           : qsTr("Export “%1”").arg(dialog.document ? dialog.document.title : "")
    preferredCharacters: 64
    preferredHeight: Math.round(theme.fontSize * 34)
    // The safe place to start: the format, or Cancel when printing.
    initialFocus: dialog.mode === "print" ? cancelButton : formatChoice

    contentItem: Flickable {
        id: flick
        objectName: "exportBody"
        clip: true
        contentHeight: body.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        implicitHeight: body.implicitHeight

        Connections {
            target: dialog.visible ? flick.Window.window : null
            function onActiveFocusItemChanged() { dialog.reveal(flick.Window.window.activeFocusItem) }
        }

        Column {
            id: body
            width: flick.width - (scroller.visible ? scroller.width : 0)
            spacing: Math.round(theme.fontSize * 2 / 3)

            PreferenceRow {
                width: parent.width
                visible: dialog.mode === "export"
                label: qsTr("Format")

                ThemedComboBox {
                    id: formatChoice
                    objectName: "exportFormat"
                    width: Math.min(Math.round(theme.fontSize * 14), parent.width)
                    model: dialog.formats.map(format => format.title)
                    currentIndex: Math.max(0, dialog.formats.findIndex(format => format.format === dialog.format))
                    Accessible.name: qsTr("Format")
                    onActivated: (index) => dialog.format = dialog.formats[index].format
                }
            }

            Repeater {
                model: dialog.shownOptions

                delegate: Loader {
                    id: option
                    width: body.width
                    readonly property var info: modelData
                    sourceComponent: info.kind === "switch" ? switchOption : rowOption
                }
            }

            Column {
                objectName: "exportLimitations"
                width: parent.width
                spacing: Math.round(theme.fontSize / 4)

                Text {
                    objectName: "exportLimitationsHeading"
                    width: parent.width
                    text: backend.exports.reading ? qsTr("Reading the document…")
                          : dialog.limitations.length > 0
                            ? (dialog.mode === "print" ? qsTr("On paper:") : qsTr("In %1:").arg(dialog.formatInfo ? dialog.formatInfo.title : ""))
                            : qsTr("Everything in this document goes into it.")
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    font.weight: Font.DemiBold
                    color: theme.foreground
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.StaticText
                    Accessible.name: text
                }

                Repeater {
                    model: backend.exports.reading ? [] : dialog.limitations

                    delegate: Text {
                        required property string modelData
                        objectName: "exportLimitation"
                        width: parent ? parent.width : 0
                        leftPadding: Math.round(theme.fontSize)
                        text: modelData
                        textFormat: Text.PlainText
                        font.family: theme.fontFamily
                        font.pixelSize: theme.sizes.small
                        color: theme.dim
                        wrapMode: Text.Wrap
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }
                }
            }

            // The pages as they will come out, side by side; the arrows
            // move from page to page.
            Column {
                objectName: "printPreview"
                visible: dialog.paged
                width: parent.width
                spacing: Math.round(theme.fontSize / 3)

                Text {
                    objectName: "previewPageLabel"
                    width: parent.width
                    text: dialog.preview.pageCount > 0
                          ? qsTr("Page %1 of %2").arg(pages.currentIndex + 1).arg(dialog.preview.pageCount)
                          : qsTr("Setting the pages…")
                    textFormat: Text.PlainText
                    font.family: theme.fontFamily
                    font.pixelSize: theme.fontSize
                    font.weight: Font.DemiBold
                    color: theme.foreground
                    elide: Text.ElideRight
                    Accessible.ignored: true
                }

                ListView {
                    id: pages
                    objectName: "previewPages"
                    width: parent.width
                    height: Math.round(theme.fontSize * 20)
                    orientation: ListView.Horizontal
                    spacing: Math.round(theme.fontSize)
                    clip: true
                    // Pages are drawn only while the dialog shows them.
                    model: dialog.visible && dialog.paged ? dialog.preview.pageCount : 0
                    activeFocusOnTab: true
                    keyNavigationEnabled: true
                    highlightMoveDuration: 0
                    boundsBehavior: Flickable.StopAtBounds
                    Accessible.role: Accessible.List
                    Accessible.name: qsTr("Print preview")
                    Accessible.description: dialog.preview.pageCount > 0
                        ? qsTr("Page %1 of %2").arg(pages.currentIndex + 1).arg(dialog.preview.pageCount) : ""
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Home) {
                            pages.currentIndex = 0
                            event.accepted = true
                        } else if (event.key === Qt.Key_End) {
                            pages.currentIndex = pages.count - 1
                            event.accepted = true
                        }
                    }
                    onCountChanged: if (currentIndex >= count) currentIndex = Math.max(0, count - 1)

                    delegate: Rectangle {
                        id: sheet
                        required property int index
                        height: pages.height
                        width: Math.round(height / Math.max(0.1, dialog.preview.pageAspect))
                        color: theme.background
                        border.width: Math.max(1, theme.controlBorderWidth) * (sheet.ListView.isCurrentItem && pages.activeFocus ? 2 : 1)
                        border.color: sheet.ListView.isCurrentItem && pages.activeFocus ? theme.accent : theme.controlBorder
                        Accessible.role: Accessible.Graphic
                        Accessible.name: qsTr("Page %1 of %2").arg(sheet.index + 1).arg(dialog.preview.pageCount)

                        PreviewPage {
                            objectName: "previewPage"
                            anchors.fill: parent
                            anchors.margins: parent.border.width
                            preview: dialog.preview
                            page: sheet.index
                        }
                    }
                }
            }
        }

        ScrollBar.vertical: ThemedDialog.Scroller {
            id: scroller
            Accessible.name: qsTr("Scroll the export options")
        }
    }

    footer: ThemedDialog.Choices {
        ThemedDialog.Choice {
            id: cancelButton
            objectName: "cancelExport"
            text: qsTr("Cancel")
            onClicked: dialog.close()
        }
        ThemedDialog.Choice {
            id: goButton
            objectName: "exportGo"
            text: dialog.mode === "print" ? qsTr("Print…") : qsTr("Export…")
            enabled: dialog.document !== null && !backend.exports.busy
            onClicked: dialog.go()
        }
    }

    Component {
        id: switchOption

        ThemedCheck {
            objectName: "exportOption-" + info.name
            width: Math.min(implicitWidth, body.width)
            text: info.label
            checked: dialog.valueOf(info)
            Accessible.name: text
            onToggled: dialog.setValue(info.name, checked)
        }
    }

    Component {
        id: rowOption

        PreferenceRow {
            label: info.label

            ThemedComboBox {
                objectName: info.kind === "choice" ? "exportOption-" + info.name : ""
                visible: info.kind === "choice"
                width: Math.min(Math.round(theme.fontSize * 14), parent.width)
                model: info.kind === "choice" ? info.choices : []
                currentIndex: info.kind === "choice" ? Math.max(0, info.choices.indexOf(String(dialog.valueOf(info)))) : -1
                Accessible.name: info.label
                onActivated: (index) => dialog.setValue(info.name, info.choices[index])
            }

            ThemedSpinBox {
                objectName: info.kind === "number" ? "exportOption-" + info.name : ""
                visible: info.kind === "number"
                from: info.kind === "number" ? info.minimum : 0
                to: info.kind === "number" ? info.maximum : 0
                value: info.kind === "number" ? Number(dialog.valueOf(info)) : 0
                Accessible.name: info.label
                onValueModified: dialog.setValue(info.name, String(value))
            }

            ThemedField {
                objectName: info.kind === "text" ? "exportOption-" + info.name : ""
                visible: info.kind === "text"
                width: parent.width
                text: info.kind === "text" ? (dialog.values[info.name] || "") : ""
                placeholderText: info.name === "title" ? qsTr("From the document")
                                 : info.name === "font" ? qsTr("A face made for reading") : ""
                Accessible.name: info.label
                onTextEdited: dialog.setValue(info.name, text)
            }
        }
    }
}
