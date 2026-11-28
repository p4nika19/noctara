import QtQuick
import QtQuick.Templates as T
import Noctara

T.Slider {
    id: control

    // Light up the filled part (used for progress-like sliders; follows the "Progress glow" setting).
    property bool glow: false

    implicitWidth: vertical ? Theme.space.controlHeightSmall : 200 * Theme.scale
    implicitHeight: vertical ? 120 * Theme.scale : Theme.space.controlHeightSmall
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true

    background: Item {
        x: control.vertical ? control.leftPadding + control.availableWidth / 2 - width / 2 : control.leftPadding
        y: control.vertical ? control.topPadding : control.topPadding + control.availableHeight / 2 - height / 2
        width: control.vertical ? 6 * Theme.scale : control.availableWidth
        height: control.vertical ? control.availableHeight : 6 * Theme.scale
        opacity: control.enabled ? 1 : 0.45

        Rectangle {
            anchors.fill: parent
            radius: Math.min(width, height) / 2
            color: Theme.dark ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(0, 0, 0, 0.12)
        }
        Shadow {
            anchors.fill: fill
            z: -1
            radius: fill.radius
            blur: 16 * Theme.scale
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                           control.glow && Theme.glass.glowProgress ? Theme.glass.glow : 0)
        }
        Rectangle {
            id: fill
            width: control.vertical ? parent.width : Math.max(height, control.visualPosition * parent.width)
            height: control.vertical ? Math.max(width, (1 - control.visualPosition) * parent.height) : parent.height
            y: control.vertical ? parent.height - height : 0
            radius: Math.min(width, height) / 2
            gradient: Gradient {
                orientation: control.vertical ? Gradient.Vertical : Gradient.Horizontal
                GradientStop { position: 0.0; color: Theme.colors.accentLight }
                GradientStop { position: 1.0; color: Theme.colors.accent }
            }
        }
    }
    handle: Item {
        readonly property real diameter: 16 * Theme.scale
        x: control.vertical ? control.leftPadding + control.availableWidth / 2 - width / 2
                            : control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.vertical ? control.topPadding + control.visualPosition * (control.availableHeight - height)
                            : control.topPadding + control.availableHeight / 2 - height / 2
        width: diameter
        height: diameter
        opacity: control.enabled ? 1 : 0.45

        // Accent ring around a white knob; grows a little while dragging.
        Rectangle {
            anchors.centerIn: parent
            width: parent.diameter + (control.pressed ? 12 : 8) * Theme.scale
            height: width
            radius: width / 2
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.3)
            Behavior on width { NumberAnimation { duration: Theme.motion.fast; easing.type: Theme.motion.easingEmphasized } }
        }
        Rectangle {
            anchors.fill: parent
            radius: width / 2
            color: "white"
        }
        FocusRing { target: control; targetRadius: parent.width / 2; visible: control.visualFocus }
    }
}
