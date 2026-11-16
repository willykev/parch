import QtQuick
import Parch

// The Markdown settings of one layer, as Document settings and Preferences
// edit them: the profile, the extensions, the citation style, the
// bibliography and, but for every document, the library root. Each row shows
// what applies and where it comes from, and a change goes into the layer the
// form edits ("document", "library" or "application") at once; Use the
// default gives the setting back to the layers below.
Column {
    id: form

    // The document the settings are for, or null for every document.
    property Document document: null
    // The layer edited: "document", "library" or "application".
    property string scope: "document"
    // What backend.documentSettings.describe() says, or describeApplication().
    property var info: ({})
    readonly property var own: info.layers ? info.layers[scope] || ({}) : ({})
    readonly property var store: backend.documentSettings
    readonly property real controlWidth: Math.round(theme.fontSize * 16)

    // Where a value comes from, in words.
    function sourceText(from) {
        switch (from) {
        case "document": return qsTr("Set for this document")
        case "front-matter": return qsTr("From the front matter")
        case "library": return form.info.libraryName ? qsTr("Set for the library folder “%1”").arg(form.info.libraryName)
                                                     : qsTr("From the library folder")
        case "application": return qsTr("Set in Preferences for every document")
        case "profile": return qsTr("As the profile has it")
        default: return qsTr("The default")
        }
    }
    // Whether the layer edited sets the value, so that it can be given back.
    function ownSets(key) {
        const value = form.own[key]
        if (value === undefined || value === null)
            return false
        if (typeof value === "string")
            return value.length > 0
        return true
    }
    function ownExtensions() {
        return form.own.extensions ? Object.keys(form.own.extensions).length : 0
    }
    function styleIndex() {
        const style = form.info.citationStyle || ""
        if (!style.length)
            return -1
        for (let i = 0; i < store.styles.length; ++i)
            if (store.styles[i].id === style)
                return i
        return store.styles.length
    }
    function shown(path) { return backend.shownText(path) }

    // The answers of the desktop's file dialog, for this form while it shows.
    Connections {
        target: backend
        enabled: form.visible
        function onSettingsFilesChosen(purpose, paths) {
            if (purpose === "bibliography") {
                const files = (form.info.bibliography || []).slice()
                for (const path of paths)
                    if (files.indexOf(path) < 0)
                        files.push(path)
                form.store.setBibliography(form.document, form.scope, files)
            } else if (purpose === "style" && paths.length > 0) {
                form.store.setCitationStyle(form.document, form.scope, paths[0])
            } else if (purpose === "libraryRoot" && paths.length > 0) {
                form.store.setLibraryRoot(form.document, form.scope, paths[0])
            }
        }
    }

    spacing: Math.round(theme.fontSize / 2)

    PreferenceRow {
        width: parent.width
        label: qsTr("Markdown")

        Column {
            width: parent.width
            spacing: Math.round(theme.fontSize / 4)

            Flow {
                width: parent.width
                spacing: Math.round(theme.fontSize / 2)

                ThemedComboBox {
                    objectName: "profileChoice"
                    width: Math.min(form.controlWidth * 1.3, form.width * 0.6)
                    model: [qsTr("Writer-compatible"), qsTr("Standard (CommonMark and GFM)")]
                    currentIndex: form.info.profile === "standard" ? 1 : 0
                    Accessible.name: qsTr("Markdown profile")
                    Accessible.description: profileSource.text
                    onActivated: (index) => form.store.setProfile(form.document, form.scope, index === 1 ? "standard" : "writer")
                }

                Reset {
                    objectName: "profileReset"
                    visible: form.ownSets("profile")
                    Accessible.name: qsTr("Use the default profile")
                    onClicked: form.store.setProfile(form.document, form.scope, "")
                }
            }

            Note {
                id: profileSource
                objectName: "profileSource"
                text: form.sourceText(form.info.profileFrom)
            }
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Extensions")

        Column {
            width: parent.width
            spacing: 0

            Note { text: qsTr("What the Markdown is read with, beyond the profile's own:") }

            // One box for each extension, kept as the settings change, so
            // that the keys stay on the box just toggled.
            Repeater {
                model: form.store.extensions

                delegate: ThemedCheck {
                    id: box

                    required property var modelData
                    required property int index
                    readonly property var current: (form.info.extensions || [])[index] || ({})

                    objectName: "extension-" + modelData.name
                    width: Math.min(implicitWidth, parent.width)
                    text: box.current.from === "profile" || !box.current.from
                          ? modelData.title
                          : qsTr("%1 (%2)").arg(modelData.title).arg(form.sourceText(box.current.from).toLowerCase())
                    checked: box.current.on === true
                    Accessible.name: modelData.title
                    Accessible.description: form.sourceText(box.current.from)
                    onToggled: {
                        form.store.setExtension(form.document, form.scope, modelData.name, checked ? "on" : "off")
                        checked = Qt.binding(() => box.current.on === true)
                    }
                }
            }

            Reset {
                objectName: "extensionsReset"
                visible: form.ownExtensions() > 0
                text: qsTr("Use the default extensions")
                onClicked: form.store.resetExtensions(form.document, form.scope)
            }
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Citation style")

        Column {
            width: parent.width
            spacing: Math.round(theme.fontSize / 4)

            Flow {
                width: parent.width
                spacing: Math.round(theme.fontSize / 2)

                ThemedComboBox {
                    objectName: "styleChoice"
                    width: Math.min(form.controlWidth * 1.6, form.width * 0.6)
                    model: form.store.styles.map(style => style.title).concat([qsTr("A style file (.csl)…")])
                    currentIndex: form.styleIndex()
                    displayText: form.info.citationStyleTitle ? form.info.citationStyleTitle : qsTr("None")
                    Accessible.name: qsTr("Citation style")
                    Accessible.description: styleSource.text
                    onActivated: (index) => {
                        if (index < form.store.styles.length)
                            form.store.setCitationStyle(form.document, form.scope, form.store.styles[index].id)
                        else
                            backend.chooseSettingsFiles("style")
                        currentIndex = Qt.binding(() => form.styleIndex())
                    }
                }

                Reset {
                    objectName: "styleReset"
                    visible: form.ownSets("citationStyle")
                    Accessible.name: qsTr("Use the default citation style")
                    onClicked: form.store.setCitationStyle(form.document, form.scope, "")
                }
            }

            Note {
                id: styleSource
                objectName: "styleSource"
                text: form.info.citationStyle && form.info.citationStyle.startsWith("/")
                      ? qsTr("%1: %2").arg(form.sourceText(form.info.citationStyleFrom)).arg(form.shown(form.info.citationStyle))
                      : form.sourceText(form.info.citationStyleFrom)
            }
        }
    }

    PreferenceRow {
        width: parent.width
        label: qsTr("Bibliography")

        Column {
            width: parent.width
            spacing: Math.round(theme.fontSize / 4)

            Note {
                objectName: "bibliographyNone"
                visible: !(form.info.bibliography && form.info.bibliography.length)
                text: qsTr("No bibliography file. Citations can still use works written in the document.")
            }

            Repeater {
                model: form.info.bibliography || []

                delegate: Flow {
                    id: entry

                    required property string modelData
                    required property int index

                    width: parent ? parent.width : implicitWidth
                    spacing: Math.round(theme.fontSize / 2)

                    Text {
                        objectName: "bibliographyFile"
                        width: Math.min(implicitWidth, form.width * 0.45)
                        height: removeButton.height
                        text: form.shown(entry.modelData)
                        textFormat: Text.PlainText
                        elide: Text.ElideMiddle
                        verticalAlignment: Text.AlignVCenter
                        font.family: theme.fontFamily
                        font.pixelSize: theme.fontSize
                        color: theme.foreground
                        Accessible.role: Accessible.StaticText
                        Accessible.name: text
                    }

                    ThemedButton {
                        id: removeButton
                        objectName: "bibliographyRemove"
                        framed: true
                        text: qsTr("Remove")
                        Accessible.name: qsTr("Remove %1").arg(entry.modelData)
                        onClicked: {
                            const files = (form.info.bibliography || []).slice()
                            files.splice(entry.index, 1)
                            form.store.setBibliography(form.document, form.scope, files)
                        }
                    }
                }
            }

            Flow {
                width: parent.width
                spacing: Math.round(theme.fontSize / 2)

                ThemedButton {
                    objectName: "bibliographyAdd"
                    framed: true
                    text: qsTr("Add files…")
                    detail: qsTr("BibTeX, BibLaTeX, CSL JSON or CSL YAML")
                    onClicked: backend.chooseSettingsFiles("bibliography")
                }

                Reset {
                    objectName: "bibliographyReset"
                    visible: form.ownSets("bibliography")
                    Accessible.name: qsTr("Use the default bibliography")
                    onClicked: form.store.resetBibliography(form.document, form.scope)
                }
            }

            Note {
                objectName: "bibliographySource"
                text: form.sourceText(form.info.bibliographyFrom)
            }
        }
    }

    PreferenceRow {
        visible: form.scope !== "application"
        width: parent.width
        label: qsTr("Library root")

        Column {
            width: parent.width
            spacing: Math.round(theme.fontSize / 4)

            Note {
                text: qsTr("Where includes that start with / are found.")
            }

            Text {
                objectName: "libraryRootPath"
                width: parent.width
                text: form.info.libraryRoot ? form.shown(form.info.libraryRoot) : qsTr("None")
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                font.family: theme.fontFamily
                font.pixelSize: theme.fontSize
                color: theme.foreground
                Accessible.role: Accessible.StaticText
                Accessible.name: qsTr("Library root: %1").arg(text)
            }

            Flow {
                width: parent.width
                spacing: Math.round(theme.fontSize / 2)

                ThemedButton {
                    objectName: "libraryRootChoose"
                    framed: true
                    text: qsTr("Choose a folder…")
                    onClicked: backend.chooseSettingsFiles("libraryRoot")
                }

                Reset {
                    objectName: "libraryRootReset"
                    visible: form.ownSets("libraryRoot")
                    Accessible.name: qsTr("Use the default library root")
                    onClicked: form.store.setLibraryRoot(form.document, form.scope, "")
                }
            }

            Note {
                objectName: "libraryRootSource"
                text: form.sourceText(form.info.libraryRootFrom)
            }
        }
    }

    component Note: Text {
        width: parent ? parent.width : implicitWidth
        textFormat: Text.PlainText
        font.family: theme.fontFamily
        font.pixelSize: theme.sizes.small
        color: theme.dim
        wrapMode: Text.Wrap
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    // Gives a setting back to the layers below this one.
    component Reset: ThemedButton {
        framed: true
        text: qsTr("Use the default")
    }
}
