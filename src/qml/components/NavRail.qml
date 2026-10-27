import QtQuick
import Noctara

// Floating glass navigation rail. destinations: [{id, icon, label}]; footer holds Settings.
GlassPanel {
    id: root

    property var destinations: []
    property var footer: []
    property string current
    signal navigate(string id)

    level: 1
    radius: Theme.radius.lg
    implicitWidth: Theme.space.navRailWidth
    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Navigation")

    component Destination: Item {
        id: item
        required property var modelData
        readonly property bool selected: root.current === modelData.id
        width: root.width
        height: Theme.space.controlHeight + Theme.space.lg

        IconButton {
            id: button
            anchors.centerIn: parent
            implicitWidth: Theme.space.controlHeight + Theme.space.sm
            implicitHeight: Theme.space.controlHeight
            iconName: item.modelData.icon
            iconSize: Theme.space.iconSize
            iconColor: item.selected ? Theme.targetColors.onAccentContainer : Theme.targetColors.iconSecondary
            text: item.modelData.label
            Accessible.role: Accessible.PageTab
            Accessible.checked: item.selected
            onClicked: root.navigate(item.modelData.id)
        }
        Binding {
            when: item.selected
            target: pill
            property: "y"
            value: item.parent.y + item.y + (item.height - pill.height) / 2
        }
    }

    // Selection pill slides between destinations.
    Rectangle {
        id: pill
        visible: root.current.length > 0
        x: (root.width - width) / 2
        width: Theme.space.controlHeight + Theme.space.sm
        height: Theme.space.controlHeight
        radius: height / 2
        color: Theme.colors.accentContainer
        Behavior on y { NumberAnimation { duration: Theme.motion.slow; easing.type: Theme.motion.easingEmphasized } }
    }

    Column {
        anchors.top: parent.top
        anchors.topMargin: Theme.space.md
        width: parent.width
        Repeater {
            id: top
            model: root.destinations
            delegate: Destination {}
        }
    }
    Column {
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.space.md
        width: parent.width
        Repeater {
            id: bottom
            model: root.footer
            delegate: Destination {}
        }
    }
}
