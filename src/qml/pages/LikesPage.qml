import QtQuick
import Noctara
import "../components"

// The tracks the signed-in account liked, newest first.
Item {
    id: root

    Component.onCompleted: if (Account.signedIn) Catalog.ensure("likes")
    Connections {
        target: Account
        function onChanged() { if (Account.signedIn) Catalog.ensure("likes") }
    }

    // The search within the likes: Play and Shuffle play what it shows.
    CollectionFilter {
        id: listFilter
        source: Catalog.likes
    }

    PageHeader {
        id: header
        width: parent.width
        title: qsTr("Likes")
        subtitle: Catalog.likes.count === 0 ? ""
                  : listFilter.active ? qsTr("%1 · %n match(es)", "", (listFilter.model ? listFilter.model.count : 0))
                                            .arg(qsTr("%n track(s)", "", Catalog.likes.count))
                  : Catalog.likes.loading ? qsTr("%n track(s) · updating…", "", Catalog.likes.count)
                  : qsTr("%n track(s)", "", Catalog.likes.count)
        viewPage: "likes"
        FilterField {
            anchors.verticalCenter: parent.verticalCenter
            visible: Account.signedIn && (Catalog.likes.count > 0 || listFilter.active)
            filter: listFilter
            placeholderText: qsTr("Search in likes")
        }
        Button {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0
            variant: "primary"
            iconName: "play"
            text: qsTr("Play")
            onClicked: Player.playCollection(listFilter.model, 0, qsTr("Likes"))
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0
            iconName: "shuffle"
            text: qsTr("Shuffle")
            onClicked: Player.shufflePlay(listFilter.model, qsTr("Likes"))
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
        mode: Layout.views.likes
        context: qsTr("Likes")
        emptyText: listFilter.active ? qsTr("No liked tracks match “%1”.").arg(listFilter.text.trim())
                                 : qsTr("No liked tracks yet. Like tracks on SoundCloud and they appear here.")
    }
    EmptyPage {
        anchors.fill: parent
        visible: !Account.signedIn
        iconName: "heart"
        title: qsTr("Liked tracks")
        message: qsTr("Sign in to your SoundCloud account (Settings → Accounts) to see the tracks you liked.")
    }
}
