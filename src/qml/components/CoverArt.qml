import QtQuick
import QtQuick.Window
import Noctara

// Cover art or an avatar from the image cache, decoded off the GUI thread at the displayed size × DPR. The
// corners are rounded by the image provider (CPU), so the shape is the same with and without shader effects.
// A still skeleton shows until the image is ready; it then fades in.
Item {
    id: root

    property url source
    property string shape: Theme.coverShape  // "rounded" | "square" | "circle"
    property real radius: Theme.radius.sm

    readonly property real cornerRadius: shape === "circle" ? Math.min(width, height) / 2
                                       : shape === "square" ? 0 : Math.min(radius, Math.min(width, height) / 2)
    readonly property real maskFraction: cornerRadius / Math.max(1, Math.min(width, height))

    Accessible.ignored: true

    Skeleton {
        anchors.fill: parent
        radius: root.cornerRadius
        visible: image.status !== Image.Ready || image.opacity < 1
    }

    Image {
        id: image
        anchors.fill: parent
        asynchronous: true
        fillMode: Image.PreserveAspectCrop
        sourceSize: Qt.size(Math.ceil(root.width * Screen.devicePixelRatio),
                            Math.ceil(root.height * Screen.devicePixelRatio))
        source: root.source.toString().length > 0 && root.width > 0
                ? "image://cover/mask/" + root.maskFraction.toFixed(3) + "/" + encodeURIComponent(root.source.toString())
                : ""
        opacity: status === Image.Ready ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
    }
}
