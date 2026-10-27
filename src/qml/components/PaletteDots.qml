import QtQuick
import Noctara

// The three palette colours (accent, light, shadow) with their hex codes, as glowing dots.
Flow {
    id: root

    property bool labelled: true

    spacing: Theme.space.lg
    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("Current palette")

    Repeater {
        model: [
            { label: qsTr("Accent", "palette colour"), color: Theme.colors.accent },
            { label: qsTr("Light", "palette colour: the lighter companion of the accent"), color: Theme.colors.accentLight },
            { label: qsTr("Shadow", "palette colour: the deep shade of the accent"), color: Theme.colors.accentDeep }
        ]
        delegate: Row {
            id: dot
            required property var modelData
            spacing: Theme.space.sm
            Item {
                width: 18 * Theme.scale
                height: width
                anchors.verticalCenter: parent.verticalCenter
                Shadow {
                    anchors.fill: parent
                    z: -1
                    radius: width / 2
                    blur: 14 * Theme.scale
                    color: Qt.rgba(dot.modelData.color.r, dot.modelData.color.g, dot.modelData.color.b, 0.66)
                }
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: dot.modelData.color
                    border.width: 2 * Theme.scale
                    border.color: Qt.rgba(1, 1, 1, 0.22)
                }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: (root.labelled ? dot.modelData.label + " " : "") + dot.modelData.color.toString().toUpperCase()
                font: Theme.font.monoSmall
                color: Theme.colors.textTertiary
            }
        }
    }
}
