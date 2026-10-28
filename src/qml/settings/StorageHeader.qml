import QtQuick
import Noctara
import "../components"
import "network"

// Top of the Storage & Cache page: what the caches use on disk (one bar, one segment per kind), in memory, and
// where they live; the Clear actions, each confirmed first.
Column {
    id: root

    spacing: Theme.space.sm
    bottomPadding: Theme.space.md

    readonly property var shades: ({ "tracks": 0, "partial": 0.3, "covers": 0.5, "images": 0.65, "metadata": 0.8 })
    readonly property real barTotal: Math.max(1, Storage.totalBytes)

    function label(id) {
        switch (id) {
        case "tracks": return qsTr("Tracks")
        case "partial": return qsTr("Partly downloaded")
        case "covers": return qsTr("Track covers")
        case "images": return qsTr("Images")
        default: return qsTr("Metadata")
        }
    }
    // Opaque tones of the accent (segments overlap, so they must not be translucent): lighter steps in both
    // schemes, so they stay apart from each other and from the track.
    function shade(id) {
        const t = root.shades[id] * (Theme.dark ? 1 : 0.85)
        return Qt.tint(Theme.colors.accent, Qt.rgba(1, 1, 1, t))
    }
    function cumulative(index) {
        let sum = 0
        for (let i = 0; i <= index; ++i)
            sum += Storage.segments[i].bytes
        return sum
    }
    function ask(part) {
        confirm.part = part
        switch (part) {
        case "audio":
            confirm.title = qsTr("Remove cached tracks?")
            confirm.message = qsTr("Tracks will be downloaded again the next time you play them. Tracks that are playing right now are removed when they stop.")
            break
        case "images":
            confirm.title = qsTr("Remove cached images?")
            confirm.message = qsTr("Cover art and profile pictures will be downloaded again when they are shown.")
            break
        default:
            confirm.title = qsTr("Clear all caches?")
            confirm.message = qsTr("Cached tracks, images and metadata are removed. Your settings, likes and playlists are not affected.")
        }
        confirm.open()
    }

    Component.onCompleted: Storage.refresh()

    Text {
        topPadding: Theme.space.lg
        bottomPadding: Theme.space.xs
        text: qsTr("Usage")
        font: Theme.font.groupTitle
        color: Theme.colors.textTertiary
        Accessible.role: Accessible.Heading
    }

    // The meter: one rounded bar, segments in proportion to their share.
    Item {
        width: root.width
        height: Theme.space.sm + Theme.space.xxs
        Accessible.role: Accessible.ProgressBar
        //: %1 = total size of all caches, e.g. "1.2 GB"
        Accessible.name: qsTr("Caches use %1").arg(Format.bytes(Storage.totalBytes))
        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: Theme.colors.skeletonBase
        }
        // Segments are stacked pills that all start at the left edge, each as wide as the shares up to and
        // including it, so every visible end is rounded without clipping or layers.
        Item {
            id: bar
            anchors.fill: parent
            Repeater {
                id: segments
                model: Storage.segments
                Rectangle {
                    required property var modelData
                    required property int index
                    z: segments.count - index
                    visible: modelData.bytes > 0
                    // The share animates, not the width: resizing the window must not animate the meter.
                    property real share: root.cumulative(index) / root.barTotal
                    Behavior on share { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
                    width: Math.max(height, bar.width * share)
                    height: bar.height
                    radius: height / 2
                    color: root.shade(modelData.id)
                }
            }
        }
    }

    Flow {
        width: root.width
        spacing: Theme.space.lg
        topPadding: Theme.space.xs
        Repeater {
            model: Storage.segments
            Row {
                required property var modelData
                visible: modelData.bytes > 0
                spacing: Theme.space.xs
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Theme.space.sm
                    height: width
                    radius: width / 2
                    color: root.shade(modelData.id)
                }
                Text {
                    //: Legend entry of the cache usage meter: %1 = category, %2 = size
                    text: qsTr("%1 · %2").arg(root.label(modelData.id)).arg(Format.bytes(modelData.bytes))
                    font: Theme.font.caption
                    color: Theme.colors.textSecondary
                }
            }
        }
    }

    InfoRow {
        width: root.width
        name: qsTr("On disk")
        info: Storage.totalBytes === 0 ? qsTr("Nothing cached yet")
              //: %1 = size used, %2 = number of complete cached tracks
              : qsTr("%1 · %n complete track(s)", "", Storage.cachedTracks).arg(Format.bytes(Storage.totalBytes))
    }
    InfoRow {
        width: root.width
        name: qsTr("In memory")
        //: %1 = memory in use, %2 = memory budget
        info: qsTr("%1 of %2").arg(Format.bytes(Storage.memoryBytes)).arg(Format.bytes(Storage.memoryBudget))
    }
    InfoRow {
        width: root.width
        visible: Storage.freeBytes >= 0
        name: qsTr("Free space")
        info: Format.bytes(Storage.freeBytes)
    }
    InfoRow {
        width: root.width
        name: qsTr("Location")
        info: Storage.location.length > 0 ? Storage.location : qsTr("Memory only")
    }
    Repeater {
        model: [Storage.problem, Storage.diskFull ? qsTr("The disk space for tracks is used up by tracks that are playing, so new data is kept in memory only.") : ""]
        Row {
            required property string modelData
            visible: modelData.length > 0
            width: root.width
            topPadding: Theme.space.xs
            spacing: Theme.space.xs
            Icon {
                name: "triangle-alert"
                size: Theme.space.iconSizeSmall
                color: Theme.targetColors.warning
            }
            Text {
                width: parent.width - Theme.space.iconSizeSmall - parent.spacing
                text: parent.modelData
                font: Theme.font.caption
                color: Theme.colors.textSecondary
                wrapMode: Text.Wrap
            }
        }
    }

    Flow {
        width: root.width
        topPadding: Theme.space.sm
        spacing: Theme.space.sm
        Button {
            small: true
            iconName: "list-music"
            text: qsTr("Clear tracks")
            enabled: !Storage.clearing
            onClicked: root.ask("audio")
        }
        Button {
            small: true
            iconName: "layout-grid"
            text: qsTr("Clear images")
            enabled: !Storage.clearing
            onClicked: root.ask("images")
        }
        Button {
            small: true
            variant: "danger"
            iconName: "rotate-ccw"
            text: qsTr("Clear all")
            enabled: !Storage.clearing
            onClicked: root.ask("all")
        }
    }

    Dialog {
        id: confirm
        property string part
        destructive: true
        confirmText: qsTr("Remove")
        onAccepted: Storage.clear(part)
    }
}
