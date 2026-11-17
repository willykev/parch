import QtQuick

// One of a window's dialogs or menus, made the first time it is asked for
// rather than with the window. Most windows never open most of them, and
// every window made all of them, some five thousand objects in all; made
// once, it stays. Its handlers run in the window's context, as if it were
// declared there.
QtObject {
    id: lazy

    // The dialog or menu, declared in the window.
    required property Component component
    // The window it belongs to.
    required property QtObject owner
    // The dialog or menu, once it has been asked for; null until then.
    property var popup: null
    // It has been made and is open.
    readonly property bool visible: popup !== null && popup.visible

    // The dialog or menu, made now if it was not yet.
    function made() {
        if (popup === null)
            popup = component.createObject(owner)
        return popup
    }

    // Closes it, if it was ever made.
    function close() {
        if (popup !== null)
            popup.close()
    }
}
