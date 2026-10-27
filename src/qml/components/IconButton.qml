import QtQuick
import QtQuick.Templates as T
import Noctara

// Round icon-only button. `text` is the accessible name and tooltip (always translated by the caller).
T.AbstractButton {
    id: control

    property string iconName
    property real iconSize: Theme.space.iconSize
    property bool highlighted: false
    property color iconColor: checked ? Theme.targetColors.accent
                                      : (highlighted ? Theme.targetColors.onAccentContainer : Theme.targetColors.iconPrimary)

    implicitWidth: Theme.space.controlHeight
    implicitHeight: Theme.space.controlHeight
    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    Accessible.name: text

    background: Rectangle {
        radius: height / 2
        color: control.highlighted ? Theme.colors.accentContainer : "transparent"
        StateLayer {
            radius: parent.radius
            hovered: control.hovered && control.enabled
            pressed: control.down
        }
        FocusRing { target: control; targetRadius: parent.radius; visible: control.visualFocus }
    }
    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: control.iconName
            size: control.iconSize
            color: control.iconColor
            opacity: control.enabled ? 1 : 0.4
            scale: control.down ? 0.86 : 1
            Behavior on scale {
                NumberAnimation { duration: Theme.motion.fast; easing.type: Theme.motion.easingEmphasized }
            }
        }
    }

    Tooltip {
        visible: control.hovered && control.text.length > 0
        text: control.text
    }
}
