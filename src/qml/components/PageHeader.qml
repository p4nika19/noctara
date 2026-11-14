import QtQuick
import Noctara

// Title row of a page: a display title, an optional second line, page-specific controls (the default property)
// and, when `viewPage` is set, the list/grid switch that also updates that page's Layout → Pages setting. `search`
// (a FilterField) sits centred in the space between the titles and the controls.
Item {
    id: root

    property string title
    property string subtitle
    property string viewPage  // "library", "likes", "history", "charts" or "" for no switch
    default property alias controls: controlRow.data
    property alias search: searchSlot.data

    readonly property Item searchItem: searchSlot.children.length > 0 && searchSlot.children[0].visible
                                       ? searchSlot.children[0] : null
    // The titles keep room for the closed search (an open one shrinks to the gap in a narrow window).
    readonly property real searchReserve: !searchItem ? Theme.space.lg
        : (searchItem.collapsedWidth !== undefined ? searchItem.collapsedWidth : searchItem.implicitWidth)
          + Theme.space.xl * 2

    implicitHeight: Math.max(titles.implicitHeight, controlRow.implicitHeight, searchItem ? searchItem.implicitHeight : 0)

    Column {
        id: titles
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        width: Math.max(0, Math.min(Math.max(titleText.implicitWidth, subtitleText.visible ? subtitleText.implicitWidth
                                                                                         : 0),
                                    controlRow.x - root.searchReserve))
        spacing: Theme.space.xxs
        Text {
            id: titleText
            width: parent.width
            text: root.title
            font: Theme.font.display
            color: Theme.colors.textPrimary
            elide: Text.ElideRight
            Accessible.role: Accessible.Heading
        }
        Text {
            id: subtitleText
            width: parent.width
            visible: text.length > 0
            text: root.subtitle
            font: Theme.font.body
            color: Theme.colors.textSecondary
            elide: Text.ElideRight
        }
    }
    // The gap between the titles and the controls; the search is centred in it and never wider.
    Item {
        id: searchSlot
        anchors.left: titles.right
        anchors.leftMargin: Theme.space.xl
        anchors.right: controlRow.left
        anchors.rightMargin: Theme.space.xl
        anchors.top: parent.top
        anchors.bottom: parent.bottom
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
