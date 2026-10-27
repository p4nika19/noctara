import QtQuick
import QtQuick.Templates as T
import Noctara

T.Switch {
    id: control

    implicitWidth: indicator.width
    implicitHeight: Math.max(indicator.height, Theme.space.controlHeightSmall)
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    indicator: Item {
        width: 44 * Theme.scale
        height: 26 * Theme.scale
        anchors.verticalCenter: parent.verticalCenter
        opacity: control.enabled ? 1 : 0.45

        Shadow {
            anchors.fill: track
            z: -1
            radius: track.radius
            blur: 18 * Theme.scale
            spread: -2 * Theme.scale
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                           control.checked && Theme.glass.glowControls ? Theme.glass.glow * 0.8 : 0)
        }
        Rectangle {
            id: track
            anchors.fill: parent
            radius: height / 2
            color: control.checked ? Theme.colors.accent
                                   : (Theme.dark ? Qt.rgba(1, 1, 1, control.hovered ? 0.22 : 0.16)
                                                 : Qt.rgba(0, 0, 0, control.hovered ? 0.18 : 0.13))
            Behavior on color { ColorAnimation { duration: Theme.motion.fast } }
        }
        Rectangle {
            id: knob
            readonly property real inset: 3 * Theme.scale
            width: parent.height - inset * 2
            height: width
            radius: width / 2
            y: inset
            x: control.checked ? parent.width - width - inset : inset
            color: "white"
            Behavior on x { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingEmphasized } }
            Shadow {
                anchors.fill: parent
                z: -1
                radius: parent.radius
                blur: 6 * Theme.scale
                offsetY: 2 * Theme.scale
                color: Qt.rgba(0, 0, 0, 0.35)
            }
        }
        FocusRing { target: control; targetRadius: track.radius; visible: control.visualFocus }
    }
    contentItem: Item {}
}
