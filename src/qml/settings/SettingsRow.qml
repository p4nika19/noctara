import QtQuick
import Noctara
import "../components"

// One generated settings row (delegate of SettingsModel). The control is chosen from the schema type;
// long labels wrap, and wide controls move below the text so translations never overflow.
Item {
    id: row

    required property string key
    required property string type
    required property string label
    required property string description
    required property var value
    required property bool isDefault
    required property var options
    required property real minimum
    required property real maximum
    required property real step
    required property string unit
    required property bool requiresRestart
    required property bool overridden
    required property string overrideHint
    required property bool shown
    required property string disabledReason
    required property string section
    required property string sectionLabel
    required property string sectionDescription
    required property string categoryLabel
    required property bool sectionStart
    required property string status
    property bool showCategory: false

    readonly property bool editable: !overridden && disabledReason.length === 0
    readonly property bool stacked: type === "color" || control.implicitWidth > width * 0.45

    width: ListView.view ? ListView.view.width : implicitWidth
    height: shown ? layout.height : 0
    visible: shown
    Accessible.role: Accessible.Grouping
    Accessible.name: label

    function commit(newValue) {
        const error = SettingsUi.setValue(row.key, newValue)
        if (error.length > 0)
            App.showToast("warning", error, "", "")
    }

    Column {
        id: layout
        width: parent.width

        Item {
            visible: row.sectionStart
            width: parent.width
            height: visible ? header.implicitHeight + Theme.space.xl : 0
            Column {
                id: header
                anchors.left: parent.left
                anchors.right: resetSection.left
                anchors.rightMargin: Theme.space.md
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.space.sm
                spacing: Theme.space.xxs
                Text {
                    width: parent.width
                    text: row.showCategory ? row.categoryLabel + " › " + row.sectionLabel : row.sectionLabel
                    font: Theme.font.subtitle
                    color: Theme.colors.textPrimary
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.Heading
                }
                Text {
                    width: parent.width
                    text: row.sectionDescription
                    font: Theme.font.caption
                    color: Theme.colors.textSecondary
                    wrapMode: Text.Wrap
                }
            }
            Button {
                id: resetSection
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.space.xs
                variant: "ghost"
                text: qsTr("Reset section")
                onClicked: SettingsUi.resetSection(row.section)
            }
        }

        Item {
            id: body
            width: parent.width
            height: (row.stacked ? texts.height + Theme.space.sm + controlArea.height
                                 : Math.max(texts.height, controlArea.height)) + Theme.space.md * 2

            Column {
                id: texts
                y: row.stacked ? Theme.space.md : (body.height - height) / 2
                width: row.stacked ? body.width : body.width - controlArea.width - Theme.space.xl
                spacing: Theme.space.xxs

                Flow {
                    width: parent.width
                    spacing: Theme.space.sm
                    Text {
                        width: Math.min(implicitWidth, parent.width)
                        text: row.label
                        font: Theme.font.bodyStrong
                        color: Theme.colors.textPrimary
                        wrapMode: Text.Wrap
                    }
                    Rectangle {
                        visible: row.requiresRestart
                        width: restartText.implicitWidth + Theme.space.md
                        height: restartText.implicitHeight + Theme.space.xxs * 2
                        radius: height / 2
                        color: Theme.colors.accentContainer
                        Text {
                            id: restartText
                            anchors.centerIn: parent
                            text: qsTr("Restart required")
                            font: Theme.font.caption
                            color: Theme.colors.onAccentContainer
                        }
                    }
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: row.description
                    font: Theme.font.caption
                    color: Theme.colors.textSecondary
                    wrapMode: Text.Wrap
                }
                Row {
                    width: parent.width
                    visible: row.status.length > 0
                    topPadding: Theme.space.xxs
                    spacing: Theme.space.xs
                    Icon {
                        name: "info"
                        size: Theme.space.iconSizeSmall
                        color: Theme.targetColors.accent
                    }
                    Text {
                        width: parent.width - Theme.space.iconSizeSmall - parent.spacing
                        text: row.status
                        font: Theme.font.caption
                        color: Theme.colors.textPrimary
                        wrapMode: Text.Wrap
                    }
                }
                Text {
                    width: parent.width
                    visible: text.length > 0
                    text: row.overridden ? row.overrideHint : row.disabledReason
                    font: Theme.font.caption
                    color: Theme.colors.warning
                    wrapMode: Text.Wrap
                }
            }

            Row {
                id: controlArea
                x: row.stacked ? 0 : body.width - width
                y: row.stacked ? texts.y + texts.height + Theme.space.sm : (body.height - height) / 2
                spacing: Theme.space.sm
                layoutDirection: row.stacked ? Qt.LeftToRight : Qt.RightToLeft

                Loader {
                    id: control
                    enabled: row.editable
                    opacity: row.editable ? 1 : 0.5
                    sourceComponent: {
                        switch (row.type) {
                        case "bool": return boolControl
                        case "enum": return row.options.reduce((n, o) => n + o.label.length, 0) <= 34 && row.options.length <= 4
                                            ? segmentedControl : comboControl
                        case "int":
                        case "real": return numberControl
                        case "color": return colorControl
                        case "list": return listControl
                        default: return textControl
                        }
                    }
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !row.isDefault && !row.overridden
                    iconName: "rotate-ccw"
                    iconSize: Theme.space.iconSizeSmall
                    implicitWidth: Theme.space.controlHeightSmall
                    implicitHeight: Theme.space.controlHeightSmall
                    text: qsTr("Reset to default")
                    onClicked: SettingsUi.reset(row.key)
                }
            }

            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: Theme.space.hairline
                color: Theme.colors.divider
            }
        }
    }

    Component {
        id: boolControl
        Switch {
            checked: row.value === true
            Accessible.name: row.label
            onToggled: row.commit(checked)
        }
    }
    Component {
        id: segmentedControl
        SegmentedControl {
            model: row.options
            currentId: row.value
            Accessible.name: row.label
            onActivated: id => row.commit(id)
        }
    }
    Component {
        id: comboControl
        ComboBox {
            model: row.options
            currentId: row.value
            Accessible.name: row.label
            onPicked: id => row.commit(id)
        }
    }
    Component {
        id: numberControl
        Row {
            spacing: Theme.space.md
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: 64 * Theme.scale
                horizontalAlignment: Text.AlignRight
                font: Theme.font.mono
                color: Theme.colors.textSecondary
                text: {
                    const v = slider.value
                    if (row.unit === "%")
                        return Math.round(v) + " %"
                    if (row.minimum === 0 && row.maximum === 1)
                        return Math.round(v * 100) + " %"
                    const text = row.type === "int" ? Math.round(v).toString()
                                                    : Number(v).toLocaleString(Qt.locale(Language.localeName), 'f', 2)
                    return row.unit.length > 0 ? text + " " + row.unit : text
                }
            }
            Slider {
                id: slider
                anchors.verticalCenter: parent.verticalCenter
                from: row.minimum
                to: row.maximum
                stepSize: row.step
                snapMode: Slider.SnapAlways
                value: row.value
                Accessible.name: row.label
                onMoved: row.commit(value)
            }
        }
    }
    Component {
        id: colorControl
        ColorPicker {
            value: row.value
            onPicked: hex => row.commit(hex)
        }
    }
    Component {
        id: textControl
        TextField {
            text: row.value
            placeholderText: qsTr("Default")
            Accessible.name: row.label
            onEditingFinished: if (text !== row.value) row.commit(text)
        }
    }
    Component {
        id: listControl
        TextField {
            text: (row.value || []).join(", ")
            placeholderText: qsTr("Comma-separated values")
            Accessible.name: row.label
            onEditingFinished: row.commit(text.split(",").map(s => s.trim()).filter(s => s.length > 0))
        }
    }
}
