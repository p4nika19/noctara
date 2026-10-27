import QtQuick
import Noctara
import "../components"

// Quick Appearance panel over any page (Ctrl+Shift+A, or right-click the settings button): presets and the
// most-used appearance options with live preview. The rows are the same generated settings rows as on the
// Settings page, so both stay in sync by construction.
FocusScope {
    id: root

    property bool open: false
    signal closeRequested()
    signal allSettingsRequested()

    readonly property var keys: [
        "appearance.accent.adaptive", "appearance.accent.extraction", "appearance.accent.color",
        "appearance.backdrop.style",
        "appearance.glass.buttons", "appearance.glass.blur_strength", "appearance.glass.opacity", "appearance.glass.grain",
        "appearance.glow.intensity", "appearance.glow.cover", "appearance.glow.controls", "appearance.glow.progress",
        "appearance.shape.corner_radius", "appearance.shape.buttons", "layout.content.density",
        "appearance.motion.color_transition"
    ]
    readonly property string presetName: {
        const preset = SettingsUi.presets.find(p => p.id === SettingsUi.currentPreset)
        return preset ? preset.name : ""
    }

    visible: panel.opacity > 0
    focus: open
    Keys.onEscapePressed: root.closeRequested()

    GlassPanel {
        id: panel
        level: 2
        liquid: true
        radius: Theme.radius.lg
        width: Math.min(400 * Theme.scale, root.width)
        height: root.height
        x: root.open ? root.width - width : root.width - width + 28 * Theme.scale
        opacity: root.open ? 1 : 0
        Behavior on x { NumberAnimation { duration: Theme.motion.slow; easing.type: Theme.motion.easingStandard } }
        Behavior on opacity { NumberAnimation { duration: Theme.motion.normal } }
        Accessible.role: Accessible.Dialog
        Accessible.name: qsTr("Appearance")

        Item {
            id: head
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Math.max(Theme.space.lg, panel.cornerInset)
            height: Math.max(titles.implicitHeight, close.implicitHeight)
            Column {
                id: titles
                anchors.left: parent.left
                anchors.right: close.left
                anchors.verticalCenter: parent.verticalCenter
                spacing: Theme.space.xxs
                Text {
                    width: parent.width
                    text: qsTr("Appearance")
                    font: Theme.font.title
                    color: Theme.colors.textPrimary
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    //: %1 = colour preset name (Auto, Midnight or Frost)
                    text: qsTr("Midnight Glass · %1").arg(root.presetName)
                    font: Theme.font.caption
                    color: Theme.colors.textTertiary
                    elide: Text.ElideRight
                }
            }
            IconButton {
                id: close
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                iconName: "x"
                text: qsTr("Close panel")
                onClicked: root.closeRequested()
            }
        }

        ListView {
            id: list
            anchors.top: head.bottom
            anchors.bottom: foot.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.space.lg
            anchors.rightMargin: Theme.space.sm
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            rightMargin: Theme.space.md
            bottomMargin: Theme.space.lg
            model: root.open || panel.opacity > 0 ? SettingsUi.subset(root.keys) : null
            ScrollBar.vertical: ScrollBar {}
            header: AppearanceHeader {
                width: list.width - list.rightMargin
                showReset: false
            }
            delegate: SettingsRow {
                compact: true
                width: list.width - list.rightMargin
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: foot.top
            height: Theme.space.hairline
            color: Theme.colors.divider
        }
        Row {
            id: foot
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Math.max(Theme.space.lg, panel.cornerInset)
            spacing: Theme.space.sm
            Button {
                width: (parent.width - parent.spacing) / 2
                text: qsTr("Reset")
                onClicked: SettingsUi.resetKeys(root.keys)
            }
            Button {
                width: (parent.width - parent.spacing) / 2
                variant: "primary"
                text: qsTr("All settings")
                onClicked: root.allSettingsRequested()
            }
        }
    }
}
