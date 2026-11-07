import QtQuick
import QtQuick.Window
import Noctara

// The play queue as a list (queue drawer, full-screen player): every item in playing order, the current one
// highlighted. Click plays an item; hovering offers "remove"; right-click opens the item's menu.
ListView {
    id: list

    // False unloads the rows (the drawer is closed).
    property bool active: true
    // The first rows and a new queue (the model is reset) bring the current item into view; removing, moving or
    // adding items keeps the scroll position.
    property int shownCount: 0

    function centerOnCurrent() {
        positionViewAtIndex(Math.max(0, Player.queue.currentIndex), ListView.Center)
    }
    function showCurrent() {
        if (active && count > 0)
            positionViewAtIndex(Math.max(0, Player.queue.currentIndex), ListView.Contain)
    }

    clip: true
    reuseItems: true
    boundsBehavior: Flickable.StopAtBounds
    model: active ? Player.queue : null
    ScrollBar.vertical: ScrollBar {}
    onCountChanged: {
        if (shownCount === 0)
            showCurrent()
        shownCount = count
    }
    Connections {
        target: list.model
        // After the view has rebuilt its rows.
        function onModelReset() { Qt.callLater(list.showCurrent) }
    }

    delegate: Item {
        id: row
        required property int index
        required property string title
        required property string subtitle
        required property string thumbnail
        required property var duration
        required property bool current
        required property bool protectedMedia
        required property bool pending
        required property string permalink
        required property var artistId
        required property var trackId

        width: ListView.view.width
        implicitHeight: Theme.space.rowHeight
        Accessible.role: Accessible.ListItem
        Accessible.name: pending ? qsTr("Loading") : qsTr("%1, %2").arg(title).arg(subtitle)

        // Concentric with the cover, which sits Theme.space.sm inside the row.
        readonly property real rowRadius: Math.min(height / 2, Theme.radius.sm + Theme.space.sm)
        StateLayer {
            radius: row.rowRadius
            hovered: area.containsMouse
            pressed: area.pressed
        }
        Rectangle {
            anchors.fill: parent
            radius: row.rowRadius
            visible: row.current
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.16)
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: mouse => {
                if (mouse.button === Qt.RightButton)
                    Window.window.showQueueMenu(row.index, row.permalink, row.artistId, row.trackId, row,
                                                Qt.point(mouse.x, mouse.y))
                else
                    Player.playQueueItem(row.index)
            }
        }
        CoverArt {
            id: art
            anchors.left: parent.left
            anchors.leftMargin: Theme.space.sm
            anchors.verticalCenter: parent.verticalCenter
            width: row.height - Theme.space.sm * 2
            height: width
            source: row.thumbnail
        }
        Column {
            anchors.left: art.right
            anchors.leftMargin: Theme.space.md
            anchors.right: trailing.left
            anchors.rightMargin: Theme.space.sm
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space.xxs
            Text {
                width: parent.width
                text: row.pending ? qsTr("Loading…") : row.title
                font: row.current ? Theme.font.bodyStrong : Theme.font.body
                color: row.current ? Theme.colors.accent : Theme.colors.textPrimary
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
            Text {
                width: parent.width
                text: row.protectedMedia ? qsTr("%1 · Protected").arg(row.subtitle) : row.subtitle
                font: Theme.font.caption
                color: Theme.colors.textSecondary
                elide: Text.ElideRight
                textFormat: Text.PlainText
            }
        }
        Item {
            id: trailing
            anchors.right: parent.right
            anchors.rightMargin: Theme.space.xs
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(time.implicitWidth, remove.width)
            height: remove.height
            Text {
                id: time
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !area.containsMouse && !remove.hovered && row.duration > 0
                text: Format.revision >= 0 ? Format.duration(row.duration) : ""
                font: Theme.font.numeric
                color: Theme.colors.textTertiary
            }
            IconButton {
                id: remove
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.space.controlHeightSmall
                height: width
                material: "flat"
                iconName: "x"
                iconSize: Theme.space.iconSizeSmall
                visible: area.containsMouse || hovered || activeFocus
                text: qsTr("Remove from queue")
                onClicked: Player.removeQueueItem(row.index)
            }
        }
    }
}
