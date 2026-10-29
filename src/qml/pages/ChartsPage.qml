import QtQuick
import Noctara
import "../components"

// SoundCloud's charts: the tracks trending right now, by genre.
Item {
    id: root

    Component.onCompleted: Catalog.ensure("charts")

    PageHeader {
        id: header
        width: parent.width
        title: qsTr("Charts")
        subtitle: qsTr("Trending on SoundCloud right now")
        viewPage: "charts"
        ComboBox {
            anchors.verticalCenter: parent.verticalCenter
            model: Catalog.genres
            currentId: Catalog.chartGenre
            Accessible.name: qsTr("Genre")
            onPicked: id => Catalog.setChartGenre(id)
        }
    }
    CollectionView {
        anchors.top: header.bottom
        anchors.topMargin: Theme.space.lg
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        model: Catalog.charts
        mode: Layout.views.charts
        context: qsTr("Charts")
        emptyText: qsTr("This chart is empty right now.")
    }
}
