import QtQuick
import QtQuick.Templates as T
import Noctara

// Text button. variant: "primary" | "secondary" | "ghost" | "danger".
T.Button {
    id: control

    property string variant: "secondary"
    property string iconName

    readonly property color fillColor: {
        switch (variant) {
        case "primary": return control.down ? Theme.colors.accentPressed
                             : (control.hovered ? Theme.colors.accentHover : Theme.colors.accent)
        case "danger": return Theme.colors.danger
        case "ghost": return "transparent"
        default: return Theme.colors.surfaceRaised
        }
    }
    readonly property color labelColor: variant === "primary" ? Theme.colors.onAccent
                                       : variant === "danger" ? Theme.colors.onAccent
                                       : variant === "ghost" ? Theme.colors.accent
                                       : Theme.colors.textPrimary

    implicitWidth: Math.max(implicitBackgroundWidth, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Theme.space.controlHeight
    leftPadding: Theme.space.lg
    rightPadding: Theme.space.lg
    spacing: Theme.space.sm
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    font: Theme.font.label
    Accessible.name: text

    scale: down ? 0.97 : 1
    Behavior on scale { NumberAnimation { duration: Theme.motion.fast; easing.type: Theme.motion.easingEmphasized } }

    background: Rectangle {
        radius: height / 2
        color: control.fillColor
        border.width: control.variant === "secondary" ? Theme.space.hairline : 0
        border.color: Theme.colors.outline
        opacity: control.enabled ? 1 : 0.45
        StateLayer {
            radius: parent.radius
            hovered: control.hovered && control.variant !== "primary"
            pressed: control.down && control.variant !== "primary"
        }
        FocusRing { target: control; targetRadius: parent.radius; visible: control.visualFocus }
    }
    contentItem: Row {
        spacing: control.spacing
        Icon {
            visible: control.iconName.length > 0
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            size: Theme.space.iconSizeSmall
            color: control.variant === "primary" ? Theme.targetColors.onAccent : Theme.targetColors.iconPrimary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: control.text
            font: control.font
            color: control.labelColor
            elide: Text.ElideRight
            opacity: control.enabled ? 1 : 0.6
        }
    }
}
