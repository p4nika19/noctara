import QtQuick
import Noctara

// One item of a CollectionModel as a card in a grid: cover (round for people), title and second line. Hovering a
// track or playlist shows a play button over the cover. Cards are plain items, never glass panels.
Item {
    id: root

    required property int index
    required property string kind
    required property var itemId
    required property var ownerId
    required property string permalink
    required property string title
    required property string subtitle
    required property string artwork
    required property bool protectedMedia
    required property bool album
    required property bool pending
    property real coverSize: width

    signal activated()
    signal playRequested()
    signal menuRequested(point point)

    implicitHeight: coverSize + Theme.space.sm + titleText.implicitHeight + subtitleText.implicitHeight
                    + Theme.space.xxs + Theme.space.md
    Accessible.role: Accessible.ListItem
    Accessible.name: pending ? qsTr("Loading") : qsTr("%1, %2").arg(title).arg(subtitle)

    CoverArt {
        id: cover
        width: root.coverSize
        height: width
        source: root.artwork
        shape: root.kind === "user" ? "circle" : Theme.coverShape
        radius: Theme.radius.md
        scale: area.containsMouse ? 1.02 : 1
        Behavior on scale { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }

        IconButton {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.space.sm
            visible: root.kind !== "user" && !root.protectedMedia
            opacity: area.containsMouse || hovered || activeFocus ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: Theme.motion.fast } }
            material: "primary"
            iconName: "play"
            text: qsTr("Play")
            onClicked: root.playRequested()
        }
    }
    MouseArea {
        id: area
        anchors.fill: parent
        z: -1
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: Qt.PointingHandCursor
        onClicked: mouse => {
            if (mouse.button === Qt.RightButton)
                root.menuRequested(Qt.point(mouse.x, mouse.y))
            else
                root.activated()
        }
    }
    Text {
        id: titleText
        anchors.top: cover.bottom
        anchors.topMargin: Theme.space.sm
        width: parent.width
        text: root.pending ? "" : root.title
        font: Theme.font.bodyStrong
        color: Theme.colors.textPrimary
        elide: Text.ElideRight
        textFormat: Text.PlainText
        horizontalAlignment: root.kind === "user" ? Text.AlignHCenter : Text.AlignLeft
    }
    Text {
        id: subtitleText
        anchors.top: titleText.bottom
        anchors.topMargin: Theme.space.xxs
        width: parent.width
        text: root.pending ? "" : (root.kind === "playlist" && root.album ? qsTr("Album · %1").arg(root.subtitle)
                                                                          : root.subtitle)
        font: Theme.font.caption
        color: Theme.colors.textSecondary
        elide: Text.ElideRight
        textFormat: Text.PlainText
        horizontalAlignment: titleText.horizontalAlignment
    }
}
