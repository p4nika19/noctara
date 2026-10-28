import QtQuick
import QtQuick.Templates as T
import Noctara

// Modal confirmation over a dimmed window: title, message and two actions. `destructive` styles the confirm
// button as a danger action. Opens centred in the window's overlay, so clipping parents never cut it off.
T.Dialog {
    id: control

    property string message
    property string confirmText: qsTr("OK")
    property bool destructive: false

    parent: T.Overlay.overlay
    anchors.centerIn: parent
    modal: true
    focus: true
    width: Math.min(440 * Theme.scale, parent ? parent.width - Theme.space.xl * 2 : 440 * Theme.scale)
    padding: Math.max(Theme.space.xl, panel.cornerInset)
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside

    T.Overlay.modal: Rectangle {
        color: Theme.colors.scrim
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fast } }
    }
    background: GlassPanel {
        id: panel
        level: 2
        liquid: true
        radius: Theme.radius.lg
    }
    contentItem: Column {
        spacing: Theme.space.md
        Accessible.role: Accessible.Dialog
        Accessible.name: control.title
        Text {
            width: parent.width
            text: control.title
            font: Theme.font.title
            color: Theme.colors.textPrimary
            wrapMode: Text.Wrap
            Accessible.role: Accessible.Heading
        }
        Text {
            width: parent.width
            text: control.message
            font: Theme.font.body
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }
        Row {
            anchors.right: parent.right
            topPadding: Theme.space.sm
            spacing: Theme.space.sm
            Button {
                text: qsTr("Cancel")
                onClicked: control.reject()
            }
            Button {
                id: confirm
                variant: control.destructive ? "danger" : "primary"
                text: control.confirmText
                onClicked: control.accept()
            }
        }
    }
    onOpened: confirm.forceActiveFocus()
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motion.fast }
        NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motion.fast } }
}
