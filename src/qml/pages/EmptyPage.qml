import QtQuick
import Noctara
import "../components"

// Placeholder for destinations whose content is not available yet.
Item {
    id: root

    property string iconName
    property string title
    property string message

    Column {
        anchors.centerIn: parent
        width: Math.min(parent.width, 420 * Theme.scale)
        spacing: Theme.space.md
        Icon {
            anchors.horizontalCenter: parent.horizontalCenter
            name: root.iconName
            size: Theme.space.iconSizeLarge * 1.6
            color: Theme.targetColors.iconSecondary
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.title
            font: Theme.font.title
            color: Theme.colors.textPrimary
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.message
            font: Theme.font.body
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }
    }
}
