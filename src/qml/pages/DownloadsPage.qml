import QtQuick
import Noctara
import "../components"

// The tracks stored in full in the audio cache, newest first: they play without a connection. Tracks are stored
// as they play (Settings → Storage); the list follows the cache while it is on screen, and a search narrows it.
Item {
    id: root

    Component.onCompleted: Downloads.watching = visible
    onVisibleChanged: Downloads.watching = visible
    Component.onDestruction: Downloads.watching = false

    CollectionFilter {
        id: listFilter
        source: Downloads.tracks
    }

    PageHeader {
        id: header
        width: parent.width
        title: qsTr("Downloaded")
        subtitle: Downloads.tracks.count === 0 ? qsTr("Played tracks stored for listening offline")
                  : listFilter.active ? qsTr("%1 · %n match(es)", "", (listFilter.model ? listFilter.model.count : 0))
                                            .arg(qsTr("%n track(s)", "", Downloads.tracks.count))
                  : qsTr("%n track(s)", "", Downloads.tracks.count)
        FilterField {
            anchors.verticalCenter: parent.verticalCenter
            visible: Downloads.tracks.count > 0 || listFilter.active
            filter: listFilter
            placeholderText: qsTr("Search in downloads")
        }
        Button {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0
            variant: "primary"
            iconName: "play"
            text: qsTr("Play")
            onClicked: Player.playCollection(listFilter.model, 0, qsTr("Downloaded"))
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: (listFilter.model ? listFilter.model.count : 0) > 0
            iconName: "shuffle"
            text: qsTr("Shuffle")
            onClicked: Player.shufflePlay(listFilter.model, qsTr("Downloaded"))
        }
    }
    CollectionView {
        anchors.top: header.bottom
        anchors.topMargin: Theme.space.lg
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        model: listFilter.model
        context: qsTr("Downloaded")
        emptyText: !Downloads.loaded ? ""
                   : listFilter.active ? qsTr("No downloaded tracks match “%1”.").arg(listFilter.text.trim())
                   : qsTr("Nothing downloaded yet. Tracks you play are stored in full and show up here, ready to play offline.")
    }
}
