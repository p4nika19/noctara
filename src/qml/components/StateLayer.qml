import QtQuick
import Noctara

// Hover/press feedback overlay. Animates opacity only, so theme crossfades stay in sync.
Rectangle {
    property bool hovered: false
    property bool pressed: false

    anchors.fill: parent
    color: pressed ? Theme.colors.statePressed : Theme.colors.stateHover
    opacity: hovered || pressed ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.motion.fast } }
}
