import QtQuick
import Noctara

// Floating glass navigation rail. destinations: [{id, icon, label}]; footer holds Settings.
// Right-clicking a destination emits secondaryActivated (the gear opens the quick Appearance panel).
GlassPanel {
    id: root

    property var destinations: []
    property var footer: []
    property string current
    signal navigate(string id)
    signal secondaryActivated(string id)

    level: 0
    liquid: true
    radius: Theme.radius.lg
    implicitWidth: Theme.space.navRailWidth
    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Navigation")

    component Destination: Item {
        id: item
        required property var modelData
        readonly property bool selected: root.current === modelData.id
        width: root.width
        height: Theme.space.controlHeight + Theme.space.md

        IconButton {
            anchors.centerIn: parent
            implicitWidth: Theme.space.controlHeight + Theme.space.xs
            implicitHeight: Theme.space.controlHeight + Theme.space.xxs
            iconName: item.modelData.icon
            iconSize: Theme.space.iconSize
            iconColor: item.selected ? Theme.targetColors.accent : Theme.targetColors.iconSecondary
            text: item.modelData.label
            Accessible.role: Accessible.PageTab
            Accessible.checked: item.selected
            onClicked: root.navigate(item.modelData.id)
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: root.secondaryActivated(item.modelData.id)
            }
        }
        // RestoreNone: when the previous destination's binding deactivates it must not restore the value it
        // had overwritten, or the pill jumps back to it depending on evaluation order.
        Binding {
            when: item.selected
            restoreMode: Binding.RestoreNone
            target: pill
            property: "y"
            value: item.parent.y + item.y + (item.height - pill.height) / 2
        }
    }

    // Active item: lit glass with a light edge and an accent halo (no overshoot when it moves).
    Item {
        id: pill
        visible: root.current.length > 0
        x: (root.width - width) / 2
        width: Theme.space.controlHeight + Theme.space.xs
        height: Theme.space.controlHeight + Theme.space.xxs
        Behavior on y { NumberAnimation { duration: Theme.motion.slow; easing.type: Theme.motion.easingEmphasized } }

        Shadow {
            anchors.fill: parent
            z: -1
            radius: pillFill.radius
            blur: 30 * Theme.scale
            spread: -10 * Theme.scale
            offsetY: 8 * Theme.scale
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                           Theme.glass.glowControls ? Theme.glass.glow * 0.9 : 0)
        }
        Rectangle {
            id: pillFill
            anchors.fill: parent
            radius: Math.min(Theme.radius.button, height / 2)
            border.width: Theme.space.hairline
            border.color: Theme.dark ? Qt.rgba(1, 1, 1, 0.15)
                                     : Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.3)
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.dark ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0.95) }
                GradientStop { position: 1.0; color: Theme.dark ? Qt.rgba(1, 1, 1, 0.04) : Qt.rgba(1, 1, 1, 0.6) }
            }
        }
    }

    Column {
        anchors.top: parent.top
        anchors.topMargin: Theme.space.md
        width: parent.width
        Repeater {
            model: root.destinations
            delegate: Destination {}
        }
    }
    Column {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space.md
        width: parent.width
        Repeater {
            model: root.footer
            delegate: Destination {}
        }
    }
}
