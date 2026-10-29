import QtQuick
import QtQuick.Window
import Noctara

// The full-screen player: the current track large over its own artwork backdrop, with transport, the waveform,
// volume and (when the window is wide enough) the queue beside it. Opening flies the cover from the player bar
// into place (shared element); F11 or the corner button also puts the window itself into full screen.
// Escape, the close button or the shortcut leave it.
FocusScope {
    id: root

    property bool open: false
    // Where the player bar shows the cover, in window coordinates (the flight starts and ends there).
    property rect sourceRect: Qt.rect(0, 0, 0, 0)
    signal closeRequested()

    readonly property bool wide: width > 1100 * Theme.scale
    readonly property bool showQueue: Layout.fullPlayerQueue && wide
    readonly property real artSide: {
        const room = Math.min(height - Theme.space.xxxl * 2 - 300 * Theme.scale,
                              (showQueue ? width * 0.42 : width * 0.5))
        return Math.max(160 * Theme.scale, room * (Layout.fullPlayerArt === "large" ? 1 : 0.75))
    }
    readonly property real progress: Player.duration > 0 ? Math.min(1, Player.position / Player.duration) : 0
    // 0 closed … 1 open; drives the scrim, the content fade and the cover flight.
    property real shown: open ? 1 : 0
    Behavior on shown { NumberAnimation { duration: Theme.motion.slow; easing.type: Theme.motion.easingEmphasized } }

    visible: shown > 0
    focus: open
    Keys.onEscapePressed: root.closeRequested()
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Now playing")

    // The cover art, heavily blurred, over the window's backdrop (pre-blurred on the CPU, so it works without a
    // GPU too) and a scrim that keeps the text readable.
    Rectangle {
        anchors.fill: parent
        color: Theme.colors.backdrop
        opacity: root.shown
    }
    Image {
        anchors.fill: parent
        source: Artwork.hasArtwork ? "image://artwork/" + (Theme.dark ? "dark" : "light") + "/" + Artwork.revision : ""
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
        cache: false
        opacity: root.shown
    }
    Rectangle {
        anchors.fill: parent
        opacity: root.shown
        gradient: Gradient {
            GradientStop { position: 0.0; color: Qt.rgba(Theme.colors.scrim.r, Theme.colors.scrim.g, Theme.colors.scrim.b, 0.05) }
            GradientStop { position: 0.6; color: Qt.rgba(Theme.colors.scrim.r, Theme.colors.scrim.g, Theme.colors.scrim.b, 0.3) }
            GradientStop { position: 1.0; color: Qt.rgba(Theme.colors.scrim.r, Theme.colors.scrim.g, Theme.colors.scrim.b, 0.65) }
        }
    }
    MouseArea {  // swallow clicks so nothing underneath reacts
        anchors.fill: parent
        enabled: root.open
        acceptedButtons: Qt.AllButtons
        onWheel: wheel => Player.volume = Math.max(0, Math.min(1, Player.volume + wheel.angleDelta.y / 1200))
    }

    // Top bar.
    Row {
        id: topBar
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.space.xl
        spacing: Theme.space.sm
        opacity: root.shown
        IconButton {
            visible: !root.wide || !Layout.fullPlayerQueue
            iconName: "list-music"
            checked: Window.window && Window.window.queuePanel
            text: qsTr("Queue")
            onClicked: Window.window.queuePanel = !Window.window.queuePanel
        }
        IconButton {
            iconName: Window.window && Window.window.visibility === Window.FullScreen ? "minimize-2" : "maximize-2"
            text: Window.window && Window.window.visibility === Window.FullScreen ? qsTr("Leave full screen")
                                                                                   : qsTr("Full screen")
            onClicked: Window.window.toggleFullScreen()
        }
        IconButton {
            iconName: "chevron-down"
            text: qsTr("Close the player")
            onClicked: root.closeRequested()
        }
    }
    Text {
        anchors.left: parent.left
        anchors.verticalCenter: topBar.verticalCenter
        anchors.leftMargin: Theme.space.xl
        opacity: root.shown
        text: Player.context.length > 0 ? qsTr("Playing from %1").arg(Player.context) : qsTr("Now playing")
        font: Theme.font.eyebrow
        color: Theme.colors.textTertiary
        textFormat: Text.PlainText
        elide: Text.ElideRight
        width: Math.min(implicitWidth, parent.width - topBar.width - Theme.space.xl * 3)
    }

    Item {
        id: stage
        anchors.top: topBar.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: queuePanel.visible ? queuePanel.left : parent.right
        anchors.margins: Theme.space.xl

        Column {
            id: main
            anchors.centerIn: parent
            width: Math.min(parent.width, Math.max(root.artSide, 520 * Theme.scale))
            spacing: Theme.space.lg

            // Placeholder where the cover lands; the flying cover is drawn above everything.
            Item {
                id: artSlot
                anchors.horizontalCenter: parent.horizontalCenter
                width: root.artSide
                height: width
                Shadow {
                    anchors.fill: parent
                    z: -1
                    radius: Theme.radius.lg
                    blur: 120 * Theme.scale
                    offsetY: 30 * Theme.scale
                    spread: -10 * Theme.scale
                    opacity: root.shown
                    color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                                   Theme.glass.glowCover && !Theme.glass.flat ? Theme.glass.glow * 1.2 : 0.15)
                }
            }

            Column {
                width: parent.width
                spacing: Theme.space.xs
                opacity: root.shown
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: Player.title
                    font: Theme.font.headline
                    color: Theme.colors.textPrimary
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: Player.errorCode >= 0 ? Format.error(Player.errorCode) : Player.artist
                    font: Theme.font.subtitle
                    color: Player.errorCode >= 0 ? Theme.colors.warning
                                                 : (artistArea.containsMouse ? Theme.colors.accent : Theme.colors.textSecondary)
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    MouseArea {
                        id: artistArea
                        anchors.centerIn: parent
                        width: Math.min(parent.width, parent.implicitWidth)
                        height: parent.height
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: if (Player.artistId > 0) {
                            root.closeRequested()
                            Window.window.openUser(Player.artistId)
                        }
                    }
                }
            }

            Item {
                width: parent.width
                height: Math.max(waveform.implicitHeight, Theme.font.numeric.pixelSize * 1.6)
                opacity: root.shown
                Text {
                    id: elapsed
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: Format.revision >= 0 ? Format.duration(waveform.dragging ? waveform.shownProgress * Player.duration
                                                                                   : Player.position) : ""
                    font: Theme.font.numeric
                    color: Theme.colors.textSecondary
                }
                WaveformView {
                    id: waveform
                    anchors.left: elapsed.right
                    anchors.right: total.left
                    anchors.margins: Theme.space.md
                    anchors.verticalCenter: parent.verticalCenter
                    implicitHeight: (Layout.fullPlayerWaveform ? 56 : 18) * Theme.scale
                    samples: Layout.fullPlayerWaveform ? Player.waveform : []
                    progress: root.progress
                    storedRanges: Player.cachedRanges
                    duration: Player.duration
                    barWidth: (Layout.fullPlayerWaveform ? 3 : 1) * Theme.scale
                    onSeekRequested: fraction => Player.seek(fraction * Player.duration)
                }
                Text {
                    id: total
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: Format.revision >= 0 ? Format.duration(Player.duration) : ""
                    font: Theme.font.numeric
                    color: Theme.colors.textTertiary
                }
            }

            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.space.md
                opacity: root.shown
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "shuffle"
                    checked: Player.shuffle
                    text: Player.shuffle ? qsTr("Shuffle: on") : qsTr("Shuffle: off")
                    onClicked: Player.toggleShuffle()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "skip-back"
                    iconSize: Theme.space.iconSizeLarge
                    implicitWidth: Theme.space.controlHeight * 1.3
                    implicitHeight: implicitWidth
                    text: qsTr("Previous")
                    onClicked: Player.previous()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    material: "primary"
                    iconName: Player.playing ? "pause" : "play"
                    iconSize: Theme.space.iconSizeLarge
                    implicitWidth: Theme.space.controlHeight * 1.8
                    implicitHeight: implicitWidth
                    text: Player.playing ? qsTr("Pause") : qsTr("Play")
                    onClicked: Player.playPause()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "skip-forward"
                    iconSize: Theme.space.iconSizeLarge
                    implicitWidth: Theme.space.controlHeight * 1.3
                    implicitHeight: implicitWidth
                    enabled: Player.canGoNext
                    text: qsTr("Next")
                    onClicked: Player.next()
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: Player.repeat === "one" ? "repeat-1" : "repeat"
                    checked: Player.repeat !== "off"
                    text: Player.repeat === "one" ? qsTr("Repeat: this track")
                          : (Player.repeat === "all" ? qsTr("Repeat: all") : qsTr("Repeat: off"))
                    onClicked: Player.cycleRepeat()
                }
            }
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.space.sm
                opacity: root.shown
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    material: "flat"
                    iconSize: Theme.space.iconSizeSmall
                    iconName: Player.muted || Player.volume === 0 ? "volume-x" : (Player.volume < 0.5 ? "volume-1" : "volume-2")
                    text: Player.muted ? qsTr("Unmute") : qsTr("Mute")
                    onClicked: Player.toggleMute()
                }
                Slider {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 180 * Theme.scale
                    from: 0
                    to: 1
                    value: Player.muted ? 0 : Player.volume
                    Accessible.name: qsTr("Volume")
                    onMoved: Player.volume = value
                }
            }
        }
    }

    GlassPanel {
        id: queuePanel
        visible: root.showQueue
        anchors.right: parent.right
        anchors.top: topBar.bottom
        anchors.bottom: parent.bottom
        anchors.margins: Theme.space.xl
        width: Math.min(380 * Theme.scale, parent.width * 0.3)
        level: 1
        radius: Theme.radius.lg
        opacity: root.shown
        Text {
            id: queueTitle
            x: Math.max(Theme.space.lg, queuePanel.cornerInset)
            y: Math.max(Theme.space.lg, queuePanel.cornerInset)
            text: qsTr("Up next")
            font: Theme.font.title
            color: Theme.colors.textPrimary
        }
        QueueList {
            anchors.top: queueTitle.bottom
            anchors.topMargin: Theme.space.md
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Math.max(Theme.space.sm, queuePanel.cornerInset)
            active: root.open && root.showQueue
            onActiveChanged: if (active) centerOnCurrent()
        }
    }

    // The flying cover: from the player bar into its slot while opening, and back while closing.
    CoverArt {
        id: flyer
        // Re-evaluated whenever anything that moves the slot changes (mapToItem alone does not track that).
        readonly property point slot: {
            void (artSlot.width + main.x + main.y + main.height + stage.x + stage.y + stage.width + stage.height)
            return artSlot.mapToItem(root, 0, 0)
        }
        x: root.sourceRect.x + (slot.x - root.sourceRect.x) * root.shown
        y: root.sourceRect.y + (slot.y - root.sourceRect.y) * root.shown
        width: root.sourceRect.width + (artSlot.width - root.sourceRect.width) * root.shown
        height: width
        radius: Theme.radius.sm + (Theme.radius.lg - Theme.radius.sm) * root.shown
        source: Player.artworkUrl
        Accessible.ignored: true
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: mouse => Window.window.showPlayerMenu(flyer, Qt.point(mouse.x, mouse.y))
        }
    }
}
