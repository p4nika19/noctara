import QtQuick
import Noctara

// Colour presets (Auto, Midnight, Frost) with preview tiles sharing the full width; the tile of the preset the
// current values match is lit.
Grid {
    id: root

    columns: Math.max(1, SettingsUi.presets.length)
    spacing: Theme.space.sm

    readonly property real tileWidth: (width - spacing * (columns - 1)) / columns

    Repeater {
        model: SettingsUi.presets
        delegate: Item {
            id: tile
            required property var modelData
            readonly property bool selected: SettingsUi.currentPreset === modelData.id
            width: root.tileWidth
            height: preview.height + name.implicitHeight + Theme.space.sm * 3
            activeFocusOnTab: true
            Accessible.role: Accessible.RadioButton
            Accessible.name: modelData.name
            Accessible.checked: selected
            Keys.onSpacePressed: SettingsUi.applyPreset(modelData.id)
            Keys.onReturnPressed: SettingsUi.applyPreset(modelData.id)

            Rectangle {
                anchors.fill: parent
                radius: Theme.radius.md * 0.6
                color: tile.selected ? Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.12)
                                     : (area.containsMouse ? Theme.colors.stateHover : Qt.rgba(1, 1, 1, Theme.dark ? 0.05 : 0.4))
                border.width: Theme.space.hairline
                border.color: tile.selected ? Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.7)
                                            : Theme.colors.edge
            }
            // Preview: the preset's light and accent glowing over its base colour.
            Rectangle {
                id: preview
                x: Theme.space.sm
                y: Theme.space.sm
                width: parent.width - Theme.space.sm * 2
                height: 44 * Theme.scale
                radius: Theme.radius.md * 0.4
                color: tile.modelData.preview[2]
                clip: true
                border.width: Theme.space.hairline
                border.color: Qt.rgba(1, 1, 1, 0.18)
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: tile.modelData.preview[0] }
                    GradientStop { position: 0.45; color: tile.modelData.preview[1] }
                    GradientStop { position: 1.0; color: tile.modelData.preview[2] }
                }
            }
            Text {
                id: name
                x: Theme.space.sm + Theme.space.xxs
                anchors.top: preview.bottom
                anchors.topMargin: Theme.space.sm
                width: parent.width - x * 2
                text: tile.modelData.name
                font: Theme.font.label
                color: Theme.colors.textPrimary
                elide: Text.ElideRight
            }
            FocusRing { target: tile; targetRadius: Theme.radius.md * 0.6 }
            MouseArea {
                id: area
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: SettingsUi.applyPreset(tile.modelData.id)
            }
        }
    }
}
