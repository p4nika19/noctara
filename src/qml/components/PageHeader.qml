import QtQuick
import Noctara

// Title row of a page: a display title, an optional second line, page-specific controls (the default property)
// and, when `viewPage` is set, the list/grid switch that also updates that page's Layout → Pages setting.
Item {
    id: root

    property string title
    property string subtitle
    property string viewPage  // "library", "likes", "history", "charts" or "" for no switch
    default property alias controls: controlRow.data

    implicitHeight: Math.max(titles.implicitHeight, controlRow.implicitHeight)

    Column {
        id: titles
        anchors.left: parent.left
        anchors.right: controlRow.left
        anchors.rightMargin: Theme.space.lg
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.xxs
        Text {
            width: parent.width
            text: root.title
            font: Theme.font.display
            color: Theme.colors.textPrimary
            elide: Text.ElideRight
            Accessible.role: Accessible.Heading
        }
        Text {
            width: parent.width
            visible: text.length > 0
            text: root.subtitle
            font: Theme.font.body
            color: Theme.colors.textSecondary
            elide: Text.ElideRight
        }
    }
    Row {
        id: controlRow
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        spacing: Theme.space.sm

        SegmentedControl {
            visible: root.viewPage.length > 0
            anchors.verticalCenter: parent.verticalCenter
            currentId: Layout.views[root.viewPage] || "list"
            model: [{ id: "list", label: qsTr("List") }, { id: "grid", label: qsTr("Grid") }]
            onActivated: id => Layout.setView(root.viewPage, id)
        }
    }
}
