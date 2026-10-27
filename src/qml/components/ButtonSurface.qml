import QtQuick
import Noctara

// Background of every button-like control. material:
//   frosted  milky glass veil with a sheen and light edge (default)
//   liquid   glossy glass: bright rim, specular highlight, light caught at the bottom edge
//   tinted   glass coloured by the accent
//   clear    outline only
//   solid    opaque surface
//   flat     invisible until hovered (toolbar/icon buttons)
//   primary  accent gradient with a glow (main actions)
Item {
    id: root

    property string material: Theme.glass.buttons
    property real radius: Math.min(Theme.radius.button, height / 2)
    property bool hovered: false
    property bool pressed: false
    property bool active: false
    property color accent: Theme.colors.accent

    readonly property bool primary: material === "primary"
    readonly property bool dark: Theme.dark

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function white(a) { return Qt.rgba(1, 1, 1, a) }

    readonly property color fillColor: {
        switch (material) {
        case "primary": return accent
        case "tinted": return alpha(accent, hovered ? 0.30 : 0.18)
        case "solid": return dark ? Qt.tint(Theme.colors.solidRaised, white(hovered ? 0.12 : 0.08))
                                  : Qt.tint(Theme.colors.solidRaised, Qt.rgba(0, 0, 0, hovered ? 0.06 : 0.03))
        case "clear":
        case "flat": return hovered || pressed ? Theme.colors.stateHover : "transparent"
        case "liquid": return dark ? white(hovered ? 0.09 : 0.05) : white(hovered ? 0.4 : 0.26)
        default: return dark ? white(hovered ? 0.16 : 0.11) : white(hovered ? 0.85 : 0.7)
        }
    }
    readonly property color edgeColor: {
        switch (material) {
        case "primary":
        case "flat": return "transparent"
        case "tinted": return alpha(accent, 0.5)
        case "solid": return Theme.colors.outline
        case "liquid": return white(dark ? 0.30 : 0.95)
        default: return active ? alpha(accent, 0.55) : Theme.colors.edge
        }
    }

    // Accent halo: always under primary actions, on hover/active for others (if enabled).
    Shadow {
        anchors.fill: parent
        z: -1
        radius: root.radius
        blur: 26 * Theme.scale
        spread: -4 * Theme.scale
        offsetY: root.primary ? 8 * Theme.scale : 0
        color: root.alpha(root.accent, !Theme.glass.glowControls ? 0
                                       : Theme.glass.glow * (root.primary ? 0.9 : (root.hovered || root.active ? 0.7 : 0)))
        Behavior on color { ColorAnimation { duration: Theme.motion.normal } }
    }

    Rectangle {
        id: base
        anchors.fill: parent
        radius: root.radius
        color: root.fillColor
        border.width: Theme.space.hairline
        border.color: root.edgeColor
        gradient: root.primary ? primaryGradient : null
        Behavior on color { ColorAnimation { duration: Theme.motion.fast } }
    }
    Gradient {
        id: primaryGradient
        GradientStop { position: 0.0; color: Qt.tint(root.accent, Qt.rgba(1, 1, 1, root.hovered ? 0.28 : 0.18)) }
        GradientStop { position: 0.5; color: root.accent }
        GradientStop { position: 1.0; color: Qt.tint(root.accent, root.alpha(Theme.colors.accentLight, 0.7)) }
    }

    // Sheen falling from the top (frosted, liquid, primary).
    Rectangle {
        anchors.fill: parent
        anchors.margins: Theme.space.hairline
        radius: root.radius
        visible: root.material === "frosted" || root.material === "liquid" || root.primary
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: root.white(root.material === "liquid" ? (root.dark ? 0.26 : 0.7) : (root.primary ? 0.28 : 0.08))
            }
            GradientStop { position: root.material === "liquid" ? 0.45 : 0.4; color: "transparent" }
            // Liquid glass catches light at its lower edge, as a thick lens does.
            GradientStop {
                position: 1.0
                color: root.material !== "liquid" ? "transparent"
                     : (root.dark ? root.alpha(Theme.colors.accentLight, 0.16) : root.white(0.6))
            }
        }
    }
    // Specular highlight (liquid only): a soft glint near the top-left, where the light comes from.
    Rectangle {
        visible: root.material === "liquid"
        x: root.width * 0.14
        y: root.height * 0.1
        width: Math.max(4, root.width * 0.42)
        height: Math.max(2, root.height * 0.22)
        radius: height / 2
        gradient: Gradient {
            GradientStop { position: 0.0; color: root.white(root.dark ? 0.38 : 0.85) }
            GradientStop { position: 1.0; color: "transparent" }
        }
    }
    // Inner highlight under the top edge (glass materials).
    Rectangle {
        visible: root.material === "frosted" || root.material === "liquid" || root.primary
        x: Math.min(root.radius, root.width / 2) * 0.8
        y: Theme.space.hairline
        width: Math.max(0, root.width - x * 2)
        height: Theme.space.hairline
        color: root.white(root.primary ? 0.5 : (root.material === "liquid" ? 0.45 : 0.16))
    }
    // Liquid glass: the rim darkens toward the lower edge, as light bends through a thick lens.
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        visible: root.material === "liquid"
        color: "transparent"
        gradient: Gradient {
            GradientStop { position: 0.55; color: "transparent" }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, root.dark ? 0.2 : 0.1) }
        }
    }
    // Pressed state darkens slightly from the bottom (inner shadow).
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        opacity: root.pressed ? 1 : 0
        gradient: Gradient {
            GradientStop { position: 0.4; color: "transparent" }
            GradientStop { position: 1.0; color: Qt.rgba(0, 0, 0, root.dark ? 0.22 : 0.08) }
        }
        Behavior on opacity { NumberAnimation { duration: Theme.motion.fast } }
    }
}
