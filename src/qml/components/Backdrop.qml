import QtQuick
import QtQuick.Shapes
import QtQuick.Window
import Noctara

// Window background. Styles (Theme.glass.backdrop):
//   aurora  soft pools of light in the palette colours over a near-black (Midnight) or pale (Frost) base
//   cover   the artwork, pre-blurred and tone-mapped off the GUI thread (AccentEngine); falls back to aurora
//           until a track with artwork is playing. Crossfades on track change.
//   dark    an almost plain base with a faint shade of the palette
// Light pools are radial gradients (no blur pass). Their strength follows "Glow intensity"; the slow drift
// runs only while the window is active, animations are allowed and the GPU path is in use.
Item {
    id: root

    readonly property real unit: Math.max(width / 1520, height / 960)
    readonly property bool cover: Theme.glass.backdrop === "cover" && Artwork.hasArtwork
    readonly property bool aurora: Theme.glass.backdrop !== "dark" && !cover
    readonly property real strength: 0.35 + Theme.glass.glow * 0.65
    readonly property bool animate: Theme.glass.backdropAnimated && Window.window !== null && Window.window.active
                                    && Window.window.visibility !== Window.Minimized

    function tone(c, a) { return Qt.rgba(c.r, c.g, c.b, a) }
    // 0 -> amplitude -> 0 over twice `halfPeriod` ms, eased like InOutSine.
    function wave(amplitude, halfPeriod) { return amplitude * (1 - Math.cos(Math.PI * clock.elapsed / halfPeriod)) / 2 }

    // The drift moves a few pixels per second, so 12 updates per second look exactly like display-rate
    // animation while redrawing (and re-blurring) the window an order of magnitude less often. A declarative
    // NumberAnimation would tick at the display refresh rate (up to 175 Hz here) for an imperceptible motion.
    Timer {
        id: clock
        property real elapsed: 0
        interval: 83
        repeat: true
        running: root.animate && (root.aurora || root.cover)
        onTriggered: elapsed += interval
    }

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
            driftX: root.wave(120 * root.unit, 28000)
            driftY: root.wave(90 * root.unit, 28000)
        }
        Pool {
            width: 1000 * root.unit
            height: width
            baseX: root.width - 640 * root.unit
            baseY: root.height * 0.16 - 300 * root.unit
            tone: Theme.colors.accentLight
            alpha: (Theme.dark ? 0.46 : 0.18) * root.strength
            driftX: root.wave(-110 * root.unit, 34000)
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
            driftY: root.wave(-70 * root.unit, 22000)
        }
    }

    // Cover art: two layers so a new artwork fades in over the previous one.
    Item {
        id: artwork
        anchors.fill: parent
        visible: root.cover
        readonly property string url: Artwork.hasArtwork
                                      ? "image://artwork/" + (Theme.dark ? "dark" : "light") + "/" + Artwork.revision : ""
        property bool front: false
        onUrlChanged: {
            front = !front
            ;(front ? coverA : coverB).source = url
        }
        Component.onCompleted: (front ? coverA : coverB).source = url

        component Layer: Image {
            anchors.fill: parent
            fillMode: Image.PreserveAspectCrop
            smooth: true
            cache: false
            asynchronous: true
            Behavior on opacity { NumberAnimation { duration: Theme.motion.accent } }
            // Very slow drift, only while animation is allowed (the same conditions as the aurora).
            transform: Scale {
                origin.x: artwork.width / 2
                origin.y: artwork.height / 2
                xScale: 1.12 + artwork.drift
                yScale: xScale
            }
        }
        readonly property real drift: root.wave(0.06, 30000)
        Layer { id: coverA; opacity: artwork.front ? 1 : 0 }
        Layer { id: coverB; opacity: artwork.front ? 0 : 1 }
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

    // Fine grain breaks up banding in the gradients. It lives in the backdrop, so text and controls stay clean;
    // glass panels add their own, fainter grain in the glass shader. Static texture: no per-frame cost.
    Image {
        anchors.fill: parent
        source: "image://noise/128"
        fillMode: Image.Tile
        smooth: false
        opacity: Theme.glass.grain * (Theme.dark ? 0.16 : 0.1)
        visible: opacity > 0.002
        Accessible.ignored: true
    }
}
