import QtQuick
import QtQuick.Window
import Noctara

// The play queue as a list (queue drawer, full-screen player): every item in playing order, the current one
// highlighted. Click plays an item; hovering offers "remove"; right-click opens the item's menu. Dragging a row
// moves it: the other rows make room while it is dragged (only on screen), and the queue itself changes once, when
// it is dropped. Near the top or bottom edge the list scrolls along.
ListView {
    id: list

    // False unloads the rows (the drawer is closed).
    property bool active: true
    // The first rows and a new queue (the model is reset) bring the current item into view; removing, moving or
    // adding items keeps the scroll position.
    property int shownCount: 0

    // --- Dragging a row --------------------------------------------------------------------------
    readonly property real rowHeight: Theme.space.rowHeight
    property int dragFrom: -1  // the row being dragged, -1 while none is
    property real dragStart: 0  // where the pointer took it, in content coordinates
    property real pointerY: 0  // the pointer, in view coordinates
    readonly property real dragOffset: dragFrom < 0 ? 0 : pointerY + contentY - dragStart
    readonly property int dragTo: dragFrom < 0 ? -1
                                  : Math.max(0, Math.min(count - 1, dragFrom + Math.round(dragOffset / rowHeight)))

    // How far a row moves aside to make room for the dragged one.
    function shiftFor(index) {
        if (dragFrom < 0 || index === dragFrom)
            return 0
        if (dragFrom < dragTo && index > dragFrom && index <= dragTo)
            return -rowHeight
        if (dragTo < dragFrom && index >= dragTo && index < dragFrom)
            return rowHeight
        return 0
    }
    function beginDrag(index, y) {
        pointerY = y
        dragStart = y + contentY
        dragFrom = index
    }
    function moveDrag(y) {
        pointerY = y
    }
    // Drops the row where it is (or puts it back): the rows return to their places at once, as the queue moves.
    function endDrag(drop) {
        const from = dragFrom, to = dragTo
        dragFrom = -1
        if (drop && from >= 0 && to >= 0 && to !== from)
            Player.moveQueueItem(from, to)
    }
    onActiveChanged: if (!active) endDrag(false)
    Timer {  // scrolls while the dragged row is held near an edge
        interval: 16
        repeat: true
        running: list.dragFrom >= 0
        onTriggered: {
            const edge = list.rowHeight * 0.75
            const speed = list.rowHeight * 0.25
            let step = 0
            if (list.pointerY < edge)
                step = -speed * (1 - Math.max(0, list.pointerY) / edge)
            else if (list.pointerY > list.height - edge)
                step = speed * (1 - Math.max(0, list.height - list.pointerY) / edge)
            if (step !== 0)
                list.contentY = Math.max(list.originY,
                                         Math.min(list.originY + list.contentHeight - list.height, list.contentY + step))
        }
    }

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
    // Dragging with the mouse moves rows; the wheel and the scroll bar scroll.
    interactive: dragFrom < 0
    // The dragged row stays alive however far the list scrolls under it (released, it would drop the drag).
    cacheBuffer: rowHeight * 6 + (dragFrom >= 0 ? Math.abs(dragOffset) : 0)
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
        implicitHeight: list.rowHeight
        z: dragged ? 2 : 0
        Accessible.role: Accessible.ListItem
        Accessible.name: pending ? qsTr("Loading") : qsTr("%1, %2").arg(title).arg(subtitle)

        readonly property bool dragged: list.dragFrom === index
        // Making room for the dragged row: animated while it moves, back in place at once when it is dropped
        // (the queue moves in the same moment, so nothing visibly jumps).
        readonly property real targetShift: list.shiftFor(index)
        property real shift: 0
        onTargetShiftChanged: {
            if (list.dragFrom < 0) {
                shiftAnimation.stop()
                shift = 0
            } else {
                shiftAnimation.to = targetShift
                shiftAnimation.restart()
            }
        }
        ListView.onReused: {
            shiftAnimation.stop()
            shift = targetShift
        }
        NumberAnimation {
            id: shiftAnimation
            target: row
            property: "shift"
            duration: Theme.motion.fast
            easing.type: Theme.motion.easingStandard
        }
        transform: Translate { y: row.dragged ? list.dragOffset : row.shift }

        // Concentric with the cover, which sits Theme.space.sm inside the row.
        readonly property real rowRadius: Math.min(height / 2, Theme.radius.sm + Theme.space.sm)
        Rectangle {  // the dragged row lifts off the list
            anchors.fill: parent
            radius: row.rowRadius
            visible: row.dragged
            color: Theme.colors.solidOverlay
            border.width: Theme.space.hairline
            border.color: Theme.colors.edge
        }
        StateLayer {
            radius: row.rowRadius
            hovered: area.containsMouse || row.dragged
            pressed: area.pressed && !row.dragged
        }
        Rectangle {
            anchors.fill: parent
            radius: row.rowRadius
            visible: row.current
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.16)
        }
        MouseArea {
            id: area
            property real pressY: 0
            property bool dragging: false
            property bool moved: false  // the press became a drag: its release is no click
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            preventStealing: true  // a drag moves the row; the list must not take it to scroll
            cursorShape: dragging ? Qt.ClosedHandCursor : Qt.ArrowCursor
            onPressed: mouse => {
                pressY = mouse.y
                moved = false
            }
            onPositionChanged: mouse => {
                if (!pressed || !(pressedButtons & Qt.LeftButton))
                    return
                const y = mapToItem(list, mouse.x, mouse.y).y
                if (!dragging && list.count > 1 && Math.abs(mouse.y - pressY) >= Application.styleHints.startDragDistance) {
                    dragging = true
                    moved = true
                    list.beginDrag(row.index, mapToItem(list, mouse.x, pressY).y)
                }
                if (dragging)
                    list.moveDrag(y)
            }
            onReleased: {
                if (dragging) {
                    dragging = false
                    list.endDrag(true)
                }
            }
            onCanceled: {
                if (dragging) {
                    dragging = false
                    list.endDrag(false)
                }
            }
            onClicked: mouse => {
                if (moved)
                    return
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
            width: Math.max(time.implicitWidth, remove.width + (grip.visible ? grip.width + Theme.space.xxs : 0))
            height: remove.height
            Icon {  // the row can be dragged
                id: grip
                anchors.right: remove.left
                anchors.rightMargin: Theme.space.xxs
                anchors.verticalCenter: parent.verticalCenter
                visible: list.count > 1 && (area.containsMouse || row.dragged) && !remove.hovered
                name: "grip-vertical"
                size: Theme.space.iconSizeSmall
                color: Theme.targetColors.iconSecondary
            }
            Text {
                id: time
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !area.containsMouse && !remove.hovered && !row.dragged && row.duration > 0
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
                visible: (area.containsMouse || hovered || activeFocus) && list.dragFrom < 0
                text: qsTr("Remove from queue")
                onClicked: Player.removeQueueItem(row.index)
            }
        }
    }
}
