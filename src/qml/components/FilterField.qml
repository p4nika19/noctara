import QtQuick
import Noctara

// A search within one list (likes, the library, a playlist), driving a CollectionFilter: typing narrows the list,
// the clear button or Escape brings everything back. Escape only goes back a page once the field is empty.
TextField {
    id: control

    property CollectionFilter filter

    implicitWidth: 240 * Theme.scale
    iconName: "search"
    rightPadding: clear.visible ? clear.width + Theme.space.xs * 2 : Theme.space.md
    text: filter ? filter.text : ""
    onTextEdited: if (filter) filter.text = text

    Keys.onShortcutOverride: event => event.accepted = event.key === Qt.Key_Escape && text.length > 0
    Keys.onEscapePressed: event => {
        if (text.length > 0) {
            if (filter)
                filter.text = ""
            event.accepted = true
        } else {
            event.accepted = false
        }
    }

    IconButton {
        id: clear
        anchors.right: parent.right
        anchors.rightMargin: Theme.space.xs
        anchors.verticalCenter: parent.verticalCenter
        width: Theme.space.controlHeightSmall
        height: width
        visible: control.text.length > 0
        material: "flat"
        iconName: "x"
        iconSize: Theme.space.iconSizeSmall
        text: qsTr("Clear the search")
        onClicked: {
            if (control.filter)
                control.filter.text = ""
            control.forceActiveFocus()
        }
    }
}
