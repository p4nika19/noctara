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
    visible: true
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

    Backdrop { anchors.fill: parent }

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
        anchors.leftMargin: Theme.space.xl
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
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space.lg
        anchors.leftMargin: Theme.space.xl

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
        Page {
            pageId: "search"
            sourceComponent: EmptyPage {
                iconName: "search"
                title: qsTr("Search SoundCloud")
                message: qsTr("Find tracks, playlists and people once your account is connected.")
            }
        }
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

    ToastHost { anchors.fill: parent }

    // Fine grain over everything breaks up banding in the gradients (static texture, no per-frame cost).
    Image {
        anchors.fill: parent
        source: "image://noise/128"
        fillMode: Image.Tile
        smooth: false
        opacity: Theme.glass.grain * 0.08
        visible: opacity > 0.002
        enabled: false
        Accessible.ignored: true
    }
}
