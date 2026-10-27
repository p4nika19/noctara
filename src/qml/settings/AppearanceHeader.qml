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
        width: Math.min(root.width, 560 * Theme.scale)
    }
    Text {
        width: root.width
        text: SettingsUi.currentPreset.length > 0
              ? qsTr("A preset only sets the options below; you can change any of them afterwards.")
              : qsTr("Custom: your settings differ from every preset.")
        font: Theme.font.caption
        color: Theme.colors.textTertiary
        wrapMode: Text.Wrap
    }
    PaletteDots {
        width: root.width
    }
}
