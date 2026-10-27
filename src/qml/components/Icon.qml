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
        const target = current === 0 ? second : first
        target.source = url
        // Returning to a colour the hidden layer already holds changes nothing about that layer, so no status
        // signal follows: switch to it directly, or the icon would keep showing the previous colour.
        if (target.status === Image.Ready && target.source.toString() === url)
            current = current === 0 ? 1 : 0
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
