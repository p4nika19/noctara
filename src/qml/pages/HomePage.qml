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

            Behavior on height {
                NumberAnimation {
                    duration: Theme.motion.slow
                    easing.type: Theme.motion.easingEmphasized
                }
            }

            Row {
                id: heroRow
                x: Theme.space.xl
                y: Theme.space.xl
                width: hero.width - Theme.space.xl * 2
                spacing: 0

                Item {
                    id: coverWrapper
                    width: Player.hasTrack ? 180 * Theme.scale + Theme.space.xl : 0
                    height: Player.hasTrack ? 180 * Theme.scale : textContent.implicitHeight
                    clip: true
                    Behavior on width {
                        NumberAnimation {
                            duration: Theme.motion.slow
                            easing.type: Theme.motion.easingEmphasized
                        }
                    }
                    Behavior on height {
                        NumberAnimation {
                            duration: Theme.motion.slow
                            easing.type: Theme.motion.easingEmphasized
                        }
                    }

                    Item {
                        id: coverContainer
                        width: 180 * Theme.scale
                        height: width
                        anchors.verticalCenter: parent.verticalCenter
                        opacity: Player.hasTrack ? 1 : 0
                        visible: opacity > 0
                        Behavior on opacity {
                            NumberAnimation {
                                duration: Theme.motion.normal
                                easing.type: Theme.motion.easingStandard
                            }
                        }

                        Shadow {
                            anchors.fill: parent
                            z: -1
                            radius: coverArt.radius
                            blur: 70 * Theme.scale
                            offsetY: 24 * Theme.scale
                            spread: -8 * Theme.scale
                            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                                           Theme.glass.glowCover ? Theme.glass.glow * 0.9 : 0)
                        }
                        CoverArt {
                            id: coverArt
                            anchors.fill: parent
                            radius: Theme.coverShape === "circle" ? width / 2 : (Theme.coverShape === "square" ? 0 : Theme.radius.lg)
                            source: Player.artworkUrl
                        }
                    }
                }
                Column {
                    id: textContent
                    width: heroRow.width - coverWrapper.width
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

    // A large discovery panel: artwork collage, title, description and play actions. Two of these sit side by side
    // on Home, with equal width and the same height.
    component DiscoveryCard: GlassPanel {
        id: card
        objectName: "discoveryCard_" + kind
        property CollectionModel model
        property string title
        property string description
        property string context
        property string kind

        readonly property bool isDaily: kind === "daily-drops"

        // Translucent dark-mode and light-mode palette
        readonly property color tintStart: Theme.dark
            ? (isDaily ? Qt.rgba(0.04, 0.12, 0.26, 0.30) : Qt.rgba(0.20, 0.08, 0.03, 0.30))
            : (isDaily ? Qt.rgba(0.10, 0.38, 0.82, 0.08) : Qt.rgba(0.85, 0.35, 0.05, 0.08))
        readonly property color tintEnd: Theme.dark
            ? (isDaily ? Qt.rgba(0.07, 0.18, 0.38, 0.40) : Qt.rgba(0.28, 0.11, 0.04, 0.40))
            : (isDaily ? Qt.rgba(0.15, 0.48, 0.92, 0.12) : Qt.rgba(0.92, 0.42, 0.08, 0.12))

        readonly property color glowColor: Theme.dark
            ? (isDaily ? Qt.rgba(0.12, 0.32, 0.70, 0.15) : Qt.rgba(0.65, 0.22, 0.05, 0.15))
            : (isDaily ? Qt.rgba(0.10, 0.38, 0.82, 0.06) : Qt.rgba(0.85, 0.35, 0.05, 0.06))

        readonly property color waveColor1: Theme.dark
            ? (isDaily ? Qt.rgba(0.22, 0.55, 0.90, 0.18) : Qt.rgba(0.85, 0.38, 0.12, 0.18))
            : (isDaily ? Qt.rgba(0.10, 0.38, 0.82, 0.14) : Qt.rgba(0.85, 0.35, 0.05, 0.14))

        readonly property color waveColor2: Theme.dark
            ? (isDaily ? Qt.rgba(0.08, 0.30, 0.68, 0.12) : Qt.rgba(0.55, 0.18, 0.04, 0.12))
            : (isDaily ? Qt.rgba(0.15, 0.48, 0.92, 0.09) : Qt.rgba(0.92, 0.42, 0.08, 0.09))

        readonly property color borderAccent: Theme.dark
            ? (isDaily ? Qt.rgba(0.25, 0.58, 0.95, 0.14) : Qt.rgba(0.88, 0.42, 0.14, 0.14))
            : (isDaily ? Qt.rgba(0.10, 0.38, 0.82, 0.16) : Qt.rgba(0.85, 0.35, 0.05, 0.16))

        readonly property color tagBg: Theme.dark
            ? (isDaily ? Qt.rgba(0.10, 0.30, 0.65, 0.35) : Qt.rgba(0.50, 0.18, 0.04, 0.35))
            : (isDaily ? Qt.rgba(0.10, 0.38, 0.82, 0.14) : Qt.rgba(0.85, 0.35, 0.05, 0.14))

        readonly property color tagBorder: Theme.dark
            ? (isDaily ? Qt.rgba(0.25, 0.58, 0.95, 0.25) : Qt.rgba(0.88, 0.42, 0.14, 0.25))
            : (isDaily ? Qt.rgba(0.10, 0.38, 0.82, 0.25) : Qt.rgba(0.85, 0.35, 0.05, 0.25))

        readonly property color tagText: Theme.dark
            ? (isDaily ? "#93c5fd" : "#fdba74")
            : (isDaily ? "#1d4ed8" : "#c2410c")

        readonly property color tagLabel: Theme.dark
            ? (isDaily ? "#bfdbfe" : "#fed7aa")
            : (isDaily ? "#2563eb" : "#ea580c")

        radius: Theme.radius.lg
        height: 164 * Theme.scale

        readonly property int trackCount: card.model ? card.model.count : 0
        property var coverSources: [
            card.model && card.model.count > 0 ? card.model.originalArtworkAt(0) : "",
            card.model && card.model.count > 1 ? card.model.originalArtworkAt(1) : "",
            card.model && card.model.count > 2 ? card.model.originalArtworkAt(2) : "",
            card.model && card.model.count > 3 ? card.model.originalArtworkAt(3) : ""
        ]

        function refreshCovers() {
            if (!card.model) return
            var srcs = []
            for (var i = 0; i < 4; ++i) {
                srcs.push(i < card.model.count ? card.model.originalArtworkAt(i) : "")
            }
            card.coverSources = srcs
        }

        Connections {
            target: card.model
            function onDataChanged() { card.refreshCovers() }
            function onCountChanged() { card.refreshCovers() }
            function onRowsInserted() { card.refreshCovers() }
            function onModelReset() { card.refreshCovers() }
        }

        // Native background patterns: clipped strictly to the rounded card bounds
        Canvas {
            id: bgCanvas
            anchors.fill: parent
            renderTarget: Canvas.FramebufferObject

            function drawRoundedRect(ctx, x, y, w, h, r) {
                var radius = Math.max(0, Math.min(r, w / 2, h / 2))
                ctx.beginPath()
                ctx.moveTo(x + radius, y)
                ctx.lineTo(x + w - radius, y)
                ctx.quadraticCurveTo(x + w, y, x + w, y + radius)
                ctx.lineTo(x + w, y + h - radius)
                ctx.quadraticCurveTo(x + w, y + h, x + w - radius, y + h)
                ctx.lineTo(x + radius, y + h)
                ctx.quadraticCurveTo(x, y + h, x, y + h - radius)
                ctx.lineTo(x, y + radius)
                ctx.quadraticCurveTo(x, y, x + radius, y)
                ctx.closePath()
            }

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.save()

                // Clip strictly to the rounded card bounds (prevents corner bleeding)
                drawRoundedRect(ctx, 0, 0, width, height, card.radius)
                ctx.clip()

                // 1. Base subtle dark tint gradient
                var grad = ctx.createLinearGradient(0, 0, width, 0)
                grad.addColorStop(0.0, card.tintStart)
                grad.addColorStop(1.0, card.tintEnd)
                ctx.fillStyle = grad
                ctx.fillRect(0, 0, width, height)

                // 2. Ambient radial glow
                var radial = ctx.createRadialGradient(width * 0.25, height * 0.5, 0,
                                                      width * 0.25, height * 0.5, width * 0.65)
                radial.addColorStop(0.0, card.glowColor)
                radial.addColorStop(1.0, "transparent")
                ctx.fillStyle = radial
                ctx.fillRect(0, 0, width, height)

                // 3. Flowing fluid wave curves
                var waveGrad1 = ctx.createLinearGradient(0, height * 0.8, width, height * 0.2)
                waveGrad1.addColorStop(0.0, card.waveColor1)
                waveGrad1.addColorStop(0.5, card.waveColor2)
                waveGrad1.addColorStop(1.0, "transparent")
                ctx.strokeStyle = waveGrad1
                ctx.lineWidth = 2.5 * Theme.scale
                ctx.beginPath()
                ctx.moveTo(-10, height * 0.7)
                ctx.bezierCurveTo(width * 0.2, height * 1.15,
                                  width * 0.35, height * 0.1,
                                  width * 0.65, height * 0.4)
                ctx.bezierCurveTo(width * 0.8, height * 0.55,
                                  width * 0.9, height * 0.2,
                                  width + 10, height * 0.15)
                ctx.stroke()

                var waveGrad2 = ctx.createLinearGradient(width * 0.1, 0, width * 0.8, height)
                waveGrad2.addColorStop(0.0, card.waveColor2)
                waveGrad2.addColorStop(1.0, "transparent")
                ctx.strokeStyle = waveGrad2
                ctx.lineWidth = 1.5 * Theme.scale
                ctx.beginPath()
                ctx.moveTo(width * 0.1, -10)
                ctx.bezierCurveTo(width * 0.25, height * 0.45,
                                  width * 0.45, height * 0.9,
                                  width * 0.75, height * 0.75)
                ctx.stroke()

                // 4. Subtle diagonal micro-texture hatch
                ctx.strokeStyle = Theme.dark ? "rgba(255, 255, 255, 0.035)" : "rgba(0, 0, 0, 0.025)"
                ctx.lineWidth = 1
                var step = 6 * Theme.scale
                for (var lx = -height; lx < width; lx += step) {
                    ctx.beginPath()
                    ctx.moveTo(lx, 0)
                    ctx.lineTo(lx + height, height)
                    ctx.stroke()
                }

                ctx.restore()
            }

            property bool darkTheme: Theme.dark
            onDarkThemeChanged: requestPaint()
        }

        // Subtle tinted card border
        Rectangle {
            anchors.fill: parent
            radius: card.radius
            color: "transparent"
            border.width: Theme.space.hairline
            border.color: card.borderAccent
        }

        // Left collage of first 4 tracks
        Item {
            id: collageContainer
            width: 120 * Theme.scale
            height: 120 * Theme.scale
            anchors.left: parent.left
            anchors.leftMargin: Math.max(Theme.space.lg, card.cornerInset)
            anchors.verticalCenter: parent.verticalCenter

            // Collage background plate
            Rectangle {
                anchors.fill: parent
                radius: Theme.radius.md
                color: Theme.colors.surfaceRaised
                border.width: Theme.space.hairline
                border.color: card.borderAccent
            }

            // 4-track 2x2 grid collage
            Grid {
                id: grid4
                visible: card.trackCount >= 4
                anchors.fill: parent
                anchors.margins: 3 * Theme.scale
                columns: 2
                rows: 2
                spacing: 2 * Theme.scale

                readonly property real cell: (width - spacing) / 2

                Repeater {
                    model: 4
                    CoverArt {
                        required property int index
                        width: grid4.cell
                        height: grid4.cell
                        radius: Theme.radius.xs
                        fillMode: Image.PreserveAspectCrop
                        source: card.coverSources[index] || ""
                    }
                }
            }

            // 1-track full cover
            CoverArt {
                visible: card.trackCount > 0 && card.trackCount < 4
                anchors.fill: parent
                anchors.margins: 3 * Theme.scale
                radius: Theme.radius.md - 2 * Theme.scale
                fillMode: Image.PreserveAspectCrop
                source: card.coverSources[0] || ""
            }

            // Empty state placeholder
            Item {
                visible: card.trackCount === 0
                anchors.fill: parent
                Icon {
                    anchors.centerIn: parent
                    name: "list-music"
                    size: 32 * Theme.scale
                    color: card.tagText
                }
            }
        }

        // Right side content
        Column {
            id: cardContent
            anchors.left: collageContainer.right
            anchors.leftMargin: Theme.space.lg
            anchors.right: parent.right
            anchors.rightMargin: Math.max(Theme.space.lg, card.cornerInset)
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4 * Theme.scale

            Row {
                spacing: Theme.space.xs
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    height: 18 * Theme.scale
                    radius: Theme.radius.xs
                    color: card.tagBg
                    border.width: Theme.space.hairline
                    border.color: card.tagBorder
                    width: tagText.implicitWidth + 8 * Theme.scale
                    Text {
                        id: tagText
                        anchors.centerIn: parent
                        text: card.isDaily ? "DAILY" : "WEEKLY"
                        font: Theme.font.eyebrow
                        color: card.tagText
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: card.isDaily ? "DROPS" : "WAVE"
                    font: Theme.font.eyebrow
                    color: card.tagLabel
                    opacity: 0.9
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

            Item {
                width: 1
                height: 4 * Theme.scale
            }

            Row {
                id: buttonRow
                spacing: Theme.space.sm
                Button {
                    objectName: "discoveryPlayButton"
                    variant: "primary"
                    iconName: "play"
                    text: qsTr("Play")
                    enabled: card.model && card.model.count > 0
                    onClicked: Player.playCollection(card.model, 0, card.context)
                }
                IconButton {
                    objectName: "discoveryShuffleButton"
                    iconName: "shuffle"
                    text: qsTr("Shuffle")
                    enabled: card.model && card.model.count > 0
                    onClicked: Player.shufflePlay(card.model, card.context)
                }
            }
        }

        // The card itself opens the full track list; its Play and Shuffle buttons do their own actions instead.
        MouseArea {
            id: cardTap
            z: -1
            anchors.fill: parent
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
