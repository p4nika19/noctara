import QtQuick
import Noctara
import "../components"

// The signed-in account's library: its playlists and albums (own and liked), and the people it follows.
Item {
    id: root

    property string tab: "playlists"

    function load() {
        if (Account.signedIn)
            Catalog.ensure(tab === "following" ? "following" : "library")
    }
    Component.onCompleted: load()
    onTabChanged: load()
    Connections {
        target: Account
        function onChanged() { root.load() }
    }

    // The search within the tab shown.
    CollectionFilter {
        id: listFilter
        source: root.tab === "following" ? Catalog.following : Catalog.library
    }

    PageHeader {
        id: header
        width: parent.width
        title: qsTr("Library")
        viewPage: "library"
        FilterField {
            anchors.verticalCenter: parent.verticalCenter
            visible: Account.signedIn && ((listFilter.source ? listFilter.source.count : 0) > 0 || listFilter.active)
            filter: listFilter
            placeholderText: root.tab === "following" ? qsTr("Search people") : qsTr("Search playlists and albums")
        }
        SegmentedControl {
            anchors.verticalCenter: parent.verticalCenter
            visible: Account.signedIn
            currentId: root.tab
            model: [{ id: "playlists", label: qsTr("Playlists & albums") }, { id: "following", label: qsTr("Following") }]
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
        mode: Layout.views.library
        emptyText: listFilter.active ? qsTr("Nothing here matches “%1”.").arg(listFilter.text.trim())
                   : root.tab === "following" ? qsTr("You do not follow anyone yet.")
                   : qsTr("No playlists or albums yet. Create or like some on SoundCloud.")
    }
    EmptyPage {
        anchors.fill: parent
        visible: !Account.signedIn
        iconName: "library"
        title: qsTr("Your library")
        message: qsTr("Sign in to your SoundCloud account (Settings → Accounts) to see your playlists, albums and the people you follow.")
    }
}
