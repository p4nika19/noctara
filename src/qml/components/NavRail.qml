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

    // Geometry of the destinations, so the pill's position is a plain binding: it follows resizes (the footer
    // moves with the window height) and never depends on when delegates were created.
    readonly property real itemHeight: Theme.space.controlHeight + Theme.space.md
    readonly property real pillY: {
        let index = destinations.findIndex(d => d.id === current)
        if (index >= 0)
            return Theme.space.md + index * itemHeight + (itemHeight - pill.height) / 2
        index = footer.findIndex(d => d.id === current)
        if (index >= 0)
            return height - Theme.space.md - (footer.length - index) * itemHeight + (itemHeight - pill.height) / 2
        return 0
    }

    property bool gliding: false
    onCurrentChanged: {
        gliding = true
        // If the move did not start an animation (no change in position), do not leave gliding switched on.
        Qt.callLater(() => { if (!glide.running) gliding = false })
    }

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
        height: root.itemHeight

        IconButton {
            anchors.centerIn: parent
            implicitWidth: Theme.space.controlHeight + Theme.space.xs
            implicitHeight: Theme.space.controlHeight + Theme.space.xxs
            // The moving pill is the selection; a hover surface on the newly selected item would appear
            // instantly and look like a second highlight while the pill is still on its way.
            surface: !item.selected
            material: "flat"
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
    }

    // Active item: lit glass with a light edge and an accent halo (no overshoot when it moves).
    Item {
        id: pill
        visible: root.current.length > 0
        x: (root.width - width) / 2
        y: root.pillY
        width: Theme.space.controlHeight + Theme.space.xs
        height: Theme.space.controlHeight + Theme.space.xxs
        // Glide only when the selection changes; resizes and startup place it directly.
        Behavior on y {
            enabled: root.gliding
            NumberAnimation {
                id: glide
                duration: Theme.motion.slow
                easing.type: Theme.motion.easingEmphasized
                onRunningChanged: if (!running) root.gliding = false
            }
        }

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
