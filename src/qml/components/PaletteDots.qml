import QtQuick
import Noctara

// The three palette colours (accent, light, shadow) with their hex codes, as glowing dots.
Flow {
    id: root

    property bool labelled: true

    spacing: Theme.space.lg
    Accessible.role: Accessible.StaticText
    Accessible.name: qsTr("Current palette")

    // Fixed instances: a Repeater over a model built from the colours rebuilt every dot (and its shadow) on each
    // frame of a colour crossfade.
    component Dot: Row {
        id: dot
        property string label
        property color tone
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
                color: Qt.rgba(dot.tone.r, dot.tone.g, dot.tone.b, 0.66)
            }
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: dot.tone
                border.width: 2 * Theme.scale
                border.color: Qt.rgba(1, 1, 1, 0.22)
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: (root.labelled ? dot.label + " " : "") + dot.tone.toString().toUpperCase()
            font: Theme.font.monoSmall
            color: Theme.colors.textTertiary
        }
    }

    Dot {
        label: qsTr("Accent", "palette colour")
        tone: Theme.colors.accent
    }
    Dot {
        label: qsTr("Light", "palette colour: the lighter companion of the accent")
        tone: Theme.colors.accentLight
    }
    Dot {
        label: qsTr("Shadow", "palette colour: the deep shade of the accent")
        tone: Theme.colors.accentDeep
    }
}
