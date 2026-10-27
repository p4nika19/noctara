import QtQuick
import QtQuick.Templates as T
import Noctara

T.Switch {
    id: control

    implicitWidth: indicator.width
    implicitHeight: Math.max(indicator.height, Theme.space.controlHeightSmall)
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    indicator: Rectangle {
        width: 44 * Theme.scale
        height: 26 * Theme.scale
        anchors.verticalCenter: parent.verticalCenter
        radius: height / 2
        color: control.checked ? Theme.colors.accent : Theme.colors.surfaceOverlay
        border.width: control.checked ? 0 : Theme.space.hairline
        border.color: Theme.colors.outline
        opacity: control.enabled ? 1 : 0.45

        Rectangle {
            readonly property real inset: 3 * Theme.scale
            width: parent.height - inset * 2 + (control.down ? 4 * Theme.scale : 0)
            height: parent.height - inset * 2
            radius: height / 2
            y: inset
            x: control.checked ? parent.width - width - inset : inset
            color: control.checked ? Theme.colors.onAccent : Theme.colors.textSecondary
            Behavior on x { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingEmphasized } }
            Behavior on width { NumberAnimation { duration: Theme.motion.fast } }
        }
        StateLayer { radius: parent.radius; hovered: control.hovered && control.enabled; pressed: control.down }
        FocusRing { target: control.indicator; targetRadius: parent.radius; visible: control.visualFocus }
    }
    contentItem: Item {}
}
