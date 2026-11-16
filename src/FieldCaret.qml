import QtQuick
import Parch

// The caret a text field shows as its cursor delegate, lit as the field's
// CaretRest says: while the field has the keys, blinking at the desktop's
// flash time and resting after a pause, as the editor's caret does, so that
// an idle window draws nothing. The CaretRest lives in the field, since a
// TextEdit makes its delegate only once its cursor is first shown.
Rectangle {
    property CaretRest rest: null

    objectName: "fieldCaret"
    width: Math.max(1, Math.round(theme.fontSize / 12))
    color: theme.accent
    visible: rest !== null && rest.shown
}
