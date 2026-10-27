import QtQuick
import QtQuick.Shapes
import QtQuick.Window
import Noctara

// Window background. Styles (Theme.glass.backdrop):
//   aurora  soft pools of light in the palette colours over a near-black (Midnight) or pale (Frost) base
//   cover   the blurred artwork; falls back to aurora until a track with artwork is playing
//   dark    an almost plain base with a faint shade of the palette
// Light pools are radial gradients (no blur pass). Their strength follows "Glow intensity"; the slow drift
// runs only while the window is active, animations are allowed and the GPU path is in use.
Item {
    id: root

    readonly property real unit: Math.max(width / 1520, height / 960)
    readonly property bool aurora: Theme.glass.backdrop !== "dark"
    readonly property real strength: 0.35 + Theme.glass.glow * 0.65
    readonly property bool animate: Theme.glass.backdropAnimated && Window.window !== null && Window.window.active
                                    && Window.window.visibility !== Window.Minimized

    function tone(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }

    component Pool: Item {
        id: pool
        property color tone
        property real alpha
        property real baseX
        property real baseY
        property real driftX: 0
        property real driftY: 0
        property real stretch: 1
        x: baseX + driftX
        y: baseY + driftY
        transform: Scale { yScale: pool.stretch; origin.y: pool.height / 2 }
        Shape {
            anchors.fill: parent
            ShapePath {
                strokeWidth: -1
                fillGradient: RadialGradient {
                    centerX: pool.width / 2
                    centerY: pool.height / 2
                    centerRadius: pool.width / 2
                    focalX: centerX
                    focalY: centerY
                    GradientStop { position: 0.0; color: root.tone(pool.tone, pool.alpha) }
                    GradientStop { position: 0.5; color: root.tone(pool.tone, pool.alpha * 0.45) }
                    GradientStop { position: 1.0; color: root.tone(pool.tone, 0) }
                }
                PathRectangle { x: 0; y: 0; width: pool.width; height: pool.height }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.colors.backdrop
    }

    Item {
        anchors.fill: parent
        visible: root.aurora

        Pool {
            id: first
            width: 900 * root.unit
            height: width
            baseX: -340 * root.unit
            baseY: -420 * root.unit
            tone: Theme.colors.accent
            alpha: (Theme.dark ? 0.6 : 0.22) * root.strength
            SequentialAnimation on driftX {
                running: root.animate
                loops: Animation.Infinite
                NumberAnimation { to: 120 * root.unit; duration: 28000; easing.type: Easing.InOutSine }
                NumberAnimation { to: 0; duration: 28000; easing.type: Easing.InOutSine }
            }
            SequentialAnimation on driftY {
                running: root.animate
                loops: Animation.Infinite
                NumberAnimation { to: 90 * root.unit; duration: 28000; easing.type: Easing.InOutSine }
                NumberAnimation { to: 0; duration: 28000; easing.type: Easing.InOutSine }
            }
        }
        Pool {
            width: 1000 * root.unit
            height: width
            baseX: root.width - 640 * root.unit
            baseY: root.height * 0.16 - 300 * root.unit
            tone: Theme.colors.accentLight
            alpha: (Theme.dark ? 0.46 : 0.18) * root.strength
            SequentialAnimation on driftX {
                running: root.animate
                loops: Animation.Infinite
                NumberAnimation { to: -110 * root.unit; duration: 34000; easing.type: Easing.InOutSine }
                NumberAnimation { to: 0; duration: 34000; easing.type: Easing.InOutSine }
            }
        }
        Pool {
            width: 1300 * root.unit
            height: width
            stretch: 0.65
            baseX: root.width * 0.2 - 160 * root.unit
            baseY: root.height - 560 * root.unit
            tone: Theme.colors.accentDeep
            alpha: Theme.dark ? 0.9 : 0.8
        }
        Pool {
            width: 620 * root.unit
            height: width
            baseX: root.width * 0.34 - 100 * root.unit
            baseY: root.height * 0.2 - 100 * root.unit
            tone: Theme.colors.accent
            alpha: (Theme.dark ? 0.2 : 0.08) * root.strength
            SequentialAnimation on driftY {
                running: root.animate
                loops: Animation.Infinite
                NumberAnimation { to: -70 * root.unit; duration: 22000; easing.type: Easing.InOutSine }
                NumberAnimation { to: 0; duration: 22000; easing.type: Easing.InOutSine }
            }
        }
    }

    // Dark style: a faint shade of the palette from the top-left.
    Shape {
        anchors.fill: parent
        visible: !root.aurora
        ShapePath {
            strokeWidth: -1
            fillGradient: RadialGradient {
                centerX: root.width * 0.2
                centerY: 0
                centerRadius: Math.max(root.width, root.height) * 0.8
                focalX: centerX
                focalY: centerY
                GradientStop { position: 0.0; color: root.tone(Theme.colors.accentDeep, Theme.dark ? 0.8 : 0.6) }
                GradientStop { position: 1.0; color: root.tone(Theme.colors.accentDeep, 0) }
            }
            PathRectangle { x: 0; y: 0; width: root.width; height: root.height }
        }
    }

    // Vignette: edges recede so the centre and the glass carry the light.
    Shape {
        anchors.fill: parent
        ShapePath {
            strokeWidth: -1
            fillGradient: RadialGradient {
                centerX: root.width / 2
                centerY: 0
                centerRadius: Math.max(root.width, root.height) * 1.15
                focalX: centerX
                focalY: centerY
                GradientStop { position: 0.3; color: "transparent" }
                GradientStop { position: 1.0; color: Theme.dark ? Qt.rgba(0.012, 0.016, 0.04, 0.6) : Qt.rgba(1, 1, 1, 0.35) }
            }
            PathRectangle { x: 0; y: 0; width: root.width; height: root.height }
        }
    }
}
