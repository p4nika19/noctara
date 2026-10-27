import QtQuick
import QtQuick.Templates as T
import Noctara

T.TextField {
    id: control

    property string iconName
    property bool monospace: false

    implicitWidth: Math.max(220 * Theme.scale, implicitBackgroundWidth)
    implicitHeight: Theme.space.controlHeight
    leftPadding: iconName.length > 0 ? Theme.space.xl + Theme.space.lg : Theme.space.lg
    rightPadding: Theme.space.md
    verticalAlignment: TextInput.AlignVCenter
    font: monospace ? Theme.font.mono : Theme.font.body
    color: Theme.colors.textPrimary
    placeholderTextColor: Theme.colors.textTertiary
    selectionColor: Theme.colors.selection
    selectedTextColor: Theme.colors.textPrimary
    hoverEnabled: true
    Accessible.name: placeholderText

    Text {
        x: control.leftPadding
        width: control.width - control.leftPadding - control.rightPadding
        height: control.height
        verticalAlignment: Text.AlignVCenter
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        elide: Text.ElideRight
        visible: control.length === 0 && !control.preeditText
    }

    background: Rectangle {
        radius: Math.min(Theme.radius.button, height / 2)
        color: control.hovered || control.activeFocus ? Theme.colors.surfaceRaised : Theme.colors.surfaceBase
        border.width: Theme.space.hairline
        border.color: control.activeFocus ? Theme.colors.focusRing : Theme.colors.edge
        Rectangle {
            anchors.fill: parent
            anchors.margins: Theme.space.hairline
            radius: parent.radius
            visible: Theme.glass.translucent
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.colors.sheen }
                GradientStop { position: 0.45; color: "transparent" }
            }
        }
        Icon {
            visible: control.iconName.length > 0
            anchors.left: parent.left
            anchors.leftMargin: Theme.space.md + Theme.space.xxs
            anchors.verticalCenter: parent.verticalCenter
            name: control.iconName
            size: Theme.space.iconSizeSmall
            color: Theme.targetColors.iconSecondary
        }
    }
}
