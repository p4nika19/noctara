import QtQuick
import Noctara
import "../components"

// Top of the Appearance page and of the quick panel: the "Theme" group (colour scheme tiles, which set
// appearance.theme.mode) and the live palette. On the page it replaces the generated Theme section header, so it
// carries that section's reset.
Column {
    id: root

    property bool showReset: true

    spacing: Theme.space.md
    bottomPadding: Theme.space.sm

    Item {
        width: root.width
        height: Math.max(title.implicitHeight, reset.visible ? reset.implicitHeight : 0) + Theme.space.lg
        Text {
            id: title
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            text: qsTr("Theme")
            font: Theme.font.groupTitle
            color: Theme.colors.textTertiary
            Accessible.role: Accessible.Heading
        }
        Button {
            id: reset
            visible: root.showReset
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            small: true
            text: qsTr("Reset section")
            onClicked: SettingsUi.resetSection("appearance.theme")
        }
    }
    PresetTiles {
        width: root.width
    }
    Text {
        width: root.width
        text: SettingsUi.currentPreset === "auto"
              ? qsTr("Auto follows the desktop: Midnight while it is dark, Frost while it is light.")
              : qsTr("A preset only changes the colour scheme; materials, shapes and layout stay as they are.")
        font: Theme.font.caption
        color: Theme.colors.textTertiary
        wrapMode: Text.Wrap
    }
    PaletteDots {
        width: root.width
    }
}
