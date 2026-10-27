import QtQuick
import Noctara

// Placeholder with a single shimmering highlight band (no spinners). The band only moves while `loading` is
// set: a placeholder for content that is not on its way (no account yet) stays still and costs no frames.
// Static under reduce motion.
Rectangle {
    id: root

    property bool loading: false

    radius: Theme.radius.sm
    color: Theme.colors.skeletonBase
    clip: true
    Accessible.role: loading ? Accessible.Animation : Accessible.Graphic
    Accessible.name: loading ? qsTr("Loading") : ""

    Rectangle {
        id: band
        width: root.width * 0.6
        height: root.height
        visible: root.loading && Theme.motion.shimmer > 0
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0.0; color: "transparent" }
            GradientStop { position: 0.5; color: Theme.colors.skeletonHighlight }
            GradientStop { position: 1.0; color: "transparent" }
        }
        NumberAnimation on x {
            from: -band.width
            to: root.width
            duration: Math.max(1, Theme.motion.shimmer)
            loops: Animation.Infinite
            running: band.visible && root.visible
        }
    }
}
