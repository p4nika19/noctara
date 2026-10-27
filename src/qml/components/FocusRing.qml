import QtQuick
import Noctara

// Keyboard focus indicator drawn just outside its parent's bounds. `target` decides visibility.
Rectangle {
    property Item target: parent
    property real targetRadius: 0

    anchors.fill: parent
    anchors.margins: -Theme.space.focusRingWidth * 2
    radius: targetRadius + Theme.space.focusRingWidth * 2
    color: "transparent"
    border.width: Theme.space.focusRingWidth
    border.color: Theme.colors.focusRing
    visible: target !== null && target.activeFocus
    Accessible.ignored: true
}
