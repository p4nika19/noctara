import QtQuick
import Noctara

// Background of every button-like control. material:
//   frosted  milky glass veil with a sheen and light edge (default)
//   liquid   glossy glass: bright rim, specular highlight, light caught at the bottom edge
//   tinted   glass coloured by the accent
//   plain    flat translucent fill and a hairline, no sheen or light edge (goes with flat surfaces)
//   clear    outline only
//   solid    opaque surface
//   flat     invisible until hovered (toolbar/icon buttons)
//   primary  main actions (play, primary buttons), drawn as the "Main buttons" setting says: a flat accent fill,
//            the button material with an accent edge, an accent outline, or the glossy accent gradient
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
    // The look actually drawn: a main action "like other buttons" uses the button material.
    readonly property string drawn: !primary ? material
                                   : Theme.glass.primary === "material" ? Theme.glass.buttons : Theme.glass.primary
    readonly property bool accentFill: drawn === "filled" || drawn === "gradient"
    readonly property bool glassy: drawn === "frosted" || drawn === "liquid" || drawn === "gradient"

    function alpha(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    function white(a) { return Qt.rgba(1, 1, 1, a) }

    readonly property color fillColor: {
        switch (drawn) {
        case "filled": return hovered ? Qt.tint(accent, white(dark ? 0.12 : 0.1)) : accent
        case "gradient": return accent
        case "outline": return alpha(accent, hovered || pressed ? 0.14 : 0.04)
        case "tinted": return alpha(accent, hovered ? 0.30 : 0.18)
        case "plain": return dark ? white(hovered ? 0.12 : 0.07) : Qt.rgba(0, 0, 0, hovered ? 0.08 : 0.045)
        case "solid": return dark ? Qt.tint(Theme.colors.solidRaised, white(hovered ? 0.12 : 0.08))
                                  : Qt.tint(Theme.colors.solidRaised, Qt.rgba(0, 0, 0, hovered ? 0.06 : 0.03))
        case "clear":
        case "flat": return hovered || pressed ? Theme.colors.stateHover : "transparent"
        case "liquid": return dark ? white(hovered ? 0.09 : 0.05) : white(hovered ? 0.4 : 0.26)
        default: return dark ? white(hovered ? 0.16 : 0.11) : white(hovered ? 0.85 : 0.7)
        }
    }
    readonly property color edgeColor: {
        if (primary && !accentFill)
            return alpha(accent, drawn === "outline" ? 1 : 0.7)
        switch (drawn) {
        case "filled":
        case "gradient":
        case "flat": return "transparent"
        case "tinted": return alpha(accent, 0.5)
        case "solid": return Theme.colors.outline
        case "plain": return active ? alpha(accent, 0.55) : Theme.colors.divider
        case "liquid": return white(dark ? 0.30 : 0.95)
        default: return active ? alpha(accent, 0.55) : Theme.colors.edge
        }
    }

    // Accent halo: always under primary actions; on hover or when active for glass materials; only when
    // active for flat buttons (a hover halo there looks exactly like a selection).
    Shadow {
        anchors.fill: parent
        z: -1
        radius: root.radius
        blur: 26 * Theme.scale
        spread: -4 * Theme.scale
        offsetY: root.accentFill ? 6 * Theme.scale : 0
        // Flat surfaces and outlines keep the halo for hover only: a constant glow would read as a shadow.
        property real strength: !Theme.glass.glowControls ? 0
                                : Theme.glass.glow * (root.accentFill ? (Theme.glass.flat ? 0.55 : 0.85)
                                                      : root.primary ? (root.hovered ? 0.7 : 0.35)
                                                      : root.active || (root.hovered && root.material !== "flat") ? 0.7 : 0)
        // Only the strength animates; the hue follows the accent directly. Animating the colour restarted the
        // animation of every button on each frame of an accent crossfade.
        Behavior on strength { NumberAnimation { duration: Theme.motion.normal } }
        color: root.alpha(root.accent, strength)
    }

    Rectangle {
        id: base
        anchors.fill: parent
        radius: root.radius
        color: root.fillColor
        border.width: root.drawn === "outline" ? 1.5 * Theme.scale : Theme.space.hairline
        border.color: root.edgeColor
        gradient: root.drawn === "gradient" ? primaryGradient : null
        Behavior on color { ColorAnimation { duration: Theme.motion.fast } }
    }
    Gradient {
        id: primaryGradient
        GradientStop { position: 0.0; color: Qt.tint(root.accent, Qt.rgba(1, 1, 1, root.hovered ? 0.28 : 0.18)) }
        GradientStop { position: 0.5; color: root.accent }
        GradientStop { position: 1.0; color: Qt.tint(root.accent, root.alpha(Theme.colors.accentLight, 0.7)) }
    }

    // Sheen falling from the top (frosted, liquid, gradient).
    Rectangle {
        anchors.fill: parent
        anchors.margins: Theme.space.hairline
        radius: Math.max(0, root.radius - Theme.space.hairline)
        visible: root.glassy
        gradient: Gradient {
            GradientStop {
                position: 0.0
                color: root.white(root.drawn === "liquid" ? (root.dark ? 0.26 : 0.7) : (root.drawn === "gradient" ? 0.28 : 0.08))
            }
            GradientStop { position: root.drawn === "liquid" ? 0.45 : 0.4; color: "transparent" }
            // Liquid glass catches light at its lower edge, as a thick lens does.
            GradientStop {
                position: 1.0
                color: root.drawn !== "liquid" ? "transparent"
                     : (root.dark ? root.alpha(Theme.colors.accentLight, 0.16) : root.white(0.6))
            }
        }
    }
    // The top of the edge catches the light, following the rounded ends (glass materials).
    RimLight {
        anchors.fill: parent
        radius: root.radius
        visible: root.glassy
        color: root.white(root.drawn === "gradient" ? 0.45 : (root.drawn === "liquid" ? (root.dark ? 0.4 : 0.9) : (root.dark ? 0.16 : 0.8)))
    }
    // Liquid glass: the rim darkens toward the lower edge, as light bends through a thick lens.
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        visible: root.drawn === "liquid"
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
