import QtQuick
import Noctara

// The waveform scrubber: SoundCloud's waveform with the played part, what is already stored on this computer and
// where a click would seek to. Click or drag to seek; the arrow keys step 5 seconds. One geometry (or two cached
// images without shaders), so playing only changes a uniform.
Item {
    id: root

    property var samples: []
    property real progress: 0  // 0…1
    property var storedRanges: []
    property real duration: 0  // ms, for the time under the pointer
    signal seekRequested(real fraction)

    readonly property bool dragging: area.pressed
    readonly property real shownProgress: area.pressed ? area.fraction : progress
    // The software renderer repaints images on colour changes: follow the target palette, not the crossfade.
    readonly property var tones: Graphics.hardware ? Theme.colors : Theme.targetColors

    implicitHeight: 32 * Theme.scale
    activeFocusOnTab: true
    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("Position")
    Accessible.description: Format.revision >= 0 ? Format.duration(progress * duration) : ""

    function step(ms) {
        if (duration > 0)
            seekRequested(Math.max(0, Math.min(1, progress + ms / duration)))
    }
    Keys.onLeftPressed: step(-5000)
    Keys.onRightPressed: step(5000)

    WaveformItem {
        anchors.fill: parent
        samples: root.samples
        progress: root.shownProgress
        hoverProgress: area.containsMouse && !area.pressed ? area.mouseX / width : -1
        storedRanges: root.storedRanges
        playedColor: root.tones.waveformPlayed
        unplayedColor: root.tones.waveformUnplayed
        storedColor: root.tones.waveformBuffered
        hoverColor: Qt.rgba(root.tones.waveformPlayed.r, root.tones.waveformPlayed.g,
                            root.tones.waveformPlayed.b, 0.5)
        barWidth: 2.5 * Theme.scale
        barGap: 1.5 * Theme.scale
    }

    MouseArea {
        id: area
        property real fraction: 0
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: mouse => fraction = Math.max(0, Math.min(1, mouse.x / width))
        onPositionChanged: mouse => {
            if (pressed)
                fraction = Math.max(0, Math.min(1, mouse.x / width))
        }
        onReleased: root.seekRequested(fraction)
    }

    // Time under the pointer.
    Rectangle {
        visible: (area.containsMouse || area.pressed) && root.duration > 0
        x: Math.max(0, Math.min(root.width - width, area.mouseX - width / 2))
        y: -height - Theme.space.xs
        width: hoverTime.implicitWidth + Theme.space.sm * 2
        height: hoverTime.implicitHeight + Theme.space.xs * 2
        radius: Theme.radius.xs
        color: Theme.colors.solidOverlay
        border.width: Theme.space.hairline
        border.color: Theme.colors.edge
        Text {
            id: hoverTime
            anchors.centerIn: parent
            text: Format.revision >= 0
                  ? Format.duration((area.pressed ? area.fraction : area.mouseX / root.width) * root.duration) : ""
            font: Theme.font.numeric
            color: Theme.colors.textPrimary
        }
    }

    FocusRing { target: root; targetRadius: Theme.radius.xs; visible: root.activeFocus }
}
