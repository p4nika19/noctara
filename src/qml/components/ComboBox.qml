import QtQuick
import QtQuick.Templates as T
import Noctara

// Dropdown over [{id, label, enabled, reason}] models. Emits activated(id).
T.ComboBox {
    id: control

    property string currentId
    signal picked(string id)

    model: []
    textRole: "label"
    valueRole: "id"
    // `count` makes the binding re-run once the model is populated.
    currentIndex: count > 0 ? indexOfValue(currentId) : -1
    implicitWidth: Math.max(200 * Theme.scale, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Theme.space.controlHeight
    leftPadding: Theme.space.md
    rightPadding: Theme.space.xl + Theme.space.sm
    font: Theme.font.body
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.name: displayText

    onActivated: index => {
        const entry = model[index]
        if (entry && entry.enabled !== false)
            picked(entry.id)
        else
            currentIndex = indexOfValue(currentId)
    }

    contentItem: Text {
        text: control.displayText
        font: control.font
        color: Theme.colors.textPrimary
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Icon {
        x: control.width - width - Theme.space.md
        anchors.verticalCenter: parent.verticalCenter
        name: "chevron-down"
        size: Theme.space.iconSizeSmall
        color: Theme.targetColors.iconSecondary
        rotation: control.popup.visible ? 180 : 0
        Behavior on rotation { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
    }
    background: Rectangle {
        radius: Theme.radius.sm
        color: Theme.colors.surfaceRaised
        border.width: Theme.space.hairline
        border.color: Theme.colors.outline
        opacity: control.enabled ? 1 : 0.45
        StateLayer { radius: parent.radius; hovered: control.hovered; pressed: control.pressed }
        FocusRing { target: control; targetRadius: parent.radius; visible: control.visualFocus }
    }

    delegate: T.ItemDelegate {
        id: entry
        required property var modelData
        required property int index
        readonly property bool available: modelData.enabled !== false
        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: Theme.space.controlHeight
        leftPadding: Theme.space.md
        rightPadding: Theme.space.md
        enabled: available
        hoverEnabled: true
        highlighted: control.highlightedIndex === index
        Accessible.name: modelData.label
        contentItem: Column {
            spacing: 0
            Text {
                width: parent.width
                text: entry.modelData.label
                font: entry.index === control.currentIndex ? Theme.font.bodyStrong : Theme.font.body
                color: entry.index === control.currentIndex ? Theme.colors.accent : Theme.colors.textPrimary
                opacity: entry.available ? 1 : 0.45
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: !entry.available && (entry.modelData.reason || "").length > 0
                text: entry.modelData.reason || ""
                font: Theme.font.caption
                color: Theme.colors.textTertiary
                elide: Text.ElideRight
            }
        }
        background: Rectangle {
            radius: Theme.radius.xs
            color: entry.highlighted ? Theme.colors.stateHover : "transparent"
        }
    }

    popup: T.Popup {
        y: control.height + Theme.space.xs
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, 360 * Theme.scale)
        padding: Theme.space.xs
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }
        background: GlassPanel { level: 2; radius: Theme.radius.sm }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motion.fast }
            NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard }
        }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motion.fast } }
    }
}
