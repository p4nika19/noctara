import QtQuick
import Noctara
import "../components"

// Edits layout.navigation.items: which pages the navigation shows, and their order (up/down buttons).
Column {
    id: root

    property var value: []
    signal picked(var items)

    readonly property var all: [
        { id: "home", icon: "house", label: qsTr("Home") },
        { id: "search", icon: "search", label: qsTr("Search") },
        { id: "library", icon: "library", label: qsTr("Library") },
        { id: "likes", icon: "heart", label: qsTr("Likes") },
        { id: "history", icon: "clock", label: qsTr("History") },
        { id: "charts", icon: "trending-up", label: qsTr("Charts") },
        { id: "downloads", icon: "download", label: qsTr("Downloaded") }
    ]
    // Shown pages in their order, then the hidden ones.
    readonly property var ordered: value.map(id => all.find(p => p.id === id)).filter(p => p !== undefined)
                                   .concat(all.filter(p => value.indexOf(p.id) < 0))

    function move(id, delta) {
        const items = value.slice()
        const at = items.indexOf(id)
        const to = at + delta
        if (at < 0 || to < 0 || to >= items.length)
            return
        items.splice(at, 1)
        items.splice(to, 0, id)
        picked(items)
    }
    function toggle(id, shown) {
        const items = value.filter(v => v !== id)
        if (shown)
            items.push(id)
        picked(items)
    }

    width: parent ? parent.width : implicitWidth
    spacing: Theme.space.xxs

    Repeater {
        model: root.ordered
        delegate: Item {
            required property var modelData
            readonly property int position: root.value.indexOf(modelData.id)
            readonly property bool shown: position >= 0
            width: root.width
            height: Theme.space.controlHeight
            Icon {
                id: icon
                anchors.left: parent.left
                anchors.leftMargin: Theme.space.sm
                anchors.verticalCenter: parent.verticalCenter
                name: parent.modelData.icon
                color: parent.shown ? Theme.targetColors.iconPrimary : Theme.targetColors.iconSecondary
            }
            Text {
                anchors.left: icon.right
                anchors.leftMargin: Theme.space.md
                anchors.right: buttons.left
                anchors.verticalCenter: parent.verticalCenter
                text: parent.modelData.label
                font: Theme.font.body
                color: parent.shown ? Theme.colors.textPrimary : Theme.colors.textTertiary
                elide: Text.ElideRight
            }
            Row {
                id: buttons
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space.xs
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.space.controlHeightSmall
                    height: width
                    iconSize: Theme.space.iconSizeSmall
                    iconName: "chevron-up"
                    enabled: parent.parent.shown && parent.parent.position > 0
                    text: qsTr("Move up")
                    onClicked: root.move(parent.parent.modelData.id, -1)
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.space.controlHeightSmall
                    height: width
                    iconSize: Theme.space.iconSizeSmall
                    iconName: "chevron-down"
                    enabled: parent.parent.shown && parent.parent.position < root.value.length - 1
                    text: qsTr("Move down")
                    onClicked: root.move(parent.parent.modelData.id, 1)
                }
                Switch {
                    anchors.verticalCenter: parent.verticalCenter
                    checked: parent.parent.shown
                    Accessible.name: qsTr("Show %1").arg(parent.parent.modelData.label)
                    onToggled: root.toggle(parent.parent.modelData.id, checked)
                }
            }
        }
    }
}
