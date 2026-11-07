import QtQuick
import QtQuick.Templates as T
import Noctara

// The right-click menu: one glass popup for the whole window, filled per use with actions
// [{text, icon, run, enabled, checked, separator, danger}]. Opens at the pointer, stays inside the window, and is
// fully usable with the keyboard (arrows, Enter, Escape). Running an action closes it.
T.Popup {
    id: root

    property var actions: []
    property int highlighted: -1

    // `item` and `point` (in the item's coordinates) place the menu at the pointer.
    function show(newActions, item, point) {
        list.widest = 0  // the new entries measure themselves as they are created
        actions = newActions.filter(a => a !== null && a !== undefined)
        highlighted = -1
        list.forceLayout()  // the Column sizes itself on the next frame: clamp with this menu's height, not the last one's
        const where = item.mapToItem(null, point.x, point.y)
        const parentWidth = parent ? parent.width : width
        const parentHeight = parent ? parent.height : height
        x = Math.max(Theme.space.sm, Math.min(where.x, parentWidth - implicitWidth - Theme.space.sm))
        y = Math.max(Theme.space.sm, Math.min(where.y, parentHeight - implicitHeight - Theme.space.sm))
        open()
        list.forceActiveFocus()
    }
    function step(direction) {
        // With nothing highlighted, Down starts at the first entry and Up at the last.
        let next = highlighted >= 0 ? highlighted : (direction > 0 ? -1 : 0)
        for (let i = 0; i < actions.length; ++i) {
            next = (next + direction + actions.length) % actions.length
            const action = actions[next]
            if (!action.separator && action.enabled !== false) {
                highlighted = next
                return
            }
        }
    }
    function trigger(index) {
        const action = actions[index]
        if (!action || action.separator || action.enabled === false)
            return
        close()
        if (action.run)
            action.run()
    }

    parent: T.Overlay.overlay
    modal: false
    focus: true
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutside | T.Popup.CloseOnReleaseOutsideParent
    padding: Theme.space.xs
    implicitWidth: Math.max(220 * Theme.scale, Math.min(380 * Theme.scale, list.widest + padding * 2))
    implicitHeight: list.implicitHeight + padding * 2

    contentItem: Column {
        id: list
        property real widest: 0
        focus: true
        Accessible.role: Accessible.PopupMenu
        Keys.onUpPressed: root.step(-1)
        Keys.onDownPressed: root.step(1)
        Keys.onReturnPressed: root.trigger(root.highlighted)
        Keys.onEnterPressed: root.trigger(root.highlighted)

        Repeater {
            model: root.actions
            delegate: Item {
                id: entry
                required property var modelData
                required property int index
                readonly property bool separator: modelData.separator === true
                readonly property bool available: modelData.enabled !== false
                width: list.width
                height: separator ? Theme.space.md : Theme.space.controlHeightSmall + Theme.space.xxs
                Accessible.role: separator ? Accessible.Separator : Accessible.MenuItem
                Accessible.name: modelData.text || ""
                Accessible.checked: modelData.checked === true

                Rectangle {
                    visible: entry.separator
                    anchors.centerIn: parent
                    width: parent.width - Theme.space.md * 2
                    height: Theme.space.hairline
                    color: Theme.colors.divider
                }
                Rectangle {
                    visible: !entry.separator
                    anchors.fill: parent
                    radius: Theme.radius.sm
                    color: root.highlighted === entry.index && entry.available ? Theme.colors.stateHover
                                                                               : "transparent"
                }
                Icon {
                    id: icon
                    visible: !entry.separator
                    x: Theme.space.md
                    anchors.verticalCenter: parent.verticalCenter
                    name: entry.modelData.icon || ""
                    size: Theme.space.iconSizeSmall
                    color: entry.modelData.danger ? Theme.targetColors.danger : Theme.targetColors.iconSecondary
                    opacity: entry.available ? 1 : 0.4
                }
                Text {
                    id: label
                    visible: !entry.separator
                    x: icon.x + icon.width + Theme.space.md
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - x - Theme.space.md - (check.visible ? check.width + Theme.space.sm : 0)
                    text: entry.modelData.text || ""
                    font: Theme.font.body
                    color: entry.modelData.danger ? Theme.colors.danger : Theme.colors.textPrimary
                    opacity: entry.available ? 1 : 0.45
                    elide: Text.ElideRight
                    Component.onCompleted: list.widest = Math.max(list.widest, x + implicitWidth + Theme.space.xl * 2)
                }
                Icon {
                    id: check
                    visible: entry.modelData.checked === true
                    anchors.right: parent.right
                    anchors.rightMargin: Theme.space.md
                    anchors.verticalCenter: parent.verticalCenter
                    name: "check"
                    size: Theme.space.iconSizeSmall
                    color: Theme.targetColors.accent
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: !entry.separator && entry.available
                    hoverEnabled: true
                    onEntered: root.highlighted = entry.index
                    onClicked: root.trigger(entry.index)
                }
            }
        }
    }
    background: GlassPanel { level: 2; liquid: true; radius: Theme.radius.md }
    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motion.fast }
        NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motion.fast } }
}
