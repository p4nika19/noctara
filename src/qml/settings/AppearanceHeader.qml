import QtQuick
import Noctara
import "../components"

// Top of the Appearance page and of the quick panel: presets and the live palette.
Column {
    id: root

    spacing: Theme.space.md
    bottomPadding: Theme.space.sm

    Text {
        topPadding: Theme.space.lg
        text: qsTr("Presets")
        font: Theme.font.groupTitle
        color: Theme.colors.textTertiary
        Accessible.role: Accessible.Heading
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
