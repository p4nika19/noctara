import QtQuick
import Noctara

// Tinted stroke icon. Rasterised once per (name, colour, stroke, size) by the icon provider; when the
// colour changes, the new rasterisation fades in over the old one instead of re-rendering every frame.
Item {
    id: root

    property string name
    property color color: Theme.targetColors.iconPrimary
    property real size: Theme.space.iconSize

    readonly property string url: name.length > 0
        ? "image://icon/" + name + "?c=" + encodeURIComponent(color.toString()) + "&s=" + Theme.iconStroke
        : ""
    property int current: 0

    implicitWidth: size
    implicitHeight: size
    Accessible.ignored: true

    onUrlChanged: {
        if (current === 0)
            second.source = url
        else
            first.source = url
    }
    Component.onCompleted: first.source = url

    component Layer: Image {
        required property int index
        anchors.fill: parent
        sourceSize: Qt.size(root.size, root.size)
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        opacity: root.current === index ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motion.normal } }
        onStatusChanged: {
            if (status === Image.Ready && source.toString() === root.url)
                root.current = index
        }
    }

    Layer { id: first; index: 0 }
    Layer { id: second; index: 1 }
}
