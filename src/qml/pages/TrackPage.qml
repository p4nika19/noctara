import QtQuick
import QtQuick.Window
import Noctara
import "../components"

// A track: artwork, title and artist, counts, tags and description, its waveform, then related tracks, the
// playlists it is in and its comments.
Item {
    id: root

    required property TrackDetail detail
    property string tab: "related"
    readonly property bool current: Player.hasTrack && Player.trackId === String(detail.trackId)
    readonly property bool liked: Catalog.likedRevision >= 0 && Catalog.isLiked(detail.trackId)

    function loadTab() {
        if (!detail)
            return
        if (tab === "playlists")
            detail.loadPlaylists()
        else if (tab === "comments")
            detail.loadComments()
    }
    onTabChanged: loadTab()
    // The page is kept when one track replaces another (Main.qml's detail Loader); the tab stays.
    onDetailChanged: loadTab()
    Component.onCompleted: {
        Catalog.ensure("likes")
        loadTab()
    }

    function play() {
        if (root.current)
            Player.playPause()
        else
            Player.playCollection(root.detail.self, 0, root.detail.title)
    }

    Component {
        id: header
        Item {
            width: ListView.view ? ListView.view.width : (GridView.view ? GridView.view.width : 0)
            height: column.height + Theme.space.xl
            Column {
                id: column
                width: parent.width
                spacing: Theme.space.lg

                Row {
                    width: parent.width
                    spacing: Theme.space.xl
                    Item {
                        id: coverBox
                        width: Math.min(220 * Theme.scale, parent.width * 0.3)
                        height: width
                        Shadow {
                            anchors.fill: parent
                            z: -1
                            radius: cover.cornerRadius
                            blur: 60 * Theme.scale
                            offsetY: 20 * Theme.scale
                            spread: -8 * Theme.scale
                            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                                           Theme.glass.glowCover && !Theme.glass.flat ? Theme.glass.glow * 0.9 : 0)
                        }
                        CoverArt {
                            id: cover
                            anchors.fill: parent
                            radius: Theme.radius.lg
                            source: root.detail.artwork
                        }
                    }
                    Column {
                        width: parent.width - coverBox.width - parent.spacing
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.space.sm
                        Row {
                            spacing: Theme.space.sm
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Track")
                                font: Theme.font.eyebrow
                                color: Theme.colors.textTertiary
                            }
                            Tag {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: root.detail.isPrivate
                                text: qsTr("Private")
                            }
                            Tag {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: root.detail.preview && !root.detail.protectedMedia
                                text: qsTr("Preview")
                                tooltip: qsTr("Only a 30-second preview is available to your account.")
                            }
                        }
                        Text {
                            width: parent.width
                            text: root.detail.title
                            font: Theme.font.headline
                            color: Theme.colors.textPrimary
                            wrapMode: Text.Wrap
                            maximumLineCount: 3
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            Accessible.role: Accessible.Heading
                        }
                        Text {
                            width: Math.min(implicitWidth, parent.width)
                            text: root.detail.artist
                            font: Theme.font.bodyStrong
                            color: artistArea.containsMouse ? Theme.colors.accent : Theme.colors.textSecondary
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            MouseArea {
                                id: artistArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: if (root.detail.artistId > 0) Window.window.openUser(root.detail.artistId)
                            }
                        }
                        Text {
                            width: parent.width
                            text: Format.revision < 0 ? "" : [
                                root.detail.genre,
                                root.detail.album.length > 0 ? qsTr("from %1").arg(root.detail.album) : "",
                                root.detail.released && !isNaN(root.detail.released) ? Format.date(root.detail.released) : "",
                                root.detail.duration > 0 ? Format.duration(root.detail.duration) : ""
                            ].filter(s => s.length > 0).join(" · ")
                            font: Theme.font.caption
                            color: Theme.colors.textTertiary
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            // Small counts with their plural forms, large ones compact ("26.5K plays").
                            text: Format.revision < 0 ? "" : [
                                root.detail.plays === undefined ? ""
                                : root.detail.plays < 1000 ? qsTr("%n play(s)", "", root.detail.plays)
                                : qsTr("%1 plays").arg(Format.count(root.detail.plays)),
                                root.detail.likes === undefined ? ""
                                : root.detail.likes < 1000 ? qsTr("%n like(s)", "", root.detail.likes)
                                : qsTr("%1 likes").arg(Format.count(root.detail.likes)),
                                root.detail.reposts === undefined ? ""
                                : root.detail.reposts < 1000 ? qsTr("%n repost(s)", "", root.detail.reposts)
                                : qsTr("%1 reposts").arg(Format.count(root.detail.reposts)),
                                root.detail.comments === undefined ? ""
                                : root.detail.comments < 1000 ? qsTr("%n comment(s)", "", root.detail.comments)
                                : qsTr("%1 comments").arg(Format.count(root.detail.comments))
                            ].filter(s => s.length > 0).join(" · ")
                            font: Theme.font.caption
                            color: Theme.colors.textTertiary
                            elide: Text.ElideRight
                        }
                        Row {
                            spacing: Theme.space.sm
                            topPadding: Theme.space.sm
                            Button {
                                variant: "primary"
                                iconName: root.current && Player.playing ? "pause" : "play"
                                text: root.current && Player.playing ? qsTr("Pause") : qsTr("Play")
                                enabled: root.detail.self.count > 0
                                onClicked: root.play()
                            }
                            IconButton {
                                iconName: "heart"
                                checked: root.liked
                                text: root.liked ? qsTr("Remove from likes") : qsTr("Like")
                                enabled: Account.signedIn && root.detail.self.count > 0
                                onClicked: Catalog.toggleLike(root.detail.trackId)
                            }
                            IconButton {
                                id: addButton
                                iconName: "plus"
                                text: qsTr("Add to playlist…")
                                enabled: Account.signedIn && root.detail.self.count > 0
                                onClicked: Window.window.showPlaylistMenu(root.detail.trackId,
                                                                          addButton.mapToItem(null, 0, addButton.height))
                            }
                            IconButton {
                                iconName: "list-start"
                                text: qsTr("Play next")
                                enabled: root.detail.self.count > 0
                                onClicked: Player.enqueueCollection(root.detail.self, 0, true)
                            }
                            IconButton {
                                iconName: "list-plus"
                                text: qsTr("Add to queue")
                                enabled: root.detail.self.count > 0
                                onClicked: Player.enqueueCollection(root.detail.self, 0, false)
                            }
                            IconButton {
                                iconName: "external-link"
                                text: qsTr("Open on SoundCloud")
                                enabled: root.detail.permalink.length > 0
                                onClicked: App.openLink(root.detail.permalink)
                            }
                            IconButton {
                                iconName: "copy"
                                text: qsTr("Copy link")
                                enabled: root.detail.permalink.length > 0
                                onClicked: Window.window.copyLink(root.detail.permalink)
                            }
                        }
                    }
                }

                // The track could not be loaded (offline, removed, private).
                Row {
                    visible: root.detail.errorCode >= 0 && root.detail.title.length === 0
                    spacing: Theme.space.md
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: Format.revision < 0 ? "" : Format.error(root.detail.errorCode)
                        font: Theme.font.body
                        color: Theme.colors.textSecondary
                    }
                    Button {
                        text: qsTr("Try again")
                        onClicked: root.detail.reload()
                    }
                }

                WaveformView {
                    width: parent.width
                    visible: root.detail.waveform.length > 0
                    implicitHeight: 56 * Theme.scale
                    samples: root.detail.waveform
                    progress: root.current && Player.duration > 0 ? Math.min(1, Player.position / Player.duration) : 0
                    storedRanges: root.current ? Player.cachedRanges : []
                    duration: root.detail.duration
                    barWidth: 3 * Theme.scale
                    // Seeks in the playing track; another track starts from the beginning.
                    onSeekRequested: fraction => {
                        if (root.current)
                            Player.seek(fraction * Player.duration)
                        else
                            root.play()
                    }
                }

                Flow {
                    width: parent.width
                    visible: root.detail.tags.length > 0
                    spacing: Theme.space.sm
                    Repeater {
                        model: root.detail.tags
                        delegate: Tag {
                            required property string modelData
                            text: "#" + modelData
                        }
                    }
                }

                Text {
                    id: description
                    property bool expanded: false
                    width: parent.width
                    visible: root.detail.description.length > 0
                    text: root.detail.description
                    font: Theme.font.body
                    color: Theme.colors.textSecondary
                    wrapMode: Text.Wrap
                    maximumLineCount: expanded ? 1000 : 4
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
                Text {
                    visible: description.visible && (description.truncated || description.expanded)
                    text: description.expanded ? qsTr("Show less") : qsTr("Show more")
                    font: Theme.font.label
                    color: moreArea.containsMouse ? Theme.colors.accent : Theme.colors.textTertiary
                    MouseArea {
                        id: moreArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: description.expanded = !description.expanded
                    }
                }

                SegmentedControl {
                    currentId: root.tab
                    model: [{ id: "related", label: qsTr("Related tracks") },
                            { id: "playlists", label: qsTr("In playlists") },
                            { id: "comments", label: qsTr("Comments") }]
                    onActivated: id => root.tab = id
                }
            }
        }
    }

    CollectionView {
        anchors.fill: parent
        visible: root.tab !== "comments"
        model: root.tab === "playlists" ? root.detail.playlists : root.detail.related
        mode: root.tab === "playlists" ? "grid" : "list"
        context: root.detail.title
        emptyText: root.tab === "playlists" ? qsTr("Not in any public playlist.") : qsTr("No related tracks.")
        header: header
    }

    ListView {
        id: comments
        // Runs on under the player bar, like the CollectionView beside it.
        readonly property real overhang: Window.window && Window.window.contentOverhang !== undefined
                                         ? Window.window.contentOverhang : 0
        anchors.fill: parent
        anchors.bottomMargin: -overhang
        bottomMargin: overhang
        visible: root.tab === "comments"
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Bound by tab, not by visibility, like CollectionView.
        model: root.tab === "comments" ? root.detail.commentList : null
        header: header
        spacing: Theme.space.md
        property bool atStart: true
        readonly property real headerExtent: headerItem ? headerItem.height : 0
        onContentYChanged: atStart = atYBeginning
        onHeaderExtentChanged: if (atStart) positionViewAtBeginning()
        ScrollBar.vertical: ScrollBar { endInset: comments.overhang }
        delegate: Item {
            id: comment
            required property string body
            required property string user
            required property var userId
            required property string avatar
            required property var at
            required property var position
            width: ListView.view.width
            height: Math.max(avatarArt.height, text.implicitHeight)
            Accessible.role: Accessible.ListItem
            Accessible.name: user + ": " + body
            CoverArt {
                id: avatarArt
                width: 36 * Theme.scale
                height: width
                shape: "circle"
                source: comment.avatar
            }
            Column {
                id: text
                anchors.left: avatarArt.right
                anchors.leftMargin: Theme.space.md
                anchors.right: parent.right
                spacing: Theme.space.xxs
                Row {
                    spacing: Theme.space.sm
                    Text {
                        text: comment.user
                        font: Theme.font.bodyStrong
                        color: userArea.containsMouse ? Theme.colors.accent : Theme.colors.textPrimary
                        textFormat: Text.PlainText
                        MouseArea {
                            id: userArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (comment.userId > 0) Window.window.openUser(comment.userId)
                        }
                    }
                    Text {
                        // At a moment of the track: a click goes there in the playing track.
                        visible: comment.position >= 0
                        text: Format.revision < 0 ? "" : qsTr("at %1").arg(Format.duration(comment.position))
                        font: Theme.font.caption
                        color: atArea.containsMouse && root.current ? Theme.colors.accent : Theme.colors.textTertiary
                        MouseArea {
                            id: atArea
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: root.current
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Player.seek(comment.position)
                        }
                    }
                    Text {
                        visible: comment.at && !isNaN(comment.at)
                        text: Format.revision < 0 || !visible ? "" : Format.relativeTime(comment.at)
                        font: Theme.font.caption
                        color: Theme.colors.textTertiary
                    }
                }
                Text {
                    width: parent.width
                    text: comment.body
                    font: Theme.font.body
                    color: Theme.colors.textSecondary
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                }
            }
        }
        footer: Item {
            width: comments.width
            height: Theme.space.xxl * 2
            Text {
                anchors.centerIn: parent
                text: !root.detail ? "" : root.detail.commentList.loading ? qsTr("Loading comments…")
                      : root.detail.commentList.errorCode >= 0
                        ? (Format.revision < 0 ? "" : Format.error(root.detail.commentList.errorCode))
                      : root.detail.commentList.count === 0 ? qsTr("No comments yet.") : ""
                font: Theme.font.body
                color: Theme.colors.textTertiary
            }
        }
    }
}
