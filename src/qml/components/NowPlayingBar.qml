import QtQuick
import QtQuick.Templates as T
import QtQuick.Window
import Noctara

// The floating "now playing" bar: cover and title, transport, the waveform scrubber with times, volume and the
// queue. Liquid Glass applies here (one of the few surfaces that bend light). What it shows and its size follow
// Layout → Player bar; narrow windows collapse the inline volume slider into a hover popover above the mute button,
// then drop the times beside the scrubber. Clicking the cover opens the full-screen player; right-click opens the
// player menu.
GlassPanel {
    id: bar

    property bool queueOpen: false
    signal queueRequested()
    signal fullPlayerRequested()
    readonly property Item coverItem: cover

    readonly property real inset: Math.max(Theme.space.md, cornerInset)
    readonly property bool roomy: width > 900 * Theme.scale
    // When the bar is too narrow for the inline volume slider, hovering the mute button reveals a vertical slider.
    readonly property bool compactVolume: !roomy && Layout.playerVolume
    readonly property bool volumeInteractive: muteButton.hovered || volumePopupHover.hovered
                                              || volumePopupSlider.hovered || volumePopupSlider.pressed
    // Between the transport and the volume controls; the times give up their room before the scrubber gets too short.
    readonly property real scrubRoom: volume.x - transport.x - transport.width - Theme.space.lg * 2
    readonly property bool showTimes: scrubRoom - elapsed.implicitWidth - total.implicitWidth - Theme.space.md * 2
                                      >= 96 * Theme.scale
    readonly property real progress: Player.duration > 0 ? Math.min(1, Player.position / Player.duration) : 0

    function positionVolumePopup() {
        const overlay = volumePopup.parent
        if (!overlay)
            return
        const anchor = muteButton.mapToItem(overlay, muteButton.width / 2, 0)
        const barOrigin = bar.mapToItem(overlay, 0, 0)
        const popupW = volumePopup.implicitWidth
        const popupH = volumePopup.implicitHeight
        const margin = Theme.space.sm
        const gap = Theme.space.sm
        const below = Layout.playerPosition === "top"
        volumePopup.x = Math.max(margin, Math.min(anchor.x - popupW / 2, overlay.width - popupW - margin))
        const desiredY = below ? barOrigin.y + bar.height + gap : barOrigin.y - popupH - gap
        volumePopup.y = Math.max(margin, Math.min(desiredY, overlay.height - popupH - margin))
    }

    function syncVolumePopup() {
        if (!bar.compactVolume || !bar.visible) {
            volumeCloseTimer.stop()
            if (volumePopup.visible)
                volumePopup.close()
            return
        }
        if (bar.volumeInteractive) {
            volumeCloseTimer.stop()
            bar.positionVolumePopup()
            if (!volumePopup.visible)
                volumePopup.open()
        } else if (volumePopup.visible) {
            volumeCloseTimer.restart()
        }
    }

    onCompactVolumeChanged: syncVolumePopup()
    onVisibleChanged: syncVolumePopup()
    onVolumeInteractiveChanged: syncVolumePopup()
    onWidthChanged: if (volumePopup.visible) positionVolumePopup()
    onHeightChanged: if (volumePopup.visible) positionVolumePopup()
    onXChanged: if (volumePopup.visible) positionVolumePopup()
    onYChanged: if (volumePopup.visible) positionVolumePopup()

    level: 1
    liquid: true
    radius: Theme.radius.lg
    implicitHeight: Theme.space.nowPlayingHeight * (Layout.playerSize === "compact" ? 0.8
                                                    : Layout.playerSize === "large" ? 1.3 : 1)
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Now playing")

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onClicked: mouse => Window.window.showPlayerMenu(bar, Qt.point(mouse.x, mouse.y))
    }

    CoverArt {
        id: cover
        anchors.left: parent.left
        anchors.leftMargin: bar.inset
        anchors.verticalCenter: parent.verticalCenter
        height: bar.height - Theme.space.md * 2
        width: height
        // Concentric with the bar's rounded end.
        radius: Math.max(0, Math.min(bar.radius, bar.height / 2) - Theme.space.md)
        source: Player.coverUrl
        MouseArea {
            anchors.fill: parent
            enabled: Player.hasTrack
            cursorShape: Qt.PointingHandCursor
            onClicked: bar.fullPlayerRequested()
        }
    }

    Column {
        id: info
        anchors.left: cover.right
        anchors.leftMargin: Theme.space.md
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(120 * Theme.scale, Math.min(260 * Theme.scale, bar.width * 0.2))
        spacing: Theme.space.xxs

        Text {
            width: Math.min(implicitWidth, parent.width)
            text: Player.title
            font: Theme.font.bodyStrong
            color: titleArea.containsMouse ? Theme.colors.accent : Theme.colors.textPrimary
            elide: Text.ElideRight
            textFormat: Text.PlainText
            MouseArea {
                id: titleArea
                anchors.fill: parent
                enabled: Player.hasTrack
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: Window.window.openTrack(Number(Player.trackId))
            }
        }
        Text {
            width: parent.width
            text: Player.errorCode >= 0 ? Format.error(Player.errorCode)
                  : (Player.preview ? qsTr("Preview · %1").arg(Player.artist) : Player.artist)
            font: Theme.font.caption
            color: Player.errorCode >= 0 ? Theme.colors.warning : Theme.colors.textSecondary
            elide: Text.ElideRight
            textFormat: Text.PlainText
        }
    }

    Row {
        id: transport
        anchors.left: info.right
        anchors.leftMargin: Theme.space.lg
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.xs

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.space.controlHeightSmall
            height: width
            iconName: "shuffle"
            iconSize: Theme.space.iconSizeSmall
            checked: Player.shuffle
            text: Player.shuffle ? qsTr("Shuffle: on") : qsTr("Shuffle: off")
            onClicked: Player.toggleShuffle()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "skip-back"
            enabled: Player.hasTrack
            text: qsTr("Previous")
            onClicked: Player.previous()
        }
        IconButton {
            id: playButton
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.space.controlHeight + Theme.space.sm
            height: width
            material: "primary"
            iconName: Player.playing ? "pause" : "play"
            enabled: Player.hasTrack
            text: Player.playing ? qsTr("Pause") : qsTr("Play")
            onClicked: Player.playPause()

            // Waiting for audio: a slowly breathing ring instead of a spinner.
            Rectangle {
                anchors.centerIn: parent
                width: parent.width + Theme.space.xs * 2
                height: width
                radius: width / 2
                color: "transparent"
                border.width: 2 * Theme.scale
                border.color: Theme.colors.accent
                visible: Player.loading && Player.playing
                opacity: 0.6
                SequentialAnimation on opacity {
                    running: Player.loading && Player.playing && !Theme.motion.reduced
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.15; duration: Theme.motion.shimmer / 2 }
                    NumberAnimation { to: 0.6; duration: Theme.motion.shimmer / 2 }
                }
            }
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "skip-forward"
            enabled: Player.canGoNext
            text: qsTr("Next")
            onClicked: Player.next()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.space.controlHeightSmall
            height: width
            iconName: Player.repeat === "one" ? "repeat-1" : "repeat"
            iconSize: Theme.space.iconSizeSmall
            checked: Player.repeat !== "off"
            text: Player.repeat === "one" ? qsTr("Repeat: this track")
                  : (Player.repeat === "all" ? qsTr("Repeat: all") : qsTr("Repeat: off"))
            onClicked: Player.cycleRepeat()
        }
    }

    Text {
        id: elapsed
        anchors.left: transport.right
        anchors.leftMargin: Theme.space.lg
        anchors.verticalCenter: parent.verticalCenter
        visible: bar.showTimes
        width: bar.showTimes ? implicitWidth : 0
        text: Layout.playerTime && Format.revision >= 0 ? Format.duration(waveform.dragging ? waveform.shownProgress * Player.duration
                                                                       : Player.position) : ""
        font: Theme.font.numeric
        color: Theme.colors.textSecondary
    }
    WaveformView {
        id: waveform
        anchors.left: elapsed.right
        anchors.leftMargin: bar.showTimes ? Theme.space.md : 0
        anchors.right: total.left
        anchors.rightMargin: bar.showTimes ? Theme.space.md : 0
        anchors.verticalCenter: parent.verticalCenter
        // Off: a plain line (no peaks, thin bars) instead of the waveform.
        implicitHeight: (Layout.playerWaveform ? 32 : 10) * Theme.scale
        barWidth: (Layout.playerWaveform ? 2.5 : 1) * Theme.scale
        barGap: (Layout.playerWaveform ? 1.5 : 0) * Theme.scale
        samples: Layout.playerWaveform ? Player.waveform : []
        progress: bar.progress
        storedRanges: Player.cachedRanges
        duration: Player.duration
        enabled: Player.hasTrack
        onSeekRequested: fraction => Player.seek(fraction * Player.duration)
        onMenuRequested: (fraction, point) => Window.window.showWaveformMenu(fraction, waveform, point)
    }
    Text {
        id: total
        anchors.right: volume.left
        anchors.rightMargin: Theme.space.lg
        anchors.verticalCenter: parent.verticalCenter
        visible: bar.showTimes
        width: bar.showTimes ? implicitWidth : 0
        text: Layout.playerTime && Format.revision >= 0 ? Format.duration(Player.duration) : ""
        font: Theme.font.numeric
        color: Theme.colors.textTertiary
    }

    Row {
        id: volume
        anchors.right: queueButton.left
        anchors.rightMargin: Theme.space.sm
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.xs

        IconButton {
            id: muteButton
            objectName: "volumeMuteButton"
            anchors.verticalCenter: parent.verticalCenter
            width: Theme.space.controlHeightSmall
            height: width
            material: "flat"
            iconSize: Theme.space.iconSizeSmall
            iconName: Player.muted || Player.volume === 0 ? "volume-x" : (Player.volume < 0.5 ? "volume-1" : "volume-2")
            text: Player.muted ? qsTr("Unmute") : qsTr("Mute")
            showTooltip: !bar.compactVolume
            onClicked: Player.toggleMute()
            WheelHandler {
                enabled: bar.compactVolume
                onWheel: event => Player.volume = Math.max(0, Math.min(1, Player.volume + event.angleDelta.y / 1200))
            }
        }
        Slider {
            objectName: "volumeInlineSlider"
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.roomy && Layout.playerVolume
            width: 96 * Theme.scale
            from: 0
            to: 1
            value: Player.muted ? 0 : Player.volume
            Accessible.name: qsTr("Volume")
            onMoved: Player.volume = value
            WheelHandler {
                onWheel: event => Player.volume = Math.max(0, Math.min(1, Player.volume + event.angleDelta.y / 1200))
            }
        }
    }

    // Brief grace period while the pointer moves across the gap between the mute button and the vertical slider.
    Timer {
        id: volumeCloseTimer
        interval: 130
        repeat: false
        onTriggered: if (!bar.volumeInteractive) volumePopup.close()
    }

    Connections {
        target: bar.Window.window
        function onActiveChanged() {
            if (bar.Window.window && !bar.Window.window.active && !volumePopupSlider.pressed) {
                volumeCloseTimer.stop()
                volumePopup.close()
            }
        }
    }

    T.Popup {
        id: volumePopup
        objectName: "volumePopup"
        parent: T.Overlay.overlay
        modal: false
        focus: false
        closePolicy: T.Popup.CloseOnEscape
        leftPadding: Theme.space.xs
        rightPadding: Theme.space.xs
        topPadding: Theme.space.md
        bottomPadding: Theme.space.md
        implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding

        contentItem: Item {
            implicitWidth: volumePopupSlider.implicitWidth
            implicitHeight: volumePopupSlider.implicitHeight

            WheelHandler {
                onWheel: event => Player.volume = Math.max(0, Math.min(1, Player.volume + event.angleDelta.y / 1200))
            }

            Slider {
                id: volumePopupSlider
                objectName: "volumePopupSlider"
                anchors.centerIn: parent
                orientation: Qt.Vertical
                implicitHeight: 112 * Theme.scale
                focusPolicy: Qt.NoFocus
                from: 0
                to: 1
                value: Player.muted ? 0 : Player.volume
                Accessible.name: qsTr("Volume")
                onMoved: Player.volume = value
            }
        }
        background: GlassPanel {
            level: 2
            liquid: true
            radius: Math.min(Theme.radius.md, width / 2)
            HoverHandler { id: volumePopupHover }
        }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motion.fast }
            NumberAnimation {
                property: "scale"
                from: 0.94
                to: 1
                duration: Theme.motion.normal
                easing.type: Theme.motion.easingStandard
            }
        }
        exit: Transition {
            NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Math.round(Theme.motion.fast / 2) }
        }
    }
    IconButton {
        id: queueButton
        anchors.right: parent.right
        anchors.rightMargin: bar.inset
        anchors.verticalCenter: parent.verticalCenter
        visible: Layout.playerQueueButton
        width: visible ? implicitWidth : 0
        iconName: "list-music"
        checked: bar.queueOpen
        text: qsTr("Queue")
        onClicked: bar.queueRequested()
    }
}
