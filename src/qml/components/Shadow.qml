import QtQuick
import Noctara

// Soft drop shadow or coloured glow behind a rounded rectangle (our own SDF shader, :/shaders/shadow.frag).
// Fills its parent-provided geometry; the shader quad extends past it by the blur extent. GPU only: in the
// software renderer it is absent, like every shader effect. Stays loaded while hidden (no churn on hover).
Loader {
    id: root

    property real radius: 0
    property color color: "transparent"
    property real blur: 24
    property real spread: 0
    property real offsetY: 0

    active: Graphics.hardware
    // The Loader resizes its item to itself, so the (larger) shader quad lives inside a plain Item.
    sourceComponent: Item {
        ShaderEffect {
            readonly property real margin: Math.max(0, root.blur * 1.5 - root.spread) + Math.abs(root.offsetY)
            x: -margin
            y: -margin
            width: root.width + margin * 2
            height: root.height + margin * 2
            visible: root.color.a > 0.004
            property color color: root.color
            property vector2d itemSize: Qt.vector2d(width, height)
            property vector4d box: Qt.vector4d(margin - root.spread, margin - root.spread + root.offsetY,
                                               Math.max(1, root.width + root.spread * 2),
                                               Math.max(1, root.height + root.spread * 2))
            property real radius: Math.max(0, root.radius + root.spread)
            property real softness: root.blur
            fragmentShader: "qrc:/shaders/shadow.frag.qsb"
        }
    }
}
