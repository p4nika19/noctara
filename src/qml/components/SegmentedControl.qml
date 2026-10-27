import QtQuick
import Noctara

// Mutually exclusive options. model: [{id, label, enabled, reason}]. When given more width than it needs
// (e.g. full-width in settings), segments share it equally.
FocusScope {
    id: root

    property var model: []
    property string currentId
    signal activated(string id)

    readonly property int currentIndex: {
        for (let i = 0; i < model.length; ++i)
            if (model[i].id === currentId) return i
        return -1
    }
    readonly property real inset: 3 * Theme.scale
    readonly property real naturalSegment: {
        let widest = 0
        for (let i = 0; i < repeater.count; ++i) {
            const item = repeater.itemAt(i)
            if (item) widest = Math.max(widest, item.naturalWidth)
        }
        return widest
    }

    implicitWidth: naturalSegment * model.length + inset * 2
    implicitHeight: Theme.space.controlHeight - 4 * Theme.scale
    activeFocusOnTab: true
    Accessible.role: Accessible.PageTabList

    Keys.onLeftPressed: step(-1)
    Keys.onRightPressed: step(1)
    function step(delta) {
        for (let i = currentIndex + delta; i >= 0 && i < model.length; i += delta) {
            if (model[i].enabled !== false) { activated(model[i].id); return }
        }
    }

    Rectangle {
        id: track
        anchors.fill: parent
        radius: Math.min(Theme.radius.button, height / 2)
        color: Theme.dark ? Qt.rgba(0, 0, 0, 0.3) : Qt.rgba(0, 0, 0, 0.05)
        border.width: Theme.space.hairline
        border.color: Theme.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.08)
    }
    Item {
        id: pill
        visible: root.currentIndex >= 0 && width > 0
        y: root.inset
        width: 0
        height: root.height - root.inset * 2
        Behavior on x { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
        Behavior on width { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }

        Shadow {
            anchors.fill: parent
            z: -1
            radius: pillFill.radius
            blur: 20 * Theme.scale
            spread: -6 * Theme.scale
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                           Theme.glass.glowControls ? Theme.glass.glow * 0.8 : 0)
        }
        Rectangle {
            id: pillFill
            anchors.fill: parent
            radius: Math.max(0, track.radius - root.inset)
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, Theme.dark ? 0.28 : 0.16)
            border.width: Theme.space.hairline
            border.color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.55)
        }
    }
    FocusRing { target: root; targetRadius: track.radius; visible: root.activeFocus }

    Row {
        x: root.inset
        anchors.verticalCenter: parent.verticalCenter
        Repeater {
            id: repeater
            model: root.model
            delegate: Item {
                id: segment
                required property var modelData
                required property int index
                readonly property bool selected: index === root.currentIndex
                readonly property bool available: modelData.enabled !== false
                readonly property real naturalWidth: label.implicitWidth + Theme.space.lg * 2
                width: Math.max(naturalWidth, (root.width - root.inset * 2) / Math.max(1, root.model.length))
                height: root.height - root.inset * 2
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData.label
                Accessible.checked: selected

                // The selected segment drives the pill. RestoreNone: a deselected segment must not restore
                // the pill's previous geometry.
                Binding {
                    when: segment.selected
                    restoreMode: Binding.RestoreNone
                    target: pill
                    property: "x"
                    value: segment.x + root.inset
                }
                Binding {
                    when: segment.selected
                    restoreMode: Binding.RestoreNone
                    target: pill
                    property: "width"
                    value: segment.width
                }

                Text {
                    id: label
                    anchors.centerIn: parent
                    width: Math.min(implicitWidth, segment.width - Theme.space.sm)
                    horizontalAlignment: Text.AlignHCenter
                    text: segment.modelData.label
                    font: Theme.font.label
                    elide: Text.ElideRight
                    color: segment.selected ? Theme.colors.textPrimary
                                            : (area.containsMouse ? Theme.colors.textPrimary : Theme.colors.textSecondary)
                    opacity: segment.available ? 1 : 0.4
                }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: segment.available ? Qt.PointingHandCursor : Qt.ForbiddenCursor
                    onClicked: if (segment.available) root.activated(segment.modelData.id)
                }
                Tooltip {
                    visible: area.containsMouse && !segment.available && (segment.modelData.reason || "").length > 0
                    text: segment.modelData.reason || ""
                }
            }
        }
    }
}
