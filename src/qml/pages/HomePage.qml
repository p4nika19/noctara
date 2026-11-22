import QtQuick
import QtQuick.Window
import Noctara
import "../components"

// Home: date and greeting, the now-playing card, then recently played and the feed (signed in) or the current
// charts (signed out), as rows of cards.
Flickable {
    id: root

    // Main.qml lets this page run on under the floating player bar; the last row can still scroll clear of it.
    readonly property real overhang: Window.window && Window.window.contentOverhang !== undefined
                                     ? Window.window.contentOverhang : 0

    contentHeight: column.implicitHeight + Theme.space.xxl
    bottomMargin: overhang
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar { endInset: root.overhang }

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
            Catalog.ensure("daily-drops")
            Catalog.ensure("weekly-wave")
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
                              ? (Player.context.length > 0 ? qsTr("%1 · from %2").arg(Player.artist, Player.context)
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

        Row {
            visible: Account.signedIn
            width: column.width
            spacing: Theme.space.lg
            DiscoveryCard {
                width: (column.width - Theme.space.lg) / 2
                model: Catalog.dailyDrops
                title: qsTr("Daily Drops")
                description: qsTr("New releases based on your taste. Updated every day")
                context: qsTr("Daily Drops")
                kind: "daily-drops"
            }
            DiscoveryCard {
                width: (column.width - Theme.space.lg) / 2
                model: Catalog.weeklyWave
                title: qsTr("Weekly Wave")
                description: qsTr("The best of SoundCloud just for you. Updated every Monday")
                context: qsTr("Weekly Wave")
                kind: "weekly-wave"
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

    // A large discovery panel: artwork, title, description and play actions. Two of these sit side by side under
    // Now playing, with equal width and the same height.
    component DiscoveryCard: GlassPanel {
        id: card
        property CollectionModel model
        property string title
        property string description
        property string context
        property string kind

        radius: Theme.radius.lg
        height: 312 * Theme.scale  // identical size for both discovery panels

        Column {
            id: cardContent
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Math.max(Theme.space.xl, card.cornerInset)
            spacing: Theme.space.md

            CoverArt {
                id: artworkImage
                width: parent.width
                height: 136 * Theme.scale
                radius: Theme.radius.md
                fillMode: Image.PreserveAspectFit
                source: card.model && card.model.count > 0 ? card.model.originalArtworkAt(0) : ""
            }
            // Stub tracks hydrate after the first page arrives: method calls are not reactive, so the cover must
            // be re-read when the model's data changes.
            Connections {
                target: card.model
                function onDataChanged() {
                    artworkImage.source = card.model && card.model.count > 0 ? card.model.originalArtworkAt(0) : ""
                }
                function onCountChanged() {
                    artworkImage.source = card.model && card.model.count > 0 ? card.model.originalArtworkAt(0) : ""
                }
            }
            Text {
                width: parent.width
                text: card.title
                font: Theme.font.headline
                color: Theme.colors.textPrimary
                elide: Text.ElideRight
                Accessible.role: Accessible.Heading
            }
            Text {
                width: parent.width
                text: card.model && card.model.errorCode >= 0 ? Format.error(card.model.errorCode) : card.description
                font: Theme.font.caption
                color: Theme.colors.textSecondary
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            Row {
                id: buttonRow
                // NOTE: no `z` here. z-values only reorder siblings inside one parent, so a z on this Row can
                // never lift the buttons above the full-card MouseArea (a sibling of cardContent). Instead, that
                // MouseArea stops short of this strip via its `height`, so presses over Play/Shuffle reach the
                // buttons; clicks anywhere else on the card still open the full track list.
                spacing: Theme.space.sm
                Button {
                    variant: "primary"
                    iconName: "play"
                    text: qsTr("Play")
                    enabled: card.model && card.model.count > 0
                    onClicked: Player.playCollection(card.model, 0, card.context)
                }
                IconButton {
                    iconName: "shuffle"
                    text: qsTr("Shuffle")
                    enabled: card.model && card.model.count > 0
                    onClicked: Player.shufflePlay(card.model, card.context)
                }
            }
        }
        // The card itself opens the full track list; its Play and Shuffle buttons do their own actions instead.
        // z-values only reorder siblings, so a `z` on the Row cannot lift the buttons above this area — instead
        // the area stops short of the button strip: its bottom is pinned to the Row's top in *card* coordinates
        // (Row.y is relative to cardContent, which starts at the content margin), so presses over Play/Shuffle
        // reach the buttons while clicks anywhere else still open the full track list.
        MouseArea {
            id: cardTap
            z: 1  // above CoverArt's own MouseArea, below the Play/Shuffle row
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            // Bottom edge pinned to the top of the Play/Shuffle strip, converted from cardContent coordinates
            // (the Row lives inside the Column) to card coordinates; minus half the row spacing so a click in
            // the gap between the texts and the buttons still opens the list rather than hitting nothing.
            height: buttonRow.mapToItem(card, 0, 0).y - Theme.space.sm / 2
            onClicked: Window.window.openDiscovery(card.kind)
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
