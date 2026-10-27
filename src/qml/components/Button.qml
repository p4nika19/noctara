import QtQuick
import QtQuick.Templates as T
import Noctara

// Text button. variant: "primary" (accent gradient + glow) | "secondary" (button material) |
// "ghost" (flat, accent text) | "danger".
T.Button {
    id: control

    property string variant: "secondary"
    property string iconName
    property bool small: false  // compact size for secondary actions inside headers

    readonly property color labelColor: variant === "primary" ? Theme.colors.onAccent
                                       : variant === "danger" ? Theme.colors.danger
                                       : variant === "ghost" ? Theme.colors.accentText
                                       : Theme.colors.textPrimary

    implicitWidth: Math.max(implicitBackgroundWidth, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: small ? Theme.space.controlHeightSmall - Theme.space.xs : Theme.space.controlHeight
    leftPadding: small ? Theme.space.md : Theme.space.lg + Theme.space.xxs
    rightPadding: small ? Theme.space.md : Theme.space.lg + Theme.space.xxs
    spacing: Theme.space.sm
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    font: small ? Theme.font.caption : Theme.font.label
    Accessible.name: text

    scale: down ? 0.96 : 1
    Behavior on scale { NumberAnimation { duration: Theme.motion.fast; easing.type: Theme.motion.easingEmphasized } }

    background: ButtonSurface {
        material: control.variant === "primary" ? "primary"
                : control.variant === "ghost" ? "flat"
                : control.variant === "danger" ? "tinted" : Theme.glass.buttons
        accent: control.variant === "danger" ? Theme.colors.danger : Theme.colors.accent
        hovered: control.hovered && control.enabled
        pressed: control.down
        opacity: control.enabled ? 1 : 0.45
        FocusRing { target: control; targetRadius: parent.radius; visible: control.visualFocus }
    }
    contentItem: Row {
        spacing: control.spacing
        Icon {
            visible: control.iconName.length > 0
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            size: Theme.space.iconSizeSmall
            color: control.variant === "primary" ? Theme.targetColors.onAccent
                 : control.variant === "ghost" ? Theme.targetColors.accent : Theme.targetColors.iconPrimary
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
