import QtQuick
import Noctara

// "Name    value" line of the status blocks at the top of settings pages.
Row {
    id: root

    property string name
    property string info
    property color infoColor: Theme.colors.textPrimary
    readonly property real nameWidth: Math.min(200 * Theme.scale, width * 0.35)

    spacing: Theme.space.md
    Text {
        width: root.nameWidth
        text: root.name
        font: Theme.font.label
        color: Theme.colors.textSecondary
        wrapMode: Text.Wrap
    }
    Text {
        width: root.width - root.nameWidth - root.spacing
        text: root.info
        font: Theme.font.mono
        color: root.infoColor
        wrapMode: Text.Wrap
    }
}
