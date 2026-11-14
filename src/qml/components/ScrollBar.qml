import QtQuick
import QtQuick.Templates as T
import Noctara

T.ScrollBar {
    id: control

    // Space kept free at the end (a list that runs on under the player bar keeps its bar above it).
    property real endInset: 0

    implicitWidth: 10 * Theme.scale
    implicitHeight: 10 * Theme.scale
    padding: 2 * Theme.scale
    bottomPadding: padding + (orientation === Qt.Vertical ? endInset : 0)
    minimumSize: 0.08
    visible: policy !== T.ScrollBar.AlwaysOff && size < 1.0

    contentItem: Rectangle {
        radius: width / 2
        color: control.pressed ? Theme.colors.textSecondary : Theme.colors.textTertiary
        opacity: control.active ? 0.8 : 0.0
        Behavior on opacity { NumberAnimation { duration: Theme.motion.slow } }
    }
}
