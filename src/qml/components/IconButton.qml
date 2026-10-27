import QtQuick
import QtQuick.Templates as T
import Noctara

// Round icon-only button. `text` is the accessible name and tooltip (always translated by the caller).
// material: the "Button material" setting by default, "flat" for toolbar-style buttons (navigation rail), or
// any ButtonSurface material such as "primary".
T.AbstractButton {
    id: control

    property string iconName
    property real iconSize: Theme.space.iconSize
    property string material: Theme.glass.buttons
    property bool highlighted: false
    // False: no hover/press surface or halo (the owner draws its own selection, e.g. the navigation pill).
    property bool surface: true
    property color iconColor: material === "primary" ? Theme.targetColors.onAccent
                            : (checked || highlighted ? Theme.targetColors.accent : Theme.targetColors.iconPrimary)

    implicitWidth: Theme.space.controlHeight
    implicitHeight: Theme.space.controlHeight
    focusPolicy: Qt.TabFocus
    hoverEnabled: true
    Accessible.name: text

    scale: down ? 0.94 : 1
    Behavior on scale { NumberAnimation { duration: Theme.motion.fast; easing.type: Theme.motion.easingEmphasized } }

    background: ButtonSurface {
        material: control.material
        radius: Math.min(Theme.radius.button, height / 2)
        hovered: control.surface && control.hovered && control.enabled
        pressed: control.surface && control.down
        active: control.checked || control.highlighted
        FocusRing { target: control; targetRadius: parent.radius; visible: control.visualFocus }
    }
    contentItem: Item {
        Icon {
            anchors.centerIn: parent
            name: control.iconName
            size: control.iconSize
            color: control.iconColor
            opacity: control.enabled ? 1 : 0.4
        }
    }

    Tooltip {
        visible: control.hovered && control.text.length > 0
        text: control.text
    }
}
