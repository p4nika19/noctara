import QtQuick
import Noctara

// Floating glass surface. level: 0 base, 1 raised, 2 overlay (drawers, popups). Translucent styles show a
// light veil with a sheen falling from the top, a 1px light edge and an inner highlight under the top edge;
// Solid style and reduce-transparency use opaque surfaces. Shadows exist on the GPU path only.
Item {
    id: root

    property int level: 0
    property real radius: Theme.radius.lg
    property bool shadowed: true
    default property alias content: contentItem.data

    readonly property bool translucent: Theme.glass.translucent
    readonly property color fillColor: translucent
        ? (level === 2 ? Theme.colors.surfaceOverlay : level === 1 ? Theme.colors.surfaceRaised : Theme.colors.surfaceBase)
        : (level === 2 ? Theme.colors.solidOverlay : level === 1 ? Theme.colors.solidRaised : Theme.colors.solidBase)

    Shadow {
        anchors.fill: parent
        z: -1
        active: root.shadowed && Graphics.hardware
        radius: root.radius
        color: Theme.colors.shadow
        blur: (root.level === 2 ? 72 : 48) * Theme.scale
        spread: -22 * Theme.scale
        offsetY: (root.level === 2 ? 28 : 22) * Theme.scale
    }
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.fillColor
        border.width: Theme.space.hairline
        border.color: Theme.colors.edge
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: Theme.space.hairline
        radius: root.radius
        visible: root.translucent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.colors.sheen }
            GradientStop { position: 0.38; color: "transparent" }
        }
    }
    Rectangle {
        x: root.radius * 0.7
        y: Theme.space.hairline
        width: root.width - x * 2
        height: Theme.space.hairline
        visible: root.translucent
        color: Theme.colors.rimHighlight
    }
    Item {
        id: contentItem
        anchors.fill: parent
    }
}
