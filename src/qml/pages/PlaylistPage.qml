import QtQuick
import QtQuick.Window
import Noctara
import "../components"

// A playlist or album: large cover, title, owner and facts, the actions, and its tracks.
Item {
    id: root

    required property PlaylistDetail detail
    readonly property string kindLabel: detail.album
        ? (detail.setType === "ep" ? qsTr("EP") : detail.setType === "single" ? qsTr("Single")
                                                : detail.setType === "compilation" ? qsTr("Compilation") : qsTr("Album"))
        : qsTr("Playlist")

    CollectionView {
        anchors.fill: parent
        model: root.detail.tracks
        context: root.detail.title
        emptyText: root.detail.errorCode >= 0 ? Format.error(root.detail.errorCode)
                                              : (root.detail.loading ? "" : qsTr("This playlist has no tracks."))
        header: Item {
            width: ListView.view ? ListView.view.width : 0
            height: headerRow.height + Theme.space.xl
            Row {
                id: headerRow
                width: parent.width
                spacing: Theme.space.xl
                Item {
                    id: coverBox
                    width: Math.min(220 * Theme.scale, parent.width * 0.3)
                    height: width
                    Shadow {
                        anchors.fill: parent
                        z: -1
                        radius: playlistCover.cornerRadius
                        blur: 60 * Theme.scale
                        offsetY: 20 * Theme.scale
                        spread: -8 * Theme.scale
                        color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                                       Theme.glass.glowCover && !Theme.glass.flat ? Theme.glass.glow * 0.9 : 0)
                    }
                    CoverArt {
                        id: playlistCover
                        anchors.fill: parent
                        radius: Theme.radius.lg
                        source: root.detail.artwork
                    }
                }
                Column {
                    width: parent.width - coverBox.width - parent.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space.sm
                    Text {
                        text: root.kindLabel
                        font: Theme.font.eyebrow
                        color: Theme.colors.textTertiary
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
                        width: parent.width
                        text: root.detail.owner
                        font: Theme.font.bodyStrong
                        color: ownerArea.containsMouse ? Theme.colors.accent : Theme.colors.textSecondary
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        MouseArea {
                            id: ownerArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: if (root.detail.ownerId > 0) Window.window.openUser(root.detail.ownerId)
                        }
                    }
                    Text {
                        width: parent.width
                        text: Format.revision < 0 ? "" : [
                            root.detail.trackCount > 0 ? qsTr("%n track(s)", "", root.detail.trackCount) : "",
                            root.detail.duration > 0 ? Format.duration(root.detail.duration) : "",
                            root.detail.likes !== undefined ? qsTr("%1 likes").arg(Format.count(root.detail.likes)) : "",
                            root.detail.published && !isNaN(root.detail.published) ? Format.date(root.detail.published) : ""
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
                            iconName: "play"
                            text: qsTr("Play")
                            enabled: root.detail.tracks.count > 0
                            onClicked: Player.playCollection(root.detail.tracks, 0, root.detail.title)
                        }
                        IconButton {
                            iconName: "shuffle"
                            text: qsTr("Shuffle")
                            enabled: root.detail.tracks.count > 0
                            onClicked: Player.shufflePlay(root.detail.tracks, root.detail.title)
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
                            onClicked: {
                                App.copyText(root.detail.permalink)
                                App.showToast("info", qsTr("Link copied."), "", "")
                            }
                        }
                    }
                }
            }
        }
    }
}
