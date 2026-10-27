import QtQuick
import QtQuick.Shapes
import Noctara

// Light caught by the top of a rounded rectangle's edge, as from a light above. It is stroked exactly on the
// 1 px edge and follows the rounded corners, fading out down the sides, so it reads as the edge being lit
// rather than as a separate line. (A straight inner line stopped short of the corners and doubled the edge.)
Shape {
    id: root

    property real radius: 0
    property color color: Theme.colors.rimHighlight
    // How far down the sides the light reaches before it has faded completely.
    property real reach: Math.min(height * 0.6, Math.max(radius * 1.6, 28 * Theme.scale))

    readonly property real stroke: Theme.space.hairline

    preferredRendererType: Shape.CurveRenderer  // anti-aliased curves
    enabled: false
    Accessible.ignored: true

    ShapePath {
        strokeWidth: root.stroke
        fillColor: "transparent"
        strokeGradient: LinearGradient {
            x1: 0
            y1: 0
            x2: 0
            y2: root.reach
            GradientStop { position: 0.0; color: root.color }
            GradientStop { position: 1.0; color: Qt.rgba(root.color.r, root.color.g, root.color.b, 0) }
        }
        PathRectangle {
            x: root.stroke / 2
            y: root.stroke / 2
            width: Math.max(0, root.width - root.stroke)
            height: Math.max(0, root.height - root.stroke)
            radius: Math.max(0, Math.min(root.radius, root.width / 2, root.height / 2) - root.stroke / 2)
        }
    }
}
