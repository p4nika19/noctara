import QtQuick
import QtQuick.Templates as T
import Noctara

// Modal dialog to create a permanent playlist from Daily Drops or Weekly Wave with track selection.
Dialog {
    id: dialog

    property string kind: ""
    property var tracks: []
    property var selectedMap: ({})
    property int selectedCount: 0

    function openFor(discoveryKind, model) {
        kind = discoveryKind
        var list = []
        var sel = {}
        var count = model ? model.count : 0
        for (var i = 0; i < count; ++i) {
            var tId = model.itemIdAt(i)
            if (tId > 0) {
                list.push({
                    id: tId,
                    title: model.titleAt(i),
                    artist: model.artistAt(i),
                    artwork: model.artworkAt(i)
                })
                sel[tId] = true
            }
        }
        tracks = list
        selectedMap = sel
        selectedCount = list.length

        var own = Catalog.ownPlaylists()
        var baseTitle = discoveryKind === "weekly-wave" ? "Weekly Wave" : "Daily Drops"
        var titles = {}
        for (var j = 0; j < own.length; ++j) {
            titles[own[j].title] = true
        }
        var candidate = baseTitle
        if (titles[candidate]) {
            var num = 1
            while (titles[baseTitle + " " + num]) {
                num++
            }
            candidate = baseTitle + " " + num
        }
        nameField.text = candidate
        privateSwitch.checked = true
        open()
    }

    function toggleTrack(id) {
        var sel = Object.assign({}, selectedMap)
        if (sel[id]) {
            delete sel[id]
        } else {
            sel[id] = true
        }
        selectedMap = sel
        selectedCount = Object.keys(sel).length
    }

    function toggleAll() {
        var sel = {}
        if (selectedCount < tracks.length) {
            for (var i = 0; i < tracks.length; ++i) {
                sel[tracks[i].id] = true
            }
        }
        selectedMap = sel
        selectedCount = Object.keys(sel).length
    }

    title: kind === "weekly-wave" ? qsTr("Create playlist from Weekly Wave")
                                  : qsTr("Create playlist from Daily Drops")
    confirmText: qsTr("Create playlist")
    confirmEnabled: nameField.text.trim().length > 0 && selectedCount > 0
    initialFocus: nameField

    onAccepted: {
        var selectedIds = []
        for (var i = 0; i < tracks.length; ++i) {
            if (selectedMap[tracks[i].id]) {
                selectedIds.push(tracks[i].id)
            }
        }
        Catalog.createPlaylistWithTracks(nameField.text.trim(), privateSwitch.checked, selectedIds)
    }

    TextField {
        id: nameField
        width: parent.width
        placeholderText: qsTr("Playlist name")
        onAccepted: if (dialog.confirmEnabled) dialog.accept()
    }

    Row {
        spacing: Theme.space.md
        Switch {
            id: privateSwitch
            checked: true
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: privateSwitch.checked ? qsTr("Private: only you can see it") : qsTr("Public: visible to everyone")
            font: Theme.font.body
            color: Theme.colors.textPrimary
            MouseArea {
                anchors.fill: parent
                onClicked: privateSwitch.toggle()
            }
        }
    }

    Item {
        width: parent.width
        height: Math.max(tracksLabel.implicitHeight, selectAllLabel.implicitHeight)
        Text {
            id: tracksLabel
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Tracks (%1 of %2 selected)").arg(dialog.selectedCount).arg(dialog.tracks.length)
            font: Theme.font.bodyStrong
            color: Theme.colors.textPrimary
        }
        Text {
            id: selectAllLabel
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: dialog.selectedCount === dialog.tracks.length ? qsTr("Deselect all") : qsTr("Select all")
            font: Theme.font.caption
            color: Theme.colors.accent
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: dialog.toggleAll()
            }
        }
    }

    ListView {
        id: trackList
        width: parent.width
        height: Math.min(240 * Theme.scale, Math.max(1, dialog.tracks.length) * 42 * Theme.scale)
        clip: true
        interactive: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {
            policy: trackList.contentHeight > trackList.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
        }
        spacing: Theme.space.xs
        model: dialog.tracks

        WheelHandler {
            target: null
            onWheel: (event) => {
                var step = 42 * Theme.scale * 3
                if (event.angleDelta.y > 0) {
                    trackList.contentY = Math.max(trackList.originY, trackList.contentY - step)
                } else if (event.angleDelta.y < 0) {
                    trackList.contentY = Math.min(Math.max(trackList.originY, trackList.originY + trackList.contentHeight - trackList.height),
                                                  trackList.contentY + step)
                }
            }
        }

        delegate: Rectangle {
            id: trackRow
            required property var modelData
            required property int index
            readonly property bool selected: !!dialog.selectedMap[modelData.id]
            width: trackList.width
            height: 38 * Theme.scale
            radius: Theme.radius.sm
            color: rowMouse.containsMouse ? Theme.colors.surfaceRaised : "transparent"

            Row {
                anchors.fill: parent
                anchors.leftMargin: Theme.space.sm
                anchors.rightMargin: Theme.space.sm
                spacing: Theme.space.sm

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18 * Theme.scale
                    height: 18 * Theme.scale
                    radius: Theme.radius.xs
                    color: trackRow.selected ? Theme.colors.accent : "transparent"
                    border.width: Theme.space.hairline
                    border.color: trackRow.selected ? Theme.colors.accent : Theme.colors.borderSubtle

                    Icon {
                        anchors.centerIn: parent
                        visible: trackRow.selected
                        name: "check"
                        size: 12 * Theme.scale
                        color: Theme.colors.onAccent
                    }
                }

                CoverArt {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28 * Theme.scale
                    height: 28 * Theme.scale
                    radius: Theme.radius.xs
                    source: trackRow.modelData.artwork || ""
                }

                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 70 * Theme.scale
                    spacing: 1

                    Text {
                        width: parent.width
                        text: trackRow.modelData.title || ""
                        font: Theme.font.caption
                        color: Theme.colors.textPrimary
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        text: trackRow.modelData.artist || ""
                        font: Theme.font.eyebrow
                        color: Theme.colors.textSecondary
                        elide: Text.ElideRight
                    }
                }
            }

            MouseArea {
                id: rowMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: dialog.toggleTrack(trackRow.modelData.id)
                onWheel: (wheel) => {
                    var step = 42 * Theme.scale * 3
                    if (wheel.angleDelta.y > 0) {
                        trackList.contentY = Math.max(trackList.originY, trackList.contentY - step)
                    } else if (wheel.angleDelta.y < 0) {
                        trackList.contentY = Math.min(Math.max(trackList.originY, trackList.originY + trackList.contentHeight - trackList.height),
                                                      trackList.contentY + step)
                    }
                }
            }
        }
    }
}
