import QtQuick
import QtQuick.Templates as T
import Noctara

// Modal confirmation over a dimmed window: title, message, optional controls (the dialog's children) and two
// actions. `destructive` styles the confirm button as a danger action. Opens centred in the window's overlay, so
// clipping parents never cut it off.
T.Dialog {
    id: control

    property string message
    property string confirmText: qsTr("OK")
    property bool destructive: false
    // False while the controls do not allow confirming yet (e.g. an empty name).
    property bool confirmEnabled: true
    // What has the keyboard focus when the dialog opens: the confirm button unless set.
    property Item initialFocus: null
    default property alias controls: extra.data

    parent: T.Overlay.overlay
    x: Math.round(((parent ? parent.width : (T.Overlay.overlay ? T.Overlay.overlay.width : 800)) - width) / 2)
    y: Math.round(((parent ? parent.height : (T.Overlay.overlay ? T.Overlay.overlay.height : 600)) - height) / 2)
    modal: true
    focus: true
    width: Math.min(480 * Theme.scale, parent ? parent.width - Theme.space.xl * 2 : 480 * Theme.scale)
    height: contentItem ? contentItem.implicitHeight + topPadding + bottomPadding : 400
    padding: Math.max(Theme.space.xl, panel.cornerInset)
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside

    T.Overlay.modal: Rectangle {
        color: Theme.colors.scrim
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fast } }
    }
    background: Rectangle {
        id: panel
        radius: Theme.radius.lg
        color: Theme.colors.solidOverlay
        border.width: Math.max(1, Theme.space.hairline)
        border.color: Theme.glass.flat ? Theme.colors.divider : Theme.colors.edge

        readonly property real cornerInset: Math.min(radius, control.width / 2) * (1 - Math.SQRT1_2)

        Shadow {
            anchors.fill: parent
            z: -1
            active: Graphics.hardware && !Theme.glass.flat
            radius: panel.radius
            color: Theme.colors.shadow
            blur: 64 * Theme.scale
            spread: 0
            offsetY: 20 * Theme.scale
        }
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
            visible: text.length > 0
            text: control.message
            font: Theme.font.body
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }
        Column {
            id: extra
            width: parent.width
            visible: children.length > 0
            spacing: Theme.space.md
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
                enabled: control.confirmEnabled
                onClicked: control.accept()
            }
        }
    }
    onOpened: (control.initialFocus || confirm).forceActiveFocus()
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motion.fast }
        NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motion.fast } }
}
