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

    PageHeader {
        id: header
        width: parent.width
        title: qsTr("Likes")
        subtitle: Catalog.likes.count > 0 ? qsTr("%n track(s)", "", Catalog.likes.count) : ""
        viewPage: "likes"
        Button {
            anchors.verticalCenter: parent.verticalCenter
            visible: Catalog.likes.count > 0
            variant: "primary"
            iconName: "play"
            text: qsTr("Play")
            onClicked: Player.playCollection(Catalog.likes, 0, qsTr("Likes"))
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: Catalog.likes.count > 0
            iconName: "shuffle"
            text: qsTr("Shuffle")
            onClicked: Player.shufflePlay(Catalog.likes, qsTr("Likes"))
        }
    }
    CollectionView {
        anchors.top: header.bottom
        anchors.topMargin: Theme.space.lg
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: Account.signedIn
        model: Catalog.likes
        mode: Layout.views.likes
        context: qsTr("Likes")
        emptyText: qsTr("No liked tracks yet. Like tracks on SoundCloud and they appear here.")
    }
    EmptyPage {
        anchors.fill: parent
        visible: !Account.signedIn
        iconName: "heart"
        title: qsTr("Liked tracks")
        message: qsTr("Sign in to your SoundCloud account (Settings → Accounts) to see the tracks you liked.")
    }
}
