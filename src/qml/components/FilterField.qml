import QtQuick
import QtQuick.Window
import Noctara

// A search within one list (likes, the library, a playlist), driving a CollectionFilter. Until it is used it is only
// a faint "Click to search" with the key that opens it (Settings → Keyboard, "/" by default); a click or that key
// opens the full field. Typing narrows the list. Escape empties the field, a second Escape closes it, and Escape goes
// back a page only while it is closed. A field that keeps a search stays open.
TextField {
    id: control

    property CollectionFilter filter
    property string prompt  // the placeholder of the open field
    property real openWidth: 240 * Theme.scale
    readonly property bool open: activeFocus || length > 0
    readonly property string keys: Shortcuts.sequences.find_in_list || ""

    // Opened by its key (the page may want to bring it into view).
    signal summoned()

    function close() {
        focus = false
    }

    // Never narrower open than closed (a long hint, a large font).
    implicitWidth: open ? Math.max(openWidth, collapsedWidth) : collapsedWidth
    Behavior on implicitWidth {
        NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard }
    }
    readonly property real collapsedWidth: Math.ceil(leftPadding + hintMetrics.advanceWidth
                                                     + (keys.length > 0 ? keycap.width + Theme.space.sm * 2
                                                                        : Theme.space.md))
    chrome: open ? 1 : 0
    Behavior on chrome { NumberAnimation { duration: Theme.motion.fast } }
    opacity: open ? 1 : hovered ? 0.85 : 0.6
    Behavior on opacity { NumberAnimation { duration: Theme.motion.fast } }

    iconName: "search"
    placeholderText: open ? prompt : hintMetrics.text
    rightPadding: open ? (clear.visible ? clear.width + Theme.space.xs * 2 : Theme.space.md)
                       : (keycap.visible ? keycap.width + Theme.space.sm * 2 : Theme.space.md)
    onTextEdited: if (filter) filter.text = text

    // The filter's text, but only while not typing: the binding would reset the cursor.
    Binding {
        target: control
        property: "text"
        value: control.filter ? control.filter.text : ""
        when: !control.activeFocus
        restoreMode: Binding.RestoreNone
    }

    TextMetrics {
        id: hintMetrics
        font: control.font
        text: qsTr("Click to search")
    }

    HoverHandler {
        enabled: !control.open
        cursorShape: Qt.PointingHandCursor
    }

    Shortcut {
        sequence: control.keys
        enabled: control.keys.length > 0 && control.visible && control.enabled && !control.activeFocus
                 && !(control.Window.window && control.Window.window.fullPlayer === true)
        onActivated: {
            control.forceActiveFocus(Qt.ShortcutFocusReason)
            control.summoned()
        }
    }

    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape
    Keys.onEscapePressed: event => {
        if (text.length > 0) {
            if (filter)
                filter.text = ""
            else
                text = ""
        } else {
            close()
        }
        event.accepted = true
    }

    // The key that opens the field, while it is closed.
    Rectangle {
        id: keycap
        anchors.right: parent.right
        anchors.rightMargin: Theme.space.sm
        anchors.verticalCenter: parent.verticalCenter
        visible: !control.open && control.keys.length > 0
        width: Math.max(height, keyText.implicitWidth + Theme.space.sm)
        height: keyText.implicitHeight + Theme.space.xxs * 2
        // Concentric with the field's (hidden) shape.
        radius: Math.min(height / 2, Math.max(0, Math.min(Theme.radius.button, control.height / 2)
                                                 - (control.height - height) / 2))
        color: "transparent"
        border.width: Theme.space.hairline
        border.color: Theme.colors.edge
        Text {
            id: keyText
            anchors.centerIn: parent
            text: control.keys.length > 0 ? Shortcuts.display(control.keys) : ""
            font: Theme.font.monoSmall
            color: Theme.colors.textTertiary
        }
    }

    IconButton {
        id: clear
        anchors.right: parent.right
        anchors.rightMargin: Theme.space.xs
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.space.controlHeightSmall
        height: width
        visible: control.text.length > 0
        material: "flat"
        iconName: "x"
        iconSize: Theme.space.iconSizeSmall
        text: qsTr("Clear the search")
        onClicked: {
            if (control.filter)
                control.filter.text = ""
            control.forceActiveFocus()
        }
    }
}
