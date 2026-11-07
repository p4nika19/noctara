import QtQuick
import Noctara

// One notification. kind: "info" | "success" | "warning" | "error".
GlassPanel {
    id: root

    property string kind: "info"
    property string message
    property string actionLabel
    property string actionId
    signal dismissed()
    readonly property real maxWidth: 480 * Theme.scale

    level: 2
    liquid: true
    radius: Theme.radius.md
    implicitWidth: Math.min(maxWidth, layout.implicitWidth + Theme.space.lg * 2)
    implicitHeight: layout.implicitHeight + Theme.space.md * 2
    Accessible.role: Accessible.AlertMessage
    Accessible.name: message

    readonly property color kindColor: kind === "error" ? Theme.targetColors.danger
                                      : kind === "warning" ? Theme.targetColors.warning
                                      : kind === "success" ? Theme.targetColors.success : Theme.targetColors.accent

    Row {
        id: layout
        x: Theme.space.lg
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.md

        Icon {
            id: kindIcon
            anchors.verticalCenter: parent.verticalCenter
            name: root.kind === "error" || root.kind === "warning" ? "triangle-alert"
                : root.kind === "success" ? "check" : "info"
            color: root.kindColor
        }
        Text {
            // What the icon and the buttons leave of the widest panel: the message wraps instead of pushing them out.
            width: Math.min(implicitWidth, 300 * Theme.scale,
                            root.maxWidth - Theme.space.lg * 2 - kindIcon.width - dismiss.width - layout.spacing * 2
                            - (root.actionLabel.length > 0 ? action.width + layout.spacing : 0))
            anchors.verticalCenter: parent.verticalCenter
            text: root.message
            wrapMode: Text.Wrap
            font: Theme.font.body
            color: Theme.colors.textPrimary
        }
        Button {
            id: action
            visible: root.actionLabel.length > 0
            anchors.verticalCenter: parent.verticalCenter
            small: true
            text: root.actionLabel
            onClicked: {
                App.triggerAction(root.actionId)
                root.dismissed()
            }
        }
        IconButton {
            id: dismiss
            anchors.verticalCenter: parent.verticalCenter
            iconName: "x"
            iconSize: Theme.space.iconSizeSmall
            implicitWidth: Theme.space.controlHeightSmall
            implicitHeight: Theme.space.controlHeightSmall
            text: qsTr("Dismiss")
            onClicked: root.dismissed()
        }
    }
}
