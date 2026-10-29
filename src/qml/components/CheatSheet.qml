import QtQuick
import Noctara

// Every keyboard shortcut at a glance (Ctrl+/ by default). Reflects the current mapping; Escape or a click
// outside closes it.
FocusScope {
    id: root

    property bool open: false
    signal closeRequested()

    visible: panel.opacity > 0
    focus: open
    Keys.onEscapePressed: root.closeRequested()

    Rectangle {
        anchors.fill: parent
        color: Theme.colors.scrim
        opacity: root.open ? 0.6 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.motion.normal } }
        MouseArea {
            anchors.fill: parent
            enabled: root.open
            onClicked: root.closeRequested()
        }
    }
    GlassPanel {
        id: panel
        level: 2
        radius: Theme.radius.lg
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.space.xl * 2, 720 * Theme.scale)
        height: Math.min(parent.height - Theme.space.xl * 2, content.implicitHeight + inset * 2)
        opacity: root.open ? 1 : 0
        scale: root.open ? 1 : 0.97
        Behavior on opacity { NumberAnimation { duration: Theme.motion.normal } }
        Behavior on scale { NumberAnimation { duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Keyboard shortcuts")
        readonly property real inset: Math.max(Theme.space.xl, cornerInset)

        Column {
            id: content
            x: panel.inset
            y: panel.inset
            width: panel.width - panel.inset * 2
            spacing: Theme.space.lg
            Item {
                width: parent.width
                height: Math.max(title.implicitHeight, close.implicitHeight)
                Text {
                    id: title
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Keyboard shortcuts")
                    font: Theme.font.title
                    color: Theme.colors.textPrimary
                }
                IconButton {
                    id: close
                    anchors.right: parent.right
                    iconName: "x"
                    text: qsTr("Close")
                    onClicked: root.closeRequested()
                }
            }
            Grid {
                width: parent.width
                columns: width > 560 * Theme.scale ? 2 : 1
                columnSpacing: Theme.space.xl
                rowSpacing: Theme.space.sm
                Repeater {
                    model: Shortcuts.cheatSheet
                    delegate: Item {
                        required property var modelData
                        width: (content.width - (parent.columns - 1) * Theme.space.xl) / parent.columns
                        height: Math.max(label.implicitHeight, keycap.height)
                        Text {
                            id: label
                            anchors.left: parent.left
                            anchors.right: keycap.left
                            anchors.rightMargin: Theme.space.md
                            anchors.verticalCenter: parent.verticalCenter
                            text: parent.modelData.label
                            font: Theme.font.body
                            color: Theme.colors.textSecondary
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            id: keycap
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: keys.implicitWidth + Theme.space.md * 2
                            height: keys.implicitHeight + Theme.space.xs * 2
                            radius: Theme.radius.xs
                            color: Theme.colors.stateHover
                            border.width: Theme.space.hairline
                            border.color: Theme.colors.divider
                            Text {
                                id: keys
                                anchors.centerIn: parent
                                text: parent.parent.modelData.keys
                                font: Theme.font.mono
                                color: Theme.colors.textPrimary
                            }
                        }
                    }
                }
            }
            Text {
                width: parent.width
                text: qsTr("Change them under Settings → Keyboard. Media keys always work.")
                font: Theme.font.caption
                color: Theme.colors.textTertiary
                wrapMode: Text.Wrap
            }
        }
    }
}
