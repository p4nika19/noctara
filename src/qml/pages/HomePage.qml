import QtQuick
import QtQuick.Window
import Noctara
import "../components"

// Home: date and greeting, the now-playing card, then recently played and the feed (signed in) or the current
// charts (signed out), as rows of cards.
Flickable {
    id: root

    contentHeight: column.implicitHeight + Theme.space.xxl
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    // The page stays alive once visited: the date and greeting follow the clock while shown and catch up when the
    // page comes back.
    property date now: new Date()
    onVisibleChanged: if (visible) now = new Date()
    Timer {
        interval: 60000
        repeat: true
        running: root.visible
        onTriggered: root.now = new Date()
    }

    function load() {
        if (Account.signedIn) {
            Catalog.ensure("history")
            Catalog.ensure("feed")
        } else {
            Catalog.ensure("charts")
        }
    }
    Component.onCompleted: load()
    Connections {
        target: Account
        function onChanged() { root.load() }
    }
    readonly property int hour: now.getHours()

    Column {
        id: column
        width: root.width
        spacing: Theme.space.xl

        Column {
            spacing: Theme.space.xs
            Text {
                text: root.now.toLocaleDateString(Qt.locale(Language.localeName), "dddd, d MMMM")
                font: Theme.font.eyebrow
                color: Theme.colors.textTertiary
            }
            Text {
                text: root.hour < 5 ? qsTr("Good night")
                    : root.hour < 12 ? qsTr("Good morning")
                    : root.hour < 18 ? qsTr("Good afternoon") : qsTr("Good evening")
                font: Theme.font.display
                color: Theme.colors.textPrimary
                Accessible.role: Accessible.Heading
            }
        }

        GlassPanel {
            id: hero
            width: column.width
            height: heroRow.implicitHeight + Theme.space.xl * 2
            radius: Theme.radius.lg

            Row {
                id: heroRow
                x: Theme.space.xl
                y: Theme.space.xl
                width: hero.width - Theme.space.xl * 2
                spacing: Theme.space.xl
                Item {
                    width: 180 * Theme.scale
                    height: width
                    Shadow {
                        anchors.fill: parent
                        z: -1
                        radius: cover.radius
                        blur: 70 * Theme.scale
                        offsetY: 24 * Theme.scale
                        spread: -8 * Theme.scale
                        color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                                       Theme.glass.glowCover ? Theme.glass.glow * 0.9 : 0)
                    }
                    Skeleton {
                        id: cover
                        anchors.fill: parent
                        visible: !Player.hasTrack
                        radius: Theme.coverShape === "circle" ? width / 2 : (Theme.coverShape === "square" ? 0 : Theme.radius.lg)
                    }
                    CoverArt {
                        anchors.fill: parent
                        visible: Player.hasTrack
                        radius: Theme.radius.lg
                        source: Player.artworkUrl
                    }
                }
                Column {
                    width: heroRow.width - 180 * Theme.scale - heroRow.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space.md
                    Text {
                        text: qsTr("Now playing")
                        font: Theme.font.eyebrow
                        color: Theme.colors.textTertiary
                    }
                    Text {
                        width: parent.width
                        text: Player.hasTrack ? Player.title : qsTr("Nothing is playing yet")
                        font: Theme.font.headline
                        color: Theme.colors.textPrimary
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }
                    Text {
                        width: parent.width
                        text: Player.hasTrack
                              ? (Player.context.length > 0 ? qsTr("%1 · from %2").arg(Player.artist).arg(Player.context)
                                                           : Player.artist)
                              : (Account.signedIn ? qsTr("Find something in Search and it starts here.")
                                                  : qsTr("Connect your SoundCloud account to see your feed, likes and playlists."))
                        font: Theme.font.body
                        color: Theme.colors.textSecondary
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                    }
                }
            }
        }

        CardRow {
            visible: Account.signedIn
            title: qsTr("Recently played")
            model: Catalog.history
            context: qsTr("History")
            emptyText: qsTr("Nothing played yet.")
        }
        CardRow {
            visible: Account.signedIn
            title: qsTr("From your feed")
            model: Catalog.feed
            context: qsTr("Feed")
            emptyText: qsTr("Follow people on SoundCloud and their new tracks and reposts appear here.")
        }
        CardRow {
            visible: !Account.signedIn
            title: qsTr("Top tracks right now")
            model: Catalog.charts
            context: qsTr("Charts")
            emptyText: ""
        }
    }

    // A titled, horizontally scrolling row of cards over the first page of a list.
    component CardRow: Column {
        id: cardRow
        property string title
        property CollectionModel model
        property string context
        property string emptyText
        readonly property real cardWidth: (Layout.cardSize === "small" ? 140 : Layout.cardSize === "large" ? 220 : 172)
                                          * Theme.scale
        width: column.width
        spacing: Theme.space.md
        Text {
            text: cardRow.title
            font: Theme.font.title
            color: Theme.colors.textPrimary
            Accessible.role: Accessible.Heading
        }
        ListView {
            id: cards
            width: parent.width
            height: cardRow.cardWidth + Theme.font.bodyStrong.pixelSize * 1.4 + Theme.font.caption.pixelSize * 1.4
                    + Theme.space.lg
            orientation: ListView.Horizontal
            spacing: Theme.space.lg
            clip: true
            reuseItems: true
            boundsBehavior: Flickable.StopAtBounds
            model: cardRow.model
            visible: cardRow.model.count > 0
            delegate: Card {
                width: cardRow.cardWidth
                onActivated: {
                    if (kind === "track")
                        Player.playCollection(cardRow.model, index, cardRow.context)
                    else if (kind === "playlist")
                        Window.window.openPlaylist(itemId)
                    else
                        Window.window.openUser(itemId)
                }
                onPlayRequested: Player.playCollection(cardRow.model, index, cardRow.context)
                onMenuRequested: point => Window.window.showCollectionMenu(cardRow.model, index, cardRow.context, this, point)
            }
        }
        Row {
            visible: cardRow.model.count === 0 && cardRow.model.loading
            spacing: Theme.space.lg
            Repeater {
                model: 5
                delegate: Column {
                    spacing: Theme.space.sm
                    Skeleton { width: cardRow.cardWidth; height: width; radius: Theme.radius.md; loading: true }
                    Skeleton { width: cardRow.cardWidth * 0.75; height: 12 * Theme.scale; radius: height / 2; loading: true }
                }
            }
        }
        Text {
            width: parent.width
            visible: cardRow.model.count === 0 && !cardRow.model.loading && text.length > 0
            text: cardRow.model.errorCode >= 0 ? Format.error(cardRow.model.errorCode) : cardRow.emptyText
            font: Theme.font.body
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }
    }
}
