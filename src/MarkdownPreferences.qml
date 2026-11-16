import QtQuick
import Parch

// How every document is read, as a section of the Preferences dialog: the
// Markdown profile and extensions, the citation style and the bibliography
// that apply wherever a document, its front matter or its library folder
// does not choose its own (Document settings… does that). Each change
// applies at once in every window and is kept.
PreferenceSection {
    id: section

    title: qsTr("Markdown and citations")

    Text {
        width: parent.width
        text: qsTr("For every document, unless its own settings, its front matter or its library folder say "
                   + "otherwise. Document settings… changes them for one document or one library folder.")
        textFormat: Text.PlainText
        wrapMode: Text.Wrap
        font.family: theme.fontFamily
        font.pixelSize: theme.sizes.small
        color: theme.dim
        Accessible.role: Accessible.StaticText
        Accessible.name: text
    }

    DialectSettingsForm {
        objectName: "applicationDialectForm"
        width: parent.width
        scope: "application"
        info: {
            const revision = backend.documentSettings.revision
            return section.visible && revision >= 0 ? backend.documentSettings.describeApplication() : ({})
        }
    }
}
