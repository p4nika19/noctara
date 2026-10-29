import QtQuick
import QtQuick.Window

// Invisible resize handles along a frameless window's edges and corners (the system does the resizing).
Item {
    id: root

    readonly property real grip: 6

    component Edge: MouseArea {
        property int edges: 0
        hoverEnabled: true
        onPressed: Window.window.startSystemResize(edges)
    }
    Edge { anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom; width: root.grip
           edges: Qt.LeftEdge; cursorShape: Qt.SizeHorCursor }
    Edge { anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom; width: root.grip
           edges: Qt.RightEdge; cursorShape: Qt.SizeHorCursor }
    Edge { anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; height: root.grip
           edges: Qt.TopEdge; cursorShape: Qt.SizeVerCursor }
    Edge { anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom; height: root.grip
           edges: Qt.BottomEdge; cursorShape: Qt.SizeVerCursor }
    Edge { anchors.left: parent.left; anchors.top: parent.top; width: root.grip * 2; height: width
           edges: Qt.LeftEdge | Qt.TopEdge; cursorShape: Qt.SizeFDiagCursor }
    Edge { anchors.right: parent.right; anchors.bottom: parent.bottom; width: root.grip * 2; height: width
           edges: Qt.RightEdge | Qt.BottomEdge; cursorShape: Qt.SizeFDiagCursor }
    Edge { anchors.right: parent.right; anchors.top: parent.top; width: root.grip * 2; height: width
           edges: Qt.RightEdge | Qt.TopEdge; cursorShape: Qt.SizeBDiagCursor }
    Edge { anchors.left: parent.left; anchors.bottom: parent.bottom; width: root.grip * 2; height: width
           edges: Qt.LeftEdge | Qt.BottomEdge; cursorShape: Qt.SizeBDiagCursor }
}
