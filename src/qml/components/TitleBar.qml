import QtQuick
import QtQuick.Window
import Noctara

// Noctara's own window frame (Layout → Window frame → Noctara): a draggable strip with the title and the
// minimize, maximize and close buttons in the glass style, plus resize edges. The system still moves and resizes
// the window (startSystemMove/Resize), so snapping and tiling keep working.
Item {
    id: root

    readonly property Window window: Window.window
    readonly property bool maximized: window && (window.visibility === Window.Maximized
                                                 || window.visibility === Window.FullScreen)

    implicitHeight: Theme.space.controlHeight + Theme.space.sm

    DragHandler {
        target: null
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        onActiveChanged: if (active) root.window.startSystemMove()
    }
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onDoubleTapped: root.window.visibility = root.maximized ? Window.Windowed : Window.Maximized
    }
    Text {
        anchors.centerIn: parent
        text: root.window ? root.window.title : ""
        font: Theme.font.label
        color: Theme.colors.textTertiary
    }
    Row {
        anchors.right: parent.right
        anchors.rightMargin: Theme.space.sm
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.xs
        IconButton {
            width: Theme.space.controlHeightSmall
            height: width
            iconName: "minus"
            iconSize: Theme.space.iconSizeSmall
            text: qsTr("Minimize")
            onClicked: root.window.showMinimized()
        }
        IconButton {
            width: Theme.space.controlHeightSmall
            height: width
            iconName: root.maximized ? "minimize-2" : "maximize-2"
            iconSize: Theme.space.iconSizeSmall
            text: root.maximized ? qsTr("Restore") : qsTr("Maximize")
            onClicked: root.window.visibility = root.maximized ? Window.Windowed : Window.Maximized
        }
        IconButton {
            width: Theme.space.controlHeightSmall
            height: width
            iconName: "x"
            iconSize: Theme.space.iconSizeSmall
            text: qsTr("Close")
            onClicked: root.window.close()
        }
    }
}
