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
    leftPadding: Theme.space.lg
    rightPadding: Theme.space.xl + Theme.space.sm
    font: Theme.font.body
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.name: displayText

    onActivated: index => {
        const entry = model[index]
        if (entry && entry.enabled !== false)
            picked(entry.id)
        else  // refused: back to currentId, restoring the binding a plain assignment would remove
            currentIndex = Qt.binding(() => count > 0 ? indexOfValue(currentId) : -1)
    }

    contentItem: Text {
        text: control.displayText
        textFormat: Text.PlainText
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
    background: ButtonSurface {
        hovered: control.hovered && control.enabled
        pressed: control.pressed
        opacity: control.enabled ? 1 : 0.45
        FocusRing { target: control; targetRadius: parent.radius; visible: control.visualFocus }
    }

    delegate: T.ItemDelegate {
        id: entry
        required property var modelData
        required property int index
        readonly property bool available: modelData.enabled !== false
        width: ListView.view ? ListView.view.width : implicitWidth
        implicitHeight: Math.max(Theme.space.controlHeight, labels.implicitHeight + Theme.space.sm * 2)
        leftPadding: Theme.space.md
        rightPadding: Theme.space.md
        enabled: available
        hoverEnabled: true
        highlighted: control.highlightedIndex === index
        Accessible.name: modelData.label
        // The control stretches contentItem to its full height; centre the label block inside it.
        contentItem: Item {
            implicitHeight: labels.implicitHeight
            Column {
                id: labels
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    width: parent.width
                    text: entry.modelData.label
                    textFormat: Text.PlainText
                    font: entry.index === control.currentIndex ? Theme.font.bodyStrong : Theme.font.body
                    color: entry.index === control.currentIndex ? Theme.colors.accentText : Theme.colors.textPrimary
                    opacity: entry.available ? 1 : 0.45
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    visible: !entry.available && (entry.modelData.reason || "").length > 0
                    text: entry.modelData.reason || ""
                    textFormat: Text.PlainText
                    font: Theme.font.caption
                    color: Theme.colors.textTertiary
                    wrapMode: Text.Wrap
                }
            }
        }
        background: Rectangle {
            // A menu row, not a button: it follows the popup's corners (concentric radii).
            radius: Math.min(height / 2, Math.max(0, Theme.radius.md - control.popup.padding))
            color: entry.highlighted ? Theme.colors.stateHover : "transparent"
        }
    }

    popup: T.Popup {
        // At least as wide as the button, wider for long (translated) entries; right-aligned with it.
        y: control.height + Theme.space.xs
        width: Math.max(control.width, 340 * Theme.scale)
        x: control.width - width
        implicitHeight: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, 360 * Theme.scale)
        // Keeps a row rounded to a pill clear of a large corner.
        padding: Math.max(Theme.space.xs, Math.max(0, Theme.radius.md - Theme.space.controlHeight / 2) * (1 - Math.SQRT1_2))
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
        }
        background: GlassPanel { level: 2; liquid: true; radius: Theme.radius.md }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motion.fast }
            NumberAnimation { property: "scale"; from: 0.96; to: 1; duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard }
        }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motion.fast } }
    }
}
