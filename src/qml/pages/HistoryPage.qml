import QtQuick
import Noctara
import "../components"

// Recently played tracks of the signed-in account (SoundCloud's own history, so other devices count too).
Item {
    id: root

    Component.onCompleted: if (Account.signedIn) Catalog.ensure("history")
    Connections {
        target: Account
        function onChanged() { if (Account.signedIn) Catalog.ensure("history") }
    }

    PageHeader {
        id: header
        width: parent.width
        title: qsTr("History")
        viewPage: "history"
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: Account.signedIn
            iconName: "refresh-cw"
            text: qsTr("Refresh")
            onClicked: Catalog.history.reload()
        }
    }
    CollectionView {
        anchors.top: header.bottom
        anchors.topMargin: Theme.space.lg
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: Account.signedIn
        model: Catalog.history
        mode: Layout.views.history
        context: qsTr("History")
        emptyText: qsTr("Nothing played yet.")
    }
    EmptyPage {
        anchors.fill: parent
        visible: !Account.signedIn
        iconName: "clock"
        title: qsTr("Listening history")
        message: qsTr("Sign in to your SoundCloud account (Settings → Accounts) to see what you played recently.")
    }
}
