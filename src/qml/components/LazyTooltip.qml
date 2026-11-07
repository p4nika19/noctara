import QtQuick
import Noctara

// A Tooltip created the first time it is to be shown. List rows and settings rows carry many tooltips that are never
// hovered, and each is a popup with its own items, transitions and timers. Non-visual, so it takes no room in a Row.
QtObject {
    id: root

    required property Item host
    property bool shown: false
    property string text

    property Tooltip tip: null
    readonly property Component tooltip: Component { Tooltip {} }

    function create() {
        if (tip !== null)
            return
        tip = tooltip.createObject(host, { text: Qt.binding(() => root.text) })
        // Bound only now, so becoming visible goes through the tooltip's delay exactly as before.
        tip.visible = Qt.binding(() => root.shown)
    }

    onShownChanged: if (shown) create()
    Component.onCompleted: if (shown) create()
}
