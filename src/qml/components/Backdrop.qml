import QtQuick
import QtQuick.Shapes
import Noctara

// Window background: deep tinted gradient (Midnight) or pale cool wash (Frost). The blurred cover-art
// layer and its drift animation are added by the backdrop pipeline on the GPU path.
Item {
    id: root

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0.0; color: Theme.colors.backdropTop }
            GradientStop { position: 0.55; color: Theme.colors.backdrop }
            GradientStop { position: 1.0; color: Theme.colors.backdropBottom }
        }
    }
    // Soft accent bloom from the upper-left, matching the light direction of the glass rims.
    Shape {
        anchors.fill: parent
        opacity: Theme.dark ? 0.4 : 0.6
        ShapePath {
            strokeWidth: -1
            fillGradient: RadialGradient {
                centerX: root.width * 0.12
                centerY: root.height * 0.02
                centerRadius: Math.max(root.width, root.height) * 0.65
                focalX: centerX
                focalY: centerY
                GradientStop { position: 0.0; color: Theme.colors.glow }
                GradientStop { position: 1.0; color: "transparent" }
            }
            PathRectangle { x: 0; y: 0; width: root.width; height: root.height }
        }
    }
}
