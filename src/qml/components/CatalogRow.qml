import QtQuick
import Noctara

// One row of a CollectionModel: a track, playlist or user. Rows are plain items over the list's surface, never
// glass panels. Protected (DRM) tracks are marked "Not cacheable"; previews are marked as such. A track that is
// still being fetched (a playlist's later tracks) shows skeleton text. Plays, likes, reposts and duration follow
// the Layout → Track rows settings; hovering a track offers "Add to queue"; right-click asks for the menu.
Item {
    id: root

    required property int index
    required property string kind
    required property var itemId
    required property var ownerId
    required property string permalink
    required property var likes
    required property var reposts
    required property string title
    required property string subtitle
    required property string thumbnail
    required property var duration
    required property var plays
    required property var followers
    required property var trackCount
    required property string repostedBy
    required property bool protectedMedia
    required property bool preview
    required property bool verified
    required property bool album
    required property bool pending

    signal activated()
    // Right-click (or the menu key): `point` in this row's coordinates.
    signal menuRequested(point point)
    signal enqueueRequested()

    implicitHeight: Theme.space.rowHeight
    Accessible.role: Accessible.ListItem
    Accessible.name: pending ? qsTr("Loading") : qsTr("%1, %2").arg(title).arg(subtitle)

    // Play/like/repost counter. Fixed instances: a Repeater over a model built from the row's numbers destroyed and
    // recreated all three (icons included) each time a reused row took another track's numbers.
    component Stat: Row {
        id: stat
        property string icon
        property var value
        property bool shown
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        spacing: Theme.space.xs
        visible: shown && value !== undefined
        Icon {
            anchors.verticalCenter: parent.verticalCenter
            name: stat.icon
            size: Theme.space.iconSizeSmall
            color: Theme.targetColors.iconSecondary
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: Format.revision >= 0 && stat.value !== undefined ? Format.count(stat.value) : ""
            font: Theme.font.numeric
            color: Theme.colors.textTertiary
        }
    }

    readonly property string meta: {
        if (Format.revision < 0 || pending) {
            return ""
        }
        if (kind === "user") {
            return followers === undefined ? "" : qsTr("%1 followers").arg(Format.count(followers))
        }
        if (kind === "playlist") {
            return trackCount === undefined ? "" : qsTr("%n track(s)", "", trackCount)
        }
        return ""
    }
    readonly property string secondLine: {
        const parts = [subtitle]
        if (meta.length > 0) {
            parts.push(meta)
        }
        if (repostedBy.length > 0) {
            parts.push(qsTr("reposted by %1").arg(repostedBy))
        }
        if (kind === "playlist" && album) {
            parts.unshift(qsTr("Album"))
        }
        return parts.filter(part => part.length > 0).join(" · ")
    }

    StateLayer {
        radius: Theme.radius.sm
        hovered: area.containsMouse
        pressed: area.pressed
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton)
                root.menuRequested(Qt.point(mouse.x, mouse.y))
            else
                root.activated()
        }
    }
    Keys.onMenuPressed: root.menuRequested(Qt.point(width / 2, height / 2))

    CoverArt {
        id: cover
        anchors.left: parent.left
        anchors.leftMargin: Theme.space.sm
        anchors.verticalCenter: parent.verticalCenter
        width: root.height - Theme.space.sm * 2
        height: width
        source: root.thumbnail
        shape: root.kind === "user" ? "circle" : Theme.coverShape
    }

    Column {
        anchors.left: cover.right
        anchors.leftMargin: Theme.space.md
        anchors.right: trailing.left
        anchors.rightMargin: Theme.space.md
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.xxs
        visible: !root.pending

        Text {
            width: parent.width
            text: root.title
            font: Theme.font.bodyStrong
            color: Theme.colors.textPrimary
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
        Text {
            width: parent.width
            text: root.secondLine
            font: Theme.font.caption
            color: Theme.colors.textSecondary
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
    }

    Column {
        anchors.left: cover.right
        anchors.leftMargin: Theme.space.md
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.sm
        visible: root.pending
        Skeleton { width: 180 * Theme.scale; height: 11 * Theme.scale; radius: height / 2; loading: true }
        Skeleton { width: 110 * Theme.scale; height: 9 * Theme.scale; radius: height / 2; loading: true }
    }

    Row {
        id: trailing
        anchors.right: parent.right
        anchors.rightMargin: Theme.space.md
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.md
        visible: !root.pending

        Tag {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.kind === "track" && root.protectedMedia
            text: qsTr("Not cacheable")
            tooltip: qsTr("Protected track: it plays through the protected player and is never stored on this "
                          + "computer.")
        }
        Tag {
            anchors.verticalCenter: parent.verticalCenter
            visible: root.kind === "track" && root.preview && !root.protectedMedia
            text: qsTr("Preview")
            tooltip: qsTr("Only a 30-second preview is available to your account.")
        }
        Stat {
            icon: "play"
            value: root.plays
            shown: Layout.rowPlays && root.kind === "track"
        }
        Stat {
            icon: "heart"
            value: root.likes
            shown: Layout.rowLikes && root.kind !== "user"
        }
        Stat {
            icon: "repeat"
            value: root.reposts
            shown: Layout.rowReposts && root.kind !== "user"
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.space.controlHeightSmall
            height: width
            // Keeps its place when hidden, so the columns do not move under the pointer.
            readonly property bool shown: area.containsMouse || hovered || activeFocus
            visible: root.kind === "track" && !root.protectedMedia
            opacity: shown ? 1 : 0
            enabled: shown
            material: "flat"
            iconName: "list-plus"
            iconSize: Theme.space.iconSizeSmall
            text: qsTr("Add to queue")
            onClicked: root.enqueueRequested()
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: Layout.rowDuration && root.kind !== "user" && root.duration > 0
            text: Format.revision >= 0 ? Format.duration(root.duration) : ""
            font: Theme.font.numeric
            color: Theme.colors.textSecondary
        }
    }
}
