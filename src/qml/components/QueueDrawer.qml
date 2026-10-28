import QtQuick
import Noctara

// The play queue beside the content: what played, what plays now and what comes next. Click an item to play it;
// hovering shows a button to remove it. Rows are plain items over the drawer's glass, never glass themselves.
FocusScope {
    id: root

    property bool open: false
    signal closeRequested()

    readonly property QueueModel queue: Player.queue

    visible: panel.opacity > 0
    focus: open
    Keys.onEscapePressed: root.closeRequested()

    GlassPanel {
        id: panel
        level: 2
        liquid: true
        radius: Theme.radius.lg
        width: Math.min(380 * Theme.scale, root.width)
        height: root.height
        x: root.open ? root.width - width : root.width - width + 28 * Theme.scale
        opacity: root.open ? 1 : 0
        Behavior on x { NumberAnimation { duration: Theme.motion.slow; easing.type: Theme.motion.easingStandard } }
        Behavior on opacity { NumberAnimation { duration: Theme.motion.normal } }
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Queue")

        Item {
            id: head
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Math.max(Theme.space.lg, panel.cornerInset)
            height: Math.max(titles.implicitHeight, close.implicitHeight)
            Column {
                id: titles
                anchors.left: parent.left
                anchors.right: actions.left
                anchors.rightMargin: Theme.space.sm
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space.xxs
                Text {
                    width: parent.width
                    text: qsTr("Queue")
                    font: Theme.font.title
                    color: Theme.colors.textPrimary
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: qsTr("%n track(s)", "", root.queue.count)
                    font: Theme.font.caption
                    color: Theme.colors.textTertiary
                    elide: Text.ElideRight
                }
            }
            Row {
                id: actions
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space.xs
                IconButton {
                    iconName: "list-x"
                    visible: root.queue.count > root.queue.currentIndex + 1
                    text: qsTr("Clear upcoming tracks")
                    onClicked: Player.clearUpcoming()
                }
                IconButton {
                    id: close
                    iconName: "x"
                    text: qsTr("Close queue")
                    onClicked: root.closeRequested()
                }
            }
        }

        ListView {
            id: list
            anchors.top: head.bottom
            anchors.topMargin: Theme.space.md
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Math.max(Theme.space.md, panel.cornerInset)
            anchors.left: parent.left
            anchors.leftMargin: Math.max(Theme.space.sm, panel.cornerInset)
            anchors.right: parent.right
            anchors.rightMargin: Math.max(Theme.space.sm, panel.cornerInset)
            clip: true
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.open || panel.opacity > 0 ? root.queue : null
            currentIndex: root.queue.currentIndex
            highlightFollowsCurrentItem: false
            ScrollBar.vertical: ScrollBar {}
            delegate: QueueRow {
                width: ListView.view.width
            }
            onCountChanged: if (root.open) positionViewAtIndex(Math.max(0, currentIndex), ListView.Contain)
            Connections {
                target: root
                function onOpenChanged() {
                    if (root.open)
                        list.positionViewAtIndex(Math.max(0, root.queue.currentIndex), ListView.Center)
                }
            }
        }

        Text {
            anchors.centerIn: list
            width: list.width - Theme.space.xl * 2
            visible: root.queue.count === 0
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("Nothing queued. Play a track or a playlist and it shows up here.")
            font: Theme.font.body
            color: Theme.colors.textSecondary
        }
    }

    component QueueRow: Item {
        id: row
        required property int index
        required property string title
        required property string subtitle
        required property string thumbnail
        required property var duration
        required property bool current
        required property bool protectedMedia
        required property bool pending

        implicitHeight: Theme.space.rowHeight
        Accessible.role: Accessible.ListItem
        Accessible.name: pending ? qsTr("Loading") : qsTr("%1, %2").arg(title).arg(subtitle)

        StateLayer {
            radius: Theme.radius.sm
            hovered: area.containsMouse
            pressed: area.pressed
        }
        Rectangle {
            anchors.fill: parent
            radius: Theme.radius.sm
            visible: row.current
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.16)
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: Player.playQueueItem(row.index)
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
