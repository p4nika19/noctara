import QtQuick
import Noctara

// Mutually exclusive options with a sliding selection pill. model: [{id, label, enabled, reason}]
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

    implicitWidth: row.implicitWidth + Theme.space.xs * 2
    implicitHeight: Theme.space.controlHeight
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
        anchors.fill: parent
        radius: height / 2
        color: Theme.colors.surfaceOverlay
        border.width: Theme.space.hairline
        border.color: Theme.colors.outline
    }
    Rectangle {
        id: pill
        visible: root.currentIndex >= 0 && width > 0
        y: Theme.space.xs
        width: 0
        height: root.height - Theme.space.xs * 2
        radius: height / 2
        color: Theme.colors.accent
        Behavior on x { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
        Behavior on width { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
    }
    FocusRing { target: root; targetRadius: root.height / 2; visible: root.activeFocus }

    Row {
        id: row
        x: Theme.space.xs
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
                width: label.implicitWidth + Theme.space.lg * 2
                height: root.height - Theme.space.xs * 2
                Accessible.role: Accessible.PageTab
                Accessible.name: modelData.label
                Accessible.checked: selected

                // The selected segment drives the sliding pill (declarative; updates after layout too).
                // RestoreNone: a deselected segment must not restore the pill's previous geometry.
                Binding {
                    when: segment.selected
                    restoreMode: Binding.RestoreNone
                    target: pill
                    property: "x"
                    value: segment.x + row.x
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
                    text: segment.modelData.label
                    font: Theme.font.label
                    color: segment.selected ? Theme.colors.onAccent : Theme.colors.textSecondary
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
