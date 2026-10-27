import QtQuick
import Noctara

// Floating surface. level: 0 base, 1 raised, 2 overlay (see the elevation tokens). The translucent tint
// sits over the shared backdrop; Solid style and reduce-transparency use opaque surfaces instead.
// The blurred-backdrop sampling (GPU path) is layered in by the glass pipeline.
Item {
    id: root

    property int level: 0
    property real radius: Theme.radius.md
    property bool outlined: true
    default property alias content: contentItem.data

    readonly property color tintColor: level === 2 ? Theme.colors.surfaceOverlay
                                     : level === 1 ? Theme.colors.surfaceRaised : Theme.colors.surfaceBase
    readonly property color solidColor: level === 2 ? Theme.colors.solidOverlay
                                      : level === 1 ? Theme.colors.solidRaised : Theme.colors.solidBase

    Rectangle {
        id: fill
        anchors.fill: parent
        radius: root.radius
        color: Theme.glass.translucent ? root.tintColor : root.solidColor
    }
    // Hairline rim. The directional rim (bright top-left, darker bottom-right) is drawn by the glass
    // shader on the GPU path; the software/solid path keeps a uniform, cheap outline.
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        visible: root.outlined
        color: "transparent"
        border.width: Theme.space.hairline
        border.color: Theme.dark ? Theme.colors.rimHighlight : Theme.colors.rimShadow
    }
    Item {
        id: contentItem
        anchors.fill: parent
    }
}
