import QtQuick
import Noctara
import "../components"

// Personalized SoundCloud discovery: Daily Drops (new releases every day) and Weekly Wave (updated every Monday).
// Both are system playlists resolved for the signed-in account; their stub tracks hydrate through CollectionModel.
Item {
    id: root

    property string tab: "daily-drops"
    readonly property string tabLabel: tab === "weekly-wave" ? qsTr("Weekly Wave") : qsTr("Daily Drops")

    function load() {
        if (Account.signedIn)
            Catalog.ensure(tab)
    }
    Component.onCompleted: load()
    onTabChanged: load()
    Connections {
        target: Account
        function onChanged() { root.load() }
    }

    CollectionFilter {
        id: listFilter
        source: root.tab === "weekly-wave" ? Catalog.weeklyWave : Catalog.dailyDrops
    }

    PageHeader {
        id: header
        width: parent.width
        title: qsTr("Discover")
        subtitle: root.tabLabel
        viewPage: "charts"
        search: FilterField {
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width)
            visible: Account.signedIn && ((listFilter.source ? listFilter.source.count : 0) > 0 || listFilter.active)
            filter: listFilter
            prompt: qsTr("Search in discoveries")
        }
        Button {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0
            variant: "primary"
            iconName: "play"
            text: qsTr("Play")
            onClicked: Player.playCollection(listFilter.model, 0, root.tabLabel)
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0
            iconName: "shuffle"
            text: qsTr("Shuffle")
            onClicked: Player.shufflePlay(listFilter.model, root.tabLabel)
        }
        SegmentedControl {
            anchors.verticalCenter: parent.verticalCenter
            visible: Account.signedIn
            currentId: root.tab
            model: [{ id: "daily-drops", label: qsTr("Daily Drops") }, { id: "weekly-wave", label: qsTr("Weekly Wave") }]
            onActivated: id => root.tab = id
        }
    }
    CollectionView {
        anchors.top: header.bottom
        anchors.topMargin: Theme.space.lg
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: Account.signedIn
        model: listFilter.model
        mode: Layout.views.charts
        context: root.tabLabel
        emptyText: listFilter.active ? qsTr("Nothing here matches “%1”.").arg(listFilter.text.trim())
                   : root.tab === "weekly-wave" ? qsTr("Your Weekly Wave has no tracks right now.")
                   : qsTr("Your Daily Drops has no tracks right now.")
    }
    EmptyPage {
        anchors.fill: parent
        visible: !Account.signedIn
        iconName: "radio"
        title: qsTr("Discover new music")
        message: qsTr("Sign in to your SoundCloud account (Settings → Accounts) to see Daily Drops and Weekly Wave, selected for your taste.")
    }
}
