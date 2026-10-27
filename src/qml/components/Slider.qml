import QtQuick
import QtQuick.Templates as T
import Noctara

T.Slider {
    id: control

    implicitWidth: 200 * Theme.scale
    implicitHeight: Theme.space.controlHeightSmall
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    background: Item {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 4 * Theme.scale
        opacity: control.enabled ? 1 : 0.45

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: Theme.colors.waveformUnplayed
        }
        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: height / 2
            color: Theme.colors.accent
        }
    }
    handle: Rectangle {
        property real diameter: (control.pressed ? 20 : 16) * Theme.scale
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: diameter
        height: diameter
        radius: diameter / 2
        color: Theme.colors.accent
        border.width: 2 * Theme.scale
        border.color: Theme.colors.onAccent
        opacity: control.enabled ? 1 : 0.45
        Behavior on diameter { NumberAnimation { duration: Theme.motion.fast; easing.type: Theme.motion.easingEmphasized } }
        FocusRing { target: control.handle; targetRadius: parent.radius; visible: control.visualFocus }
    }
}
