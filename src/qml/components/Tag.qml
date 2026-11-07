import QtQuick
import Noctara

// A small outlined label ("Preview", "Not cacheable") with an optional explanation on hover.
Rectangle {
    id: root

    property string text
    property string tooltip

    implicitWidth: label.implicitWidth + Theme.space.sm * 2
    implicitHeight: label.implicitHeight + Theme.space.xxs * 2
    radius: height / 2
    color: "transparent"
    border.width: Theme.space.hairline
    border.color: Theme.colors.outline
    Accessible.role: Accessible.StaticText
    Accessible.name: tooltip.length > 0 ? qsTr("%1: %2").arg(text).arg(tooltip) : text

    Text {
        id: label
        anchors.centerIn: parent
        text: root.text
        font: Theme.font.caption
        color: Theme.colors.textSecondary
    }

    HoverHandler {
        id: hover
        enabled: root.tooltip.length > 0
    }

    LazyTooltip {
        host: root
        shown: hover.hovered
        text: root.tooltip
    }
}
