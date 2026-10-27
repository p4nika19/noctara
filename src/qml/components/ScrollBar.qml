import QtQuick
import QtQuick.Templates as T
import Noctara

T.ScrollBar {
    id: control

    implicitWidth: 10 * Theme.scale
    implicitHeight: 10 * Theme.scale
    padding: 2 * Theme.scale
    minimumSize: 0.08
    visible: policy !== T.ScrollBar.AlwaysOff && size < 1.0

    contentItem: Rectangle {
        radius: width / 2
        color: control.pressed ? Theme.colors.textSecondary : Theme.colors.textTertiary
        opacity: control.active ? 0.8 : 0.0
        Behavior on opacity { NumberAnimation { duration: Theme.motion.slow } }
    }
}
