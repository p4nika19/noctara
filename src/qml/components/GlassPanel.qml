import QtQuick
import QtQuick.Window
import Noctara

// Floating glass surface. level: 0 base, 1 raised, 2 overlay (drawers, popups).
//
// Blur styles sample the window's shared blurred backdrop (BackdropBlur.qml) exactly behind the panel, cut to
// the rounded rectangle, with a saturation/brightness lift and faint grain; the veil, sheen and the light
// edge (lit at the top, RimLight.qml) are drawn on top. Liquid Glass is opt-in per surface (`liquid`): only a few prominent
// surfaces (navigation rail, drawers, popovers, toasts, the player bar) bend light, everything else uses the
// frosted look over the same blurred texture. The flat surface style keeps only the blurred backdrop, the tint
// and a hairline: no lift, sheen, light edge or shadow. The blur ("style") and the surface look are independent
// settings. No blur, reduce transparency and the software renderer use opaque surfaces. Shadows exist on the
// GPU path only.
Item {
    id: root

    property int level: 0
    property real radius: Theme.radius.lg
    property bool shadowed: true
    // Use the Liquid Glass shader when that style is active. Panels only: list rows and other repeated items
    // are not GlassPanels, so the lens effect never multiplies across a list.
    property bool liquid: true
    default property alias content: contentItem.data
    // Smallest margin that keeps a content corner inside the rounded corner: r * (1 - 1/sqrt(2)). Content near
    // the edges uses max(its usual margin, cornerInset), so large corner radii never clip or crowd it.
    readonly property real cornerInset: Math.min(radius, width / 2, height / 2) * (1 - Math.SQRT1_2)

    readonly property bool translucent: Theme.glass.translucent
    readonly property Item blurSource: Theme.glass.blurred && Window.window && Window.window.backdropTexture
                                       ? Window.window.backdropTexture : null
    readonly property bool liquidActive: liquid && Theme.glass.surface === "liquid"
    readonly property bool flat: Theme.glass.flat
    readonly property color fillColor: translucent
        ? (level === 2 ? Theme.colors.surfaceOverlay : level === 1 ? Theme.colors.surfaceRaised : Theme.colors.surfaceBase)
        : (level === 2 ? Theme.colors.solidOverlay : level === 1 ? Theme.colors.solidRaised : Theme.colors.solidBase)

    Shadow {
        anchors.fill: parent
        z: -1
        active: root.shadowed && Graphics.hardware && !root.flat
        radius: root.radius
        color: Theme.colors.shadow
        blur: (root.level === 2 ? 72 : 48) * Theme.scale
        spread: -22 * Theme.scale
        offsetY: (root.level === 2 ? 28 : 22) * Theme.scale
    }
    Loader {
        anchors.fill: parent
        active: root.blurSource !== null
        sourceComponent: ShaderEffect {
            readonly property real lightAngle: Theme.glass.lightAngle * Math.PI / 180
            property Item source: root.blurSource
            property real ndcYSign: Graphics.ndcYUp ? -1 : 1
            property vector2d itemSize: Qt.vector2d(width, height)
            property vector2d windowSize: Qt.vector2d(Window.width, Window.height)
            property real radius: root.radius
            property real saturation: root.flat ? 1 : 1 + Theme.glass.saturationLift
            property real brightness: root.flat ? 0 : Theme.glass.brightnessLift
            property real refraction: Theme.glass.refraction * Theme.scale
            property real thickness: Theme.glass.thickness * Theme.scale
            property real dispersion: Theme.glass.dispersion
            property real specular: Theme.glass.specular
            property real fresnel: Theme.glass.fresnel
            property real rimDarkening: Theme.glass.rimDarkening
            // Light from the top-left: angle measured from +x, counter-clockwise, y pointing down on screen.
            property vector2d lightDir: Qt.vector2d(Math.cos(lightAngle), -Math.sin(lightAngle))
            property vector4d rimTint: Theme.dark
                ? Qt.vector4d(Theme.colors.accent.r * 0.35, Theme.colors.accent.g * 0.35, Theme.colors.accent.b * 0.35, 0)
                : Qt.vector4d(0.25, 0.25, 0.25, 0)
            property real grain: Theme.glass.grain * 0.025
            vertexShader: "qrc:/shaders/glass.vert.qsb"
            fragmentShader: root.liquidActive ? "qrc:/shaders/liquid.frag.qsb" : "qrc:/shaders/glass.frag.qsb"
        }
    }
    Rectangle {
        anchors.fill: parent
        radius: root.radius
        color: root.fillColor
        border.width: Theme.space.hairline
        border.color: root.flat ? Theme.colors.divider : Theme.colors.edge
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: Theme.space.hairline
        radius: Math.max(0, root.radius - Theme.space.hairline)
        visible: root.translucent && !root.liquidActive && !root.flat
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.colors.sheen }
            GradientStop { position: 0.38; color: "transparent" }
        }
    }
    // The top of the edge catches the light, following the rounded corners (Liquid Glass lights its own rim).
    RimLight {
        anchors.fill: parent
        radius: root.radius
        visible: root.translucent && !root.liquidActive && !root.flat
    }
    Item {
        id: contentItem
        anchors.fill: parent
    }
}
