import QtQuick
import Noctara
import "../components"

// The full list behind a discovery card on Home: Daily Drops or Weekly Wave. Opened through Main.qml's detail stack
// (Back returns to Home), never as a separate navigation destination.
Item {
    id: root

    required property string kind
    readonly property CollectionModel model: kind === "weekly-wave" ? Catalog.weeklyWave : Catalog.dailyDrops
    readonly property string title: kind === "weekly-wave" ? qsTr("Weekly Wave") : qsTr("Daily Drops")
    readonly property string description: kind === "weekly-wave"
                                         ? qsTr("The best of SoundCloud just for you. Updated every Monday")
                                         : qsTr("New releases based on your taste. Updated every day")

    Component.onCompleted: Catalog.ensure(kind)

    CollectionFilter {
        id: listFilter
        source: root.model
    }

    PageHeader {
        id: header
        width: parent.width
        title: root.title
        subtitle: root.description
        viewPage: "charts"
        search: FilterField {
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width)
            visible: (listFilter.source ? listFilter.source.count : 0) > 0 || listFilter.active
            filter: listFilter
            prompt: qsTr("Search in discoveries")
        }
        Button {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0
            variant: "primary"
            iconName: "play"
            text: qsTr("Play")
            onClicked: Player.playCollection(listFilter.model, 0, root.title)
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0
            iconName: "shuffle"
            text: qsTr("Shuffle")
            onClicked: Player.shufflePlay(listFilter.model, root.title)
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0 && Account.signedIn
            iconName: "plus"
            text: qsTr("Create playlist")
            onClicked: Window.window.openDiscoveryPlaylist(root.kind, listFilter.model || root.model)
        }
    }
    CollectionView {
        anchors.top: header.bottom
        anchors.topMargin: Theme.space.lg
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        model: listFilter.model
        mode: Layout.views.charts
        context: root.title
        emptyText: listFilter.active ? qsTr("Nothing here matches “%1”.").arg(listFilter.text.trim())
                   : qsTr("There are no tracks here right now.")
    }
}
