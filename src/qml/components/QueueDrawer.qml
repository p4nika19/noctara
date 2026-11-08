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

        MouseArea {  // a list at its end leaves the wheel unhandled: it must not reach the volume control behind
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            onWheel: wheel => wheel.accepted = true
        }

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

        QueueList {
            id: list
            anchors.top: head.bottom
            anchors.topMargin: Theme.space.md
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Math.max(Theme.space.md, panel.cornerInset)
            anchors.left: parent.left
            anchors.leftMargin: Math.max(Theme.space.sm, panel.cornerInset)
            anchors.right: parent.right
            anchors.rightMargin: Math.max(Theme.space.sm, panel.cornerInset)
            active: root.open || panel.opacity > 0
            onActiveChanged: if (root.open) centerOnCurrent()
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
}
