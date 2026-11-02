import QtQuick
import Noctara
import "../components"

// A person: avatar, name, location and counts, their tracks and playlists.
Item {
    id: root

    required property UserDetail detail
    property string tab: "tracks"
    onTabChanged: if (tab === "playlists") detail.loadPlaylists()
    // The page is kept when one user replaces another (Main.qml's detail Loader); the tab stays.
    onDetailChanged: if (tab === "playlists" && detail) detail.loadPlaylists()

    CollectionView {
        anchors.fill: parent
        model: root.tab === "playlists" ? root.detail.playlists : root.detail.tracks
        mode: root.tab === "playlists" ? "grid" : "list"
        context: root.detail.username
        emptyText: root.tab === "playlists" ? qsTr("No playlists.") : qsTr("No tracks.")
        header: Item {
            width: ListView.view ? ListView.view.width : (GridView.view ? GridView.view.width : 0)
            height: column.height + Theme.space.xl
            Column {
                id: column
                width: parent.width
                spacing: Theme.space.lg
                Row {
                    width: parent.width
                    spacing: Theme.space.xl
                    CoverArt {
                        id: avatar
                        width: Math.min(160 * Theme.scale, parent.width * 0.25)
                        height: width
                        shape: "circle"
                        source: root.detail.avatar
                    }
                    Column {
                        width: parent.width - avatar.width - parent.spacing
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.space.sm
                        Text {
                            width: parent.width
                            text: root.detail.username
                            font: Theme.font.headline
                            color: Theme.colors.textPrimary
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            Accessible.role: Accessible.Heading
                        }
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            text: [root.detail.fullName, root.detail.location].filter(s => s.length > 0).join(" · ")
                            font: Theme.font.body
                            color: Theme.colors.textSecondary
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                        }
                        Text {
                            width: parent.width
                            text: Format.revision < 0 ? "" : [
                                root.detail.followers !== undefined ? qsTr("%1 followers").arg(Format.count(root.detail.followers)) : "",
                                root.detail.trackCount !== undefined ? qsTr("%n track(s)", "", root.detail.trackCount) : ""
                            ].filter(s => s.length > 0).join(" · ")
                            font: Theme.font.caption
                            color: Theme.colors.textTertiary
                        }
                        Row {
                            spacing: Theme.space.sm
                            topPadding: Theme.space.sm
                            Button {
                                variant: "primary"
                                iconName: "play"
                                text: qsTr("Play")
                                enabled: root.detail.tracks.count > 0
                                onClicked: Player.playCollection(root.detail.tracks, 0, root.detail.username)
                            }
                            IconButton {
                                iconName: "shuffle"
                                text: qsTr("Shuffle")
                                enabled: root.detail.tracks.count > 0
                                onClicked: Player.shufflePlay(root.detail.tracks, root.detail.username)
                            }
                            IconButton {
                                iconName: "external-link"
                                text: qsTr("Open on SoundCloud")
                                enabled: root.detail.permalink.length > 0
                                onClicked: App.openLink(root.detail.permalink)
                            }
                        }
                    }
                }
                SegmentedControl {
                    currentId: root.tab
                    model: [{ id: "tracks", label: qsTr("Tracks") }, { id: "playlists", label: qsTr("Playlists") }]
                    onActivated: id => root.tab = id
                }
            }
        }
    }
}
