import QtQuick
import Noctara
import "../components"

Flickable {
    id: root

    contentHeight: column.implicitHeight + Theme.space.xl
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    // Translated names for the glass ids (as on the Performance page), never the raw ids.
    function styleName(style) {
        switch (style) {
        case "kawase": return qsTr("Dual Kawase blur")
        case "gaussian": return qsTr("Gaussian blur")
        default: return qsTr("Solid (no blur)")
        }
    }
    function surfaceName(surface) {
        switch (surface) {
        case "liquid": return qsTr("Liquid Glass")
        case "flat": return qsTr("flat")
        default: return qsTr("frosted")
        }
    }

    component InfoRow: Row {
        property string name
        property string info
        width: column.width
        spacing: Theme.space.md
        Text {
            width: 180 * Theme.scale
            text: parent.name
            font: Theme.font.label
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width - 180 * Theme.scale - Theme.space.md
            text: parent.info
            font: Theme.font.mono
            color: Theme.colors.textPrimary
            wrapMode: Text.WrapAnywhere
        }
    }

    Column {
        id: column
        width: root.width
        spacing: Theme.space.md

        Text {
            text: "Noctara"
            font: Theme.font.display
            color: Theme.colors.textPrimary
        }
        Text {
            //: %1 = version number
            text: qsTr("Version %1").arg(App.version)
            font: Theme.font.mono
            color: Theme.colors.textSecondary
        }
        Text {
            width: parent.width
            text: qsTr("A fast, native SoundCloud client. Not affiliated with SoundCloud.")
            font: Theme.font.body
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }

        Text {
            topPadding: Theme.space.lg
            text: qsTr("Graphics")
            font: Theme.font.subtitle
            color: Theme.colors.textPrimary
        }
        InfoRow {
            name: qsTr("Renderer")
            info: Graphics.hardware ? Graphics.api : qsTr("Software (CPU)")
        }
        InfoRow {
            visible: Graphics.deviceName.length > 0
            name: qsTr("Device")
            info: Graphics.deviceName
        }
        InfoRow {
            visible: Graphics.driver.length > 0
            name: qsTr("Driver")
            info: Graphics.driver
        }
        InfoRow {
            visible: Graphics.safeMode
            name: qsTr("Safe Mode")
            info: qsTr("Active for this session")
        }
        InfoRow {
            name: qsTr("Glass style")
            info: root.styleName(Theme.glass.style) + " / " + root.surfaceName(Theme.glass.surface)
        }

        Text {
            topPadding: Theme.space.lg
            text: qsTr("Configuration")
            font: Theme.font.subtitle
            color: Theme.colors.textPrimary
        }
        InfoRow {
            name: qsTr("Config file")
            info: SettingsUi.configPath
        }
        Button {
            text: qsTr("Open config file")
            iconName: "external-link"
            onClicked: SettingsUi.openConfigFile()
        }

        Text {
            topPadding: Theme.space.lg
            text: qsTr("Open-source licenses")
            font: Theme.font.subtitle
            color: Theme.colors.textPrimary
        }
        Repeater {
            model: [
                { id: "manrope", name: "Manrope", license: "SIL Open Font License 1.1" },
                { id: "inter", name: "Inter", license: "SIL Open Font License 1.1" },
                { id: "jetbrains-mono", name: "JetBrains Mono", license: "SIL Open Font License 1.1" },
                { id: "lucide", name: "Lucide icons", license: "ISC License" }
            ]
            delegate: Column {
                id: license
                required property var modelData
                property bool expanded: false
                width: column.width
                spacing: Theme.space.xs
                Row {
                    spacing: Theme.space.sm
                    IconButton {
                        iconName: license.expanded ? "chevron-down" : "chevron-right"
                        iconSize: Theme.space.iconSizeSmall
                        implicitWidth: Theme.space.controlHeightSmall
                        implicitHeight: Theme.space.controlHeightSmall
                        text: license.expanded ? qsTr("Hide license") : qsTr("Show license")
                        onClicked: license.expanded = !license.expanded
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: license.modelData.name + " · " + license.modelData.license
                        font: Theme.font.body
                        color: Theme.colors.textPrimary
                    }
                }
                Loader {
                    active: license.expanded
                    width: parent.width
                    sourceComponent: Text {
                        width: column.width
                        text: App.licenseText(license.modelData.id)
                        font: Theme.font.monoSmall
                        color: Theme.colors.textSecondary
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
