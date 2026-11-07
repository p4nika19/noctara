import QtQuick
import QtQuick.Window
import Noctara
import "components"
import "pages"
import "settings"

Window {
    id: window

    property string page: "home"
    property string settingsCategory: "general"
    property bool appearancePanel: false
    property bool queuePanel: false
    property bool fullPlayer: false
    property bool cheatSheet: false
    // Shows the player bar without a track (smoke tests and screenshots).
    property bool playerPreview: false
    readonly property bool playerShown: Player.hasTrack || playerPreview
    // Playlist and user pages opened on top of the current page: [{kind, id}]. Back returns through them.
    property var details: []
    readonly property var detail: details.length > 0 ? details[details.length - 1] : null
    // The shared blurred backdrop sampled by every GlassPanel (null when no blur style is in use).
    readonly property Item backdropTexture: blur.item ? blur.item.output : null

    readonly property var pageInfo: ({
        home: { id: "home", icon: "house", label: qsTr("Home") },
        search: { id: "search", icon: "search", label: qsTr("Search") },
        library: { id: "library", icon: "library", label: qsTr("Library") },
        likes: { id: "likes", icon: "heart", label: qsTr("Likes") },
        history: { id: "history", icon: "clock", label: qsTr("History") },
        charts: { id: "charts", icon: "trending-up", label: qsTr("Charts") }
    })
    readonly property var destinations: Layout.navItems.map(id => pageInfo[id]).filter(p => p !== undefined)
    readonly property var footer: [{ id: "settings", icon: "settings", label: qsTr("Settings") }]

    // Where the navigation sits and whether it takes room (auto-hide floats over the content).
    readonly property string railSide: Layout.navPosition
    readonly property bool railAutoHide: Layout.navMode === "auto_hide"
    readonly property bool railRevealed: !railAutoHide || railHover.hovered || edgeHover.hovered
    readonly property bool playerOnTop: Layout.playerPosition === "top"
    readonly property real gap: Theme.space.lg
    // Content edges after the navigation (when it takes room).
    readonly property real leftInset: railSide === "left" && !railAutoHide ? rail.width + gap * 2 : gap
    readonly property real rightInset: railSide === "right" && !railAutoHide ? rail.width + gap * 2 : gap
    readonly property real bottomInset: railSide === "bottom" && !railAutoHide ? rail.height + gap * 2 : gap
    readonly property real topInset: (titleBar.visible ? titleBar.height : 0) + gap

    width: 1280
    height: 820
    minimumWidth: 880
    minimumHeight: 600
    visible: false  // shown from C++ once the graphics configuration (GPU timestamps) is set
    color: Theme.colors.backdrop
    title: qsTr("Noctara")
    flags: Layout.customDecorations ? (Qt.Window | Qt.FramelessWindowHint) : Qt.Window

    onPageChanged: details = []

    // --- Navigation ------------------------------------------------------------------------------

    // Opening the detail already on top only closes the full player: a duplicate entry would make Back look dead.
    function openDetail(kind, id) {
        if (!detail || detail.kind !== kind || detail.id !== id)
            details = details.concat([{ kind: kind, id: id }])
        fullPlayer = false
    }
    function openPlaylist(id) { openDetail("playlist", id) }
    function openUser(id) { openDetail("user", id) }
    // Leaves the innermost overlay: the full player, a panel, then the detail pages.
    function goBack() {
        if (cheatSheet)
            cheatSheet = false
        else if (fullPlayer)
            fullPlayer = false
        else if (appearancePanel)
            appearancePanel = false
        else if (queuePanel)
            queuePanel = false
        else if (details.length > 0)
            details = details.slice(0, -1)
    }
    function toggleFullScreen() {
        window.visibility = window.visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen
    }
    function openFullPlayer() {
        if (!Player.hasTrack)
            return
        const cover = player.coverItem.mapToItem(null, 0, 0)
        fullPlayerView.sourceRect = Qt.rect(cover.x, cover.y, player.coverItem.width, player.coverItem.height)
        fullPlayer = true
    }
    function closeFullPlayer() {
        const cover = player.coverItem.mapToItem(null, 0, 0)
        fullPlayerView.sourceRect = Qt.rect(cover.x, cover.y, player.coverItem.width, player.coverItem.height)
        fullPlayer = false
        if (window.visibility === Window.FullScreen)
            window.visibility = Window.Windowed
    }

    // --- Context menus ---------------------------------------------------------------------------

    function copyLink(url) {
        App.copyText(url)
        App.showToast("info", qsTr("Link copied."), "", "")
    }
    // SoundCloud links start at a time with "#t=m:ss".
    function linkAt(url, ms) {
        const seconds = Math.floor(ms / 1000)
        return url + "#t=" + Math.floor(seconds / 60) + ":" + String(seconds % 60).padStart(2, "0")
    }
    function linkActions(permalink) {
        return [
            { text: qsTr("Open on SoundCloud"), icon: "external-link", enabled: permalink.length > 0,
              run: () => App.openLink(permalink) },
            { text: qsTr("Copy link"), icon: "copy", enabled: permalink.length > 0, run: () => copyLink(permalink) }
        ]
    }
    // A row or card of a list; `item` carries the row's data (kind, itemId, ownerId, permalink…). The data is read
    // now: the menu does not block the list, so the delegate can be reused for another row before an entry is picked.
    function showCollectionMenu(model, index, context, item, point) {
        const separator = { separator: true }
        const itemId = item.itemId, ownerId = item.ownerId, permalink = item.permalink
        let actions = []
        if (item.kind === "track") {
            actions = [
                { text: qsTr("Play"), icon: "play", run: () => Player.playCollection(model, index, context) },
                { text: qsTr("Play next"), icon: "list-start", run: () => Player.enqueueCollection(model, index, true) },
                { text: qsTr("Add to queue"), icon: "list-plus", run: () => Player.enqueueCollection(model, index, false) },
                separator,
                { text: qsTr("Go to artist"), icon: "user", enabled: ownerId > 0, run: () => openUser(ownerId) },
                separator
            ].concat(linkActions(permalink))
        } else if (item.kind === "playlist") {
            actions = [
                { text: qsTr("Open"), icon: "list-music", run: () => openPlaylist(itemId) },
                { text: qsTr("Play"), icon: "play", run: () => Player.playCollection(model, index, "") },
                { text: qsTr("Play next"), icon: "list-start", run: () => Player.enqueueCollection(model, index, true) },
                { text: qsTr("Add to queue"), icon: "list-plus", run: () => Player.enqueueCollection(model, index, false) },
                separator,
                { text: qsTr("Go to owner"), icon: "user", enabled: ownerId > 0, run: () => openUser(ownerId) },
                separator
            ].concat(linkActions(permalink))
        } else {
            actions = [
                { text: qsTr("Open profile"), icon: "user", run: () => openUser(itemId) },
                separator
            ].concat(linkActions(permalink))
        }
        contextMenu.show(actions, item, point)
    }
    function showQueueMenu(index, permalink, artistId, item, point) {
        const current = Player.queue.currentIndex
        contextMenu.show([
            { text: qsTr("Play"), icon: "play", run: () => Player.playQueueItem(index) },
            { text: qsTr("Play next"), icon: "list-start", enabled: index !== current && index !== current + 1,
              run: () => Player.moveQueueItem(index, index > current ? current + 1 : current) },
            { text: qsTr("Remove from queue"), icon: "x", danger: true, run: () => Player.removeQueueItem(index) },
            { separator: true },
            { text: qsTr("Go to artist"), icon: "user", enabled: artistId > 0, run: () => openUser(artistId) },
            { separator: true }
        ].concat(linkActions(permalink)).concat([
            { separator: true },
            { text: qsTr("Clear upcoming tracks"), icon: "list-x", enabled: Player.queue.count > current + 1,
              run: () => Player.clearUpcoming() }
        ]), item, point)
    }
    function showPlayerMenu(item, point) {
        const has = Player.hasTrack
        contextMenu.show([
            { text: Player.playing ? qsTr("Pause") : qsTr("Play"), icon: Player.playing ? "pause" : "play",
              enabled: has, run: () => Player.playPause() },
            { text: qsTr("Next track"), icon: "skip-forward", enabled: Player.canGoNext, run: () => Player.next() },
            { text: qsTr("Previous track"), icon: "skip-back", enabled: has, run: () => Player.previous() },
            { separator: true },
            { text: qsTr("Shuffle"), icon: "shuffle", checked: Player.shuffle, run: () => Player.toggleShuffle() },
            { text: Player.repeat === "one" ? qsTr("Repeat: this track")
                    : (Player.repeat === "all" ? qsTr("Repeat: all") : qsTr("Repeat: off")),
              icon: Player.repeat === "one" ? "repeat-1" : "repeat", checked: Player.repeat !== "off",
              run: () => Player.cycleRepeat() },
            { separator: true },
            fullPlayer ? { text: qsTr("Close the full-screen player"), icon: "minimize-2", run: () => closeFullPlayer() }
                       : { text: qsTr("Full-screen player"), icon: "maximize-2", enabled: has, run: () => openFullPlayer() },
            { text: queuePanel ? qsTr("Hide the queue") : qsTr("Show the queue"), icon: "list-music",
              run: () => queuePanel = !queuePanel },
            { separator: true },
            { text: qsTr("Go to artist"), icon: "user", enabled: Player.artistId > 0,
              run: () => openUser(Player.artistId) }
        ].concat(linkActions(Player.permalink)).concat([
            { text: qsTr("Copy link at the current time"), icon: "clock", enabled: Player.permalink.length > 0,
              run: () => copyLink(linkAt(Player.permalink, Player.position)) }
        ]), item, point)
    }
    function showWaveformMenu(fraction, item, point) {
        const at = fraction * Player.duration
        contextMenu.show([
            { text: qsTr("Play from %1").arg(Format.duration(at)), icon: "play", enabled: Player.hasTrack,
              run: () => { Player.seek(at); if (!Player.playing) Player.play() } },
            { text: qsTr("Copy link at %1").arg(Format.duration(at)), icon: "clock",
              enabled: Player.permalink.length > 0, run: () => copyLink(linkAt(Player.permalink, at)) }
        ], item, point)
    }

    // --- Keyboard --------------------------------------------------------------------------------

    // Remappable (Settings → Keyboard). Text fields keep Space for typing (they accept it before shortcuts see it).
    component ActionShortcut: Shortcut {
        property string action
        readonly property string keys: Shortcuts.sequences[action] || ""
        sequence: keys
        enabled: keys.length > 0
    }
    ActionShortcut { action: "play_pause"; onActivated: Player.playPause() }
    ActionShortcut { action: "next"; onActivated: Player.next() }
    ActionShortcut { action: "previous"; onActivated: Player.previous() }
    ActionShortcut { action: "seek_forward"; onActivated: Player.seekBy(5000) }
    ActionShortcut { action: "seek_backward"; onActivated: Player.seekBy(-5000) }
    ActionShortcut { action: "volume_up"; onActivated: Player.volume = Math.min(1, Player.volume + 0.05) }
    ActionShortcut { action: "volume_down"; onActivated: Player.volume = Math.max(0, Player.volume - 0.05) }
    ActionShortcut { action: "mute"; onActivated: Player.toggleMute() }
    ActionShortcut { action: "shuffle"; onActivated: Player.toggleShuffle() }
    ActionShortcut { action: "repeat"; onActivated: Player.cycleRepeat() }
    ActionShortcut { action: "queue"; onActivated: window.queuePanel = !window.queuePanel }
    ActionShortcut {
        action: "full_player"
        onActivated: window.fullPlayer ? window.closeFullPlayer() : window.openFullPlayer()
    }
    ActionShortcut {
        action: "search"
        onActivated: {
            window.fullPlayer = false
            window.details = []  // also when the page already is "search" (no pageChanged then)
            window.page = "search"
            Qt.callLater(() => { if (searchPage.item) searchPage.item.focusField() })
        }
    }
    ActionShortcut { action: "back"; onActivated: window.goBack() }
    ActionShortcut { action: "settings"; onActivated: { window.fullPlayer = false; window.page = "settings" } }
    ActionShortcut { action: "appearance"; onActivated: window.appearancePanel = !window.appearancePanel }
    ActionShortcut { action: "cheat_sheet"; onActivated: window.cheatSheet = !window.cheatSheet }
    // Fixed: hardware media keys (while the window has focus; MPRIS covers the rest) and full screen.
    Shortcut {
        sequences: ["Media Play", "Toggle Media Play/Pause"]
        onActivated: Player.playPause()
    }
    Shortcut { sequence: "Media Pause"; onActivated: Player.pause() }
    Shortcut { sequence: "Media Next"; onActivated: Player.next() }
    Shortcut { sequence: "Media Previous"; onActivated: Player.previous() }
    Shortcut { sequences: ["Volume Up"]; onActivated: Player.volume = Math.min(1, Player.volume + 0.05) }
    Shortcut { sequences: ["Volume Down"]; onActivated: Player.volume = Math.max(0, Player.volume - 0.05) }
    Shortcut { sequences: ["Volume Mute"]; onActivated: Player.toggleMute() }
    Shortcut { sequence: "F11"; onActivated: window.toggleFullScreen() }
    Shortcut { sequence: "Escape"; enabled: window.fullPlayer || window.details.length > 0; onActivated: window.goBack() }

    // Closing hides the window in the tray when that is wanted (playback goes on); Quit in the tray ends it.
    onClosing: close => {
        if (Layout.closeToTray && Tray.active) {
            close.accepted = false
            window.hide()
        }
    }

    // --- Scene -----------------------------------------------------------------------------------

    Backdrop {
        id: backdrop
        anchors.fill: parent
    }
    Loader {
        id: blur
        anchors.fill: parent
        active: Theme.glass.blurred && Graphics.hardware
        sourceComponent: BackdropBlur { source: backdrop }
    }

    TitleBar {
        id: titleBar
        visible: Layout.customDecorations && window.visibility !== Window.FullScreen
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: visible ? implicitHeight : 0
    }

    // Auto-hide: the navigation slides in when the pointer reaches its edge.
    Item {
        id: edge
        visible: window.railAutoHide
        x: window.railSide === "right" ? parent.width - width : 0
        y: window.railSide === "bottom" ? parent.height - height : window.topInset
        width: window.railSide === "bottom" ? parent.width : 8
        height: window.railSide === "bottom" ? 8 : parent.height - window.topInset
        HoverHandler { id: edgeHover }
    }

    NavRail {
        id: rail
        readonly property real hiddenShift: window.railRevealed ? 0 : (horizontal ? height : width) + window.gap * 2
        horizontal: window.railSide === "bottom"
        expanded: Layout.navMode === "expanded"
        x: window.railSide === "left" ? window.gap - hiddenShift
           : window.railSide === "right" ? parent.width - width - window.gap + hiddenShift : window.gap
        y: horizontal ? parent.height - height - window.gap + hiddenShift : window.topInset
        width: horizontal ? parent.width - window.gap * 2 : implicitWidth
        height: horizontal ? implicitHeight : parent.height - window.topInset - window.gap
        z: window.railAutoHide ? 3 : 0
        Behavior on x { enabled: window.railAutoHide; NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
        Behavior on y { enabled: window.railAutoHide; NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
        destinations: window.destinations
        footer: window.footer
        current: window.page
        onNavigate: id => {
            window.fullPlayer = false
            if (id === window.page)
                window.details = []
            window.page = id
        }
        onSecondaryActivated: id => {
            if (id === "settings")
                window.appearancePanel = true
        }
        HoverHandler { id: railHover }
    }

    GlassPanel {
        id: notice
        visible: App.graphicsNotice.length > 0
        level: 2
        radius: Theme.radius.md
        x: window.leftInset
        y: window.topInset
        width: parent.width - window.leftInset - window.rightInset
        height: visible ? Math.max(noticeText.implicitHeight, dismiss.implicitHeight) + Theme.space.md * 2 : 0
        Accessible.role: Accessible.AlertMessage
        Accessible.name: App.graphicsNotice

        Icon {
            id: noticeIcon
            anchors.left: parent.left
            anchors.leftMargin: Theme.space.lg
            anchors.verticalCenter: parent.verticalCenter
            name: "triangle-alert"
            color: Theme.targetColors.warning
        }
        Text {
            id: noticeText
            anchors.left: noticeIcon.right
            anchors.leftMargin: Theme.space.md
            anchors.right: dismiss.left
            anchors.rightMargin: Theme.space.md
            anchors.verticalCenter: parent.verticalCenter
            text: App.graphicsNotice
            font: Theme.font.body
            color: Theme.colors.textPrimary
            wrapMode: Text.Wrap
        }
        Button {
            id: dismiss
            anchors.right: parent.right
            anchors.rightMargin: Theme.space.md
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Got it")
            onClicked: App.dismissGraphicsNotice()
        }
    }

    NowPlayingBar {
        id: player
        readonly property real restY: window.playerOnTop ? (notice.visible ? notice.y + notice.height + window.gap : window.topInset)
                                                         : parent.height - height - window.bottomInset
        readonly property real hiddenY: window.playerOnTop ? -height - window.gap : parent.height + window.gap
        x: window.leftInset
        width: parent.width - window.leftInset - window.rightInset
        y: window.playerShown ? restY : hiddenY
        height: implicitHeight
        visible: window.playerShown || (y > -height && y < parent.height)
        queueOpen: window.queuePanel
        onQueueRequested: window.queuePanel = !window.queuePanel
        onFullPlayerRequested: window.openFullPlayer()
        Behavior on y { NumberAnimation { duration: Theme.motion.slow; easing.type: Theme.motion.easingEmphasized } }
    }

    Item {
        id: pages
        x: window.leftInset
        width: parent.width - window.leftInset - window.rightInset
        y: window.playerShown && window.playerOnTop ? player.restY + player.height + window.gap
           : (notice.visible ? notice.y + notice.height + window.gap : window.topInset)
        height: (window.playerShown && !window.playerOnTop ? player.restY - window.gap : parent.height - window.bottomInset) - y

        // Pages are created on first visit and kept alive afterwards.
        component Page: Loader {
            required property string pageId
            anchors.fill: parent
            active: false
            visible: window.page === pageId && window.detail === null
            opacity: visible ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.motion.normal } }
            onVisibleChanged: if (visible) active = true
            Component.onCompleted: active = visible
        }

        Page { pageId: "home"; sourceComponent: HomePage {} }
        Page { id: searchPage; pageId: "search"; sourceComponent: SearchPage {} }
        Page { pageId: "library"; sourceComponent: LibraryPage {} }
        Page { pageId: "likes"; sourceComponent: LikesPage {} }
        Page { pageId: "history"; sourceComponent: HistoryPage {} }
        Page { pageId: "charts"; sourceComponent: ChartsPage {} }
        Page {
            pageId: "settings"
            sourceComponent: SettingsView {
                category: window.settingsCategory
                onCategoryRequested: id => window.settingsCategory = id
            }
        }

        // The playlist or person opened from the current page, with a way back.
        Item {
            anchors.fill: parent
            visible: window.detail !== null
            IconButton {
                id: back
                iconName: "chevron-left"
                text: qsTr("Back")
                onClicked: window.goBack()
            }
            Loader {
                anchors.top: back.bottom
                anchors.topMargin: Theme.space.md
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                active: window.detail !== null
                // No page without a detail: `active` may be updated after this, and a user page made for a moment
                // when a playlist opens would look up a user with the playlist's id.
                sourceComponent: !window.detail ? null : window.detail.kind === "playlist" ? playlistPage : userPage
            }
            Component {
                id: playlistPage
                PlaylistPage { detail: Catalog.playlist(window.detail.id) }
            }
            Component {
                id: userPage
                UserPage { detail: Catalog.user(window.detail.id) }
            }
        }
    }

    QueueDrawer {
        anchors.right: parent.right
        anchors.rightMargin: window.rightInset
        y: window.playerShown && window.playerOnTop ? player.restY + player.height + window.gap : window.topInset
        height: (window.playerShown && !window.playerOnTop && !window.fullPlayer ? player.restY - window.gap
                                                                                 : parent.height - window.bottomInset) - y
        width: Math.min(parent.width - window.leftInset - window.rightInset, 420 * Theme.scale)
        z: 4
        open: window.queuePanel
        onCloseRequested: window.queuePanel = false
    }

    AppearanceDrawer {
        anchors.right: parent.right
        anchors.rightMargin: window.rightInset
        y: window.topInset
        height: parent.height - window.topInset - window.gap
        width: parent.width - window.leftInset - window.rightInset
        z: 4
        open: window.appearancePanel
        onCloseRequested: window.appearancePanel = false
        onAllSettingsRequested: {
            window.appearancePanel = false
            window.settingsCategory = "appearance"
            window.page = "settings"
        }
    }

    FullPlayer {
        id: fullPlayerView
        anchors.fill: parent
        anchors.topMargin: titleBar.height
        z: 3
        open: window.fullPlayer
        onCloseRequested: window.closeFullPlayer()
    }

    CheatSheet {
        anchors.fill: parent
        z: 5
        open: window.cheatSheet
        onCloseRequested: window.cheatSheet = false
    }

    // Toast actions such as "Sign in" open a page.
    Connections {
        target: App
        function onPageRequested(page, category) {
            if (category.length > 0)
                window.settingsCategory = category
            window.appearancePanel = false
            window.fullPlayer = false
            window.page = page
        }
    }

    ContextMenu { id: contextMenu }

    ToastHost { anchors.fill: parent; z: 6 }
    ResizeEdges {
        anchors.fill: parent
        z: 7
        visible: Layout.customDecorations && window.visibility === Window.Windowed
    }
}
