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
    // Shows the player bar without a track (smoke tests and screenshots).
    property bool playerPreview: false
    readonly property bool playerShown: Player.hasTrack || playerPreview
    // The shared blurred backdrop sampled by every GlassPanel (null when no blur style is in use).
    readonly property Item backdropTexture: blur.item ? blur.item.output : null

    readonly property var destinations: [
        { id: "home", icon: "house", label: qsTr("Home") },
        { id: "search", icon: "search", label: qsTr("Search") },
        { id: "library", icon: "library", label: qsTr("Library") },
        { id: "likes", icon: "heart", label: qsTr("Likes") },
        { id: "history", icon: "clock", label: qsTr("History") },
        { id: "charts", icon: "trending-up", label: qsTr("Charts") }
    ]
    readonly property var footer: [{ id: "settings", icon: "settings", label: qsTr("Settings") }]

    width: 1280
    height: 820
    minimumWidth: 880
    minimumHeight: 600
    visible: false  // shown from C++ once the graphics configuration (GPU timestamps) is set
    color: Theme.colors.backdrop
    title: qsTr("Noctara")

    Shortcut {
        sequences: [StandardKey.Preferences, "Ctrl+,"]
        onActivated: window.page = "settings"
    }
    Shortcut {
        sequence: "Ctrl+Shift+A"
        onActivated: window.appearancePanel = !window.appearancePanel
    }
    // Playback. Text fields keep Space for typing (they accept it before shortcuts see it).
    Shortcut {
        sequences: ["Space", "Media Play", "Toggle Media Play/Pause"]
        enabled: Player.hasTrack
        onActivated: Player.playPause()
    }
    Shortcut {
        sequences: ["Media Pause"]
        onActivated: Player.pause()
    }
    Shortcut {
        sequences: ["Ctrl+Right", "Media Next"]
        onActivated: Player.next()
    }
    Shortcut {
        sequences: ["Ctrl+Left", "Media Previous"]
        onActivated: Player.previous()
    }
    Shortcut {
        sequences: ["Shift+Right"]
        onActivated: Player.seekBy(5000)
    }
    Shortcut {
        sequences: ["Shift+Left"]
        onActivated: Player.seekBy(-5000)
    }
    Shortcut {
        sequences: ["Ctrl+Up", "Volume Up"]
        onActivated: Player.volume = Math.min(1, Player.volume + 0.05)
    }
    Shortcut {
        sequences: ["Ctrl+Down", "Volume Down"]
        onActivated: Player.volume = Math.max(0, Player.volume - 0.05)
    }
    Shortcut {
        sequences: ["Ctrl+M", "Volume Mute"]
        onActivated: Player.toggleMute()
    }
    Shortcut {
        sequence: "Ctrl+Shift+Q"
        onActivated: window.queuePanel = !window.queuePanel
    }

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

    NavRail {
        id: rail
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space.lg
        destinations: window.destinations
        footer: window.footer
        current: window.page
        onNavigate: id => window.page = id
        onSecondaryActivated: id => {
            if (id === "settings")
                window.appearancePanel = true
        }
    }

    GlassPanel {
        id: notice
        visible: App.graphicsNotice.length > 0
        level: 2
        radius: Theme.radius.md
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space.lg
        anchors.leftMargin: Theme.space.lg
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

    Item {
        id: pages
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.top: notice.visible ? notice.bottom : parent.top
        anchors.bottom: window.playerShown ? player.top : parent.bottom
        anchors.margins: Theme.space.lg
        anchors.leftMargin: Theme.space.lg

        // Pages are created on first visit and kept alive afterwards.
        component Page: Loader {
            required property string pageId
            anchors.fill: parent
            active: false
            visible: window.page === pageId
            opacity: visible ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.motion.normal } }
            onVisibleChanged: if (visible) active = true
            Component.onCompleted: active = visible
        }

        Page { pageId: "home"; sourceComponent: HomePage {} }
        Page { pageId: "search"; sourceComponent: SearchPage {} }
        Page {
            pageId: "library"
            sourceComponent: EmptyPage {
                iconName: "library"
                title: qsTr("Your library")
                message: qsTr("Playlists, albums and followed artists will appear here.")
            }
        }
        Page {
            pageId: "likes"
            sourceComponent: EmptyPage {
                iconName: "heart"
                title: qsTr("Liked tracks")
                message: qsTr("Tracks you like on SoundCloud will appear here.")
            }
        }
        Page {
            pageId: "history"
            sourceComponent: EmptyPage {
                iconName: "clock"
                title: qsTr("Listening history")
                message: qsTr("Recently played tracks will appear here.")
            }
        }
        Page {
            pageId: "charts"
            sourceComponent: EmptyPage {
                iconName: "trending-up"
                title: qsTr("Charts")
                message: qsTr("Top and trending tracks by genre will appear here.")
            }
        }
        Page {
            pageId: "settings"
            sourceComponent: SettingsView {
                category: window.settingsCategory
                onCategoryRequested: id => window.settingsCategory = id
            }
        }
    }

    NowPlayingBar {
        id: player
        anchors.left: rail.right
        anchors.right: parent.right
        anchors.margins: Theme.space.lg
        y: window.playerShown ? parent.height - height - Theme.space.lg : parent.height + Theme.space.lg
        height: implicitHeight
        visible: y < parent.height
        queueOpen: window.queuePanel
        onQueueRequested: window.queuePanel = !window.queuePanel
        Behavior on y { NumberAnimation { duration: Theme.motion.slow; easing.type: Theme.motion.easingEmphasized } }
    }

    QueueDrawer {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: window.playerShown ? player.top : parent.bottom
        anchors.margins: Theme.space.lg
        width: parent.width - rail.width - Theme.space.lg * 3
        open: window.queuePanel
        onCloseRequested: window.queuePanel = false
    }

    AppearanceDrawer {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space.lg
        width: parent.width - rail.width - Theme.space.lg * 3
        open: window.appearancePanel
        onCloseRequested: window.appearancePanel = false
        onAllSettingsRequested: {
            window.appearancePanel = false
            window.settingsCategory = "appearance"
            window.page = "settings"
        }
    }

    // Toast actions such as "Sign in" open a page.
    Connections {
        target: App
        function onPageRequested(page, category) {
            if (category.length > 0)
                window.settingsCategory = category
            window.appearancePanel = false
            window.page = page
        }
    }

    ToastHost { anchors.fill: parent }
}
