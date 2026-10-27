import QtQuick
import Noctara

// Loading placeholder with a single shimmering highlight band (no spinners). Static under reduce motion.
Rectangle {
    id: root

    radius: Theme.radius.sm
    color: Theme.colors.skeletonBase
    clip: true
    Accessible.role: Accessible.Animation
    Accessible.name: qsTr("Loading")

    Rectangle {
        id: band
        width: root.width * 0.6
        height: root.height
        visible: Theme.motion.shimmer > 0
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
