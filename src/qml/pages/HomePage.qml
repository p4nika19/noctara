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
                    clip: false
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
                            radius: coverArt.cornerRadius
                            blur: 48 * Theme.scale
                            offsetY: 0
                            spread: -4 * Theme.scale
                            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                                           Theme.glass.glowCover && !Theme.glass.flat ? Theme.glass.glow * 0.9 : 0)
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

        Loader {
            id: discoveryLoader
            active: Account.signedIn
            visible: active
            width: column.width
            height: active ? implicitHeight : 0
            sourceComponent: Row {
                id: discoveryRow
                width: discoveryLoader.width
                spacing: Theme.space.lg

                property real waveProgress: 0
                readonly property bool animating: Theme.glass.discoveryAnimated
                    && Window.window && Window.window.active
                    && Window.window.visibility !== Window.Minimized

                NumberAnimation {
                    id: waveAnim
                    target: discoveryRow
                    property: "waveProgress"
                    from: 0.0
                    to: 1.0
                    duration: 36000
                    loops: Animation.Infinite
                    running: discoveryRow.visible
                    paused: running && !discoveryRow.animating
                }

                DiscoveryCard {
                    cardIndex: 0
                    totalRowWidth: discoveryRow.width
                    rowSpacing: discoveryRow.spacing
                    waveProgress: discoveryRow.waveProgress
                    width: (discoveryRow.width - Theme.space.lg) / 2
                    model: Catalog.dailyDrops
                    title: qsTr("Daily Drops")
                    description: qsTr("New releases based on your taste. Updated every day")
                    context: qsTr("Daily Drops")
                    kind: "daily-drops"
                }
                DiscoveryCard {
                    cardIndex: 1
                    totalRowWidth: discoveryRow.width
                    rowSpacing: discoveryRow.spacing
                    waveProgress: discoveryRow.waveProgress
                    width: (discoveryRow.width - Theme.space.lg) / 2
                    model: Catalog.weeklyWave
                    title: qsTr("Weekly Wave")
                    description: qsTr("The best of SoundCloud just for you. Updated every Monday")
                    context: qsTr("Weekly Wave")
                    kind: "weekly-wave"
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
        property int cardIndex: 0
        property real totalRowWidth: 0
        property real rowSpacing: Theme.space.lg
        property real waveProgress: 0

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
        // Collage tiles are 57-114 px: sized artwork avoids downloading and decoding multi-megabyte originals.
        property var coverSources: [
            card.model && card.model.count > 0 ? card.model.artworkAt(0) : "",
            card.model && card.model.count > 1 ? card.model.artworkAt(1) : "",
            card.model && card.model.count > 2 ? card.model.artworkAt(2) : "",
            card.model && card.model.count > 3 ? card.model.artworkAt(3) : ""
        ]

        function refreshCovers() {
            if (!card.model) return
            var srcs = []
            for (var i = 0; i < 4; ++i) {
                srcs.push(i < card.model.count ? card.model.artworkAt(i) : "")
            }
            card.coverSources = srcs
        }

        Connections {
            target: card.model
            function onDataChanged(topLeft) { if (topLeft.row <= 3) card.refreshCovers() }
            function onCountChanged() { card.refreshCovers() }
            function onRowsInserted(parent, first) { if (first <= 3) card.refreshCovers() }
            function onModelReset() { card.refreshCovers() }
        }

        // 1. Static background patterns (tint gradient, ambient glow, subtle diagonal hatch)
        // Painted strictly once into a cached image texture: never repainted during animation!
        Canvas {
            id: bgCanvas
            anchors.fill: parent
            renderTarget: Canvas.Image

            function drawRoundedRect(ctx, x, y, w, h, r) {
                var radius = Math.max(0, Math.min(r, w / 2, h / 2))
                ctx.beginPath()
                ctx.moveTo(x + radius, y)
                ctx.arcTo(x + w, y, x + w, y + h, radius)
                ctx.arcTo(x + w, y + h, x, y + h, radius)
                ctx.arcTo(x, y + h, x, y, radius)
                ctx.arcTo(x, y, x + w, y, radius)
                ctx.closePath()
            }

            onPaint: {
                var ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                ctx.save()

                // Clip strictly to the rounded card bounds (prevents corner bleeding)
                drawRoundedRect(ctx, 0, 0, width, height, card.radius)
                ctx.clip()

                // Base subtle dark tint gradient
                var grad = ctx.createLinearGradient(0, 0, width, 0)
                grad.addColorStop(0.0, card.tintStart)
                grad.addColorStop(1.0, card.tintEnd)
                ctx.fillStyle = grad
                ctx.fillRect(0, 0, width, height)

                // Ambient radial glow
                var radial = ctx.createRadialGradient(width * 0.25, height * 0.5, 0,
                                                      width * 0.25, height * 0.5, width * 0.65)
                radial.addColorStop(0.0, card.glowColor)
                radial.addColorStop(1.0, "transparent")
                ctx.fillStyle = radial
                ctx.fillRect(0, 0, width, height)

                // Subtle diagonal micro-texture hatch (batched in 1 draw call)
                ctx.strokeStyle = Theme.dark ? "rgba(255, 255, 255, 0.035)" : "rgba(0, 0, 0, 0.025)"
                ctx.lineWidth = 1
                var hatchStep = 6 * Theme.scale
                ctx.beginPath()
                for (var hx = -height; hx < width; hx += hatchStep) {
                    ctx.moveTo(hx, 0)
                    ctx.lineTo(hx + height, height)
                }
                ctx.stroke()

                ctx.restore()
            }

            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            property bool darkTheme: Theme.dark
            onDarkThemeChanged: requestPaint()
            property real cardRadius: card.radius
            onCardRadiusChanged: requestPaint()
            property real uiScale: Theme.scale
            onUiScaleChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }

        // 2. Continuous flowing wave curves: painted once onto a periodic strip and translated via GPU matrix
        // (0% CPU, 0 Canvas redraws, 0 main thread overhead while animating).
        Item {
            id: waveContainer
            anchors.fill: parent
            clip: true

            Canvas {
                id: waveCanvas
                y: 0
                height: card.height
                readonly property real period: card.totalRowWidth > 0 ? card.totalRowWidth : (card.width * 2 + card.rowSpacing)
                width: card.width + period
                x: -(card.waveProgress * period)
                renderTarget: Canvas.Image

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.save()

                    var globalOffset = card.cardIndex * (card.width + card.rowSpacing)
                    var L = period > 0 ? period : 1
                    var step = 4 * Theme.scale

                    // Primary continuous wave ribbon (harmonic period L)
                    ctx.strokeStyle = card.waveColor1
                    ctx.lineWidth = 2.5 * Theme.scale
                    ctx.lineCap = "round"
                    ctx.lineJoin = "round"
                    ctx.beginPath()
                    var first = true
                    for (var lx = -10; lx <= width + 10; lx += step) {
                        var gx = globalOffset + lx
                        var y1 = height * 0.55
                              + height * 0.22 * Math.sin((gx / L) * Math.PI * 4)
                              + height * 0.10 * Math.cos((gx / L) * Math.PI * 8)
                        if (first) {
                            ctx.moveTo(lx, y1)
                            first = false
                        } else {
                            ctx.lineTo(lx, y1)
                        }
                    }
                    ctx.stroke()

                    // Secondary harmonious ethereal ribbon (harmonic period L)
                    ctx.strokeStyle = card.waveColor2
                    ctx.lineWidth = 1.5 * Theme.scale
                    ctx.lineCap = "round"
                    ctx.lineJoin = "round"
                    ctx.beginPath()
                    first = true
                    for (var lx2 = -10; lx2 <= width + 10; lx2 += step) {
                        var gx2 = globalOffset + lx2
                        var y2 = height * 0.42
                              + height * 0.18 * Math.cos((gx2 / L) * Math.PI * 6 + 1.2)
                              + height * 0.07 * Math.sin((gx2 / L) * Math.PI * 10)
                        if (first) {
                            ctx.moveTo(lx2, y2)
                            first = false
                        } else {
                            ctx.lineTo(lx2, y2)
                        }
                    }
                    ctx.stroke()

                    ctx.restore()
                }

                onPeriodChanged: {
                    waveCanvas.requestPaint()
                    bgCanvas.requestPaint()
                }
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                property bool darkTheme: Theme.dark
                onDarkThemeChanged: requestPaint()
                property real uiScale: Theme.scale
                onUiScaleChanged: requestPaint()
                Component.onCompleted: requestPaint()
            }
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

            readonly property real collageInset: 3 * Theme.scale
            readonly property real tileCorner: Theme.coverShape === "square" ? 0
                                             : Theme.coverShape === "circle" ? grid4.cell / 2
                                             : Math.min(Theme.radius.xs, grid4.cell / 2)
            readonly property real singleCoverCorner: Theme.coverShape === "square" ? 0
                                                    : Theme.coverShape === "circle" ? (width - collageInset * 2) / 2
                                                    : Math.min(Math.max(0, Theme.radius.md - collageInset),
                                                               (width - collageInset * 2) / 2)
            // Always concentric with the cover corners inside so tiles never protrude past the plate border.
            readonly property real plateRadius: card.trackCount >= 4 ? tileCorner + collageInset
                                              : (card.trackCount > 0 ? singleCoverCorner + collageInset
                                                                     : Math.min(Theme.radius.md, width / 2))

            // Collage background plate
            Rectangle {
                anchors.fill: parent
                radius: collageContainer.plateRadius
                color: Theme.colors.surfaceRaised
                border.width: Theme.space.hairline
                border.color: card.borderAccent
            }

            // 4-track 2x2 grid collage
            Grid {
                id: grid4
                visible: card.trackCount >= 4
                anchors.fill: parent
                anchors.margins: collageContainer.collageInset
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
                        radius: collageContainer.tileCorner
                        fillMode: Image.PreserveAspectCrop
                        source: card.coverSources[index] || ""
                    }
                }
            }

            // 1-track full cover
            CoverArt {
                visible: card.trackCount > 0 && card.trackCount < 4
                anchors.fill: parent
                anchors.margins: collageContainer.collageInset
                radius: collageContainer.singleCoverCorner
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
        // Right-click opens the discovery context menu.
        MouseArea {
            id: cardTap
            z: -1
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: (mouse) => {
                if (mouse.button === Qt.RightButton) {
                    Window.window.showDiscoveryMenu(card.kind, card.model, card.context, cardTap, Qt.point(mouse.x, mouse.y))
                } else {
                    Window.window.openDiscovery(card.kind)
                }
            }
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
