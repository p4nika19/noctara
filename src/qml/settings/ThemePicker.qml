import QtQuick
import QtQuick.Dialogs
import Noctara
import "../components"

// Picks the custom theme file (appearance.theme.custom), and imports, exports or opens the themes folder. Problems
// in the selected file are listed: unknown tokens and invalid values are skipped, a syntax error ignores the file.
Column {
    id: root

    width: parent ? parent.width : implicitWidth
    spacing: Theme.space.sm

    Row {
        spacing: Theme.space.sm
        ComboBox {
            anchors.verticalCenter: parent.verticalCenter
            model: [{ id: "", label: qsTr("None (built-in)") }].concat(
                       Themes.themes.map(t => ({ id: t.id, label: t.usable ? t.name : qsTr("%1 (unusable)").arg(t.name),
                                                 enabled: t.usable })))
            currentId: Themes.selected
            Accessible.name: qsTr("Custom theme")
            onPicked: id => Themes.select(id)
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "plus"
            text: qsTr("Import a theme file")
            onClicked: importDialog.open()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "copy"
            enabled: Themes.selected.length > 0
            text: qsTr("Export the selected theme")
            onClicked: exportDialog.open()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            iconName: "external-link"
            text: qsTr("Open the themes folder")
            onClicked: Themes.openFolder()
        }
    }
    Text {
        width: parent.width
        visible: Themes.themes.length === 0
        text: qsTr("No theme files yet. Put .toml files into the themes folder or import one; see docs/themes.md "
                   + "for the format and examples.")
        font: Theme.font.caption
        color: Theme.colors.textTertiary
        wrapMode: Text.Wrap
    }
    Repeater {
        model: Themes.selectedIssues
        delegate: Text {
            required property string modelData
            width: root.width
            text: modelData
            font: Theme.font.caption
            color: Theme.colors.warning
            wrapMode: Text.Wrap
        }
    }
    Text {
        width: parent.width
        visible: Themes.selected.length > 0 && !Theme.contrastOk
        text: qsTr("Some colors of this theme fall below WCAG AA contrast. Turn on “Enforce readable contrast” to "
                   + "correct them.")
        font: Theme.font.caption
        color: Theme.colors.warning
        wrapMode: Text.Wrap
    }

    FileDialog {
        id: importDialog
        title: qsTr("Import a theme")
        nameFilters: [qsTr("Theme files (*.toml)")]
        onAccepted: Themes.importFile(selectedFile)
    }
    FileDialog {
        id: exportDialog
        title: qsTr("Export the theme")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "toml"
        nameFilters: [qsTr("Theme files (*.toml)")]
        onAccepted: Themes.exportTheme(Themes.selected, selectedFile)
    }
}
