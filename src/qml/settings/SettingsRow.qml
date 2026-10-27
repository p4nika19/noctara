import QtQuick
import Noctara
import "../components"

// One generated settings row (delegate of SettingsModel), styled after the appearance drawer of the design
// reference: toggles sit inline; segmented choices, sliders and pickers span the row under a label line
// that carries the current value. Long labels wrap, so translations never overflow.
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
    // Drawer mode: tighter spacing, descriptions only for toggles, no per-section reset.
    property bool compact: false
    // Section whose header is already drawn above the list (the Appearance page shows "Theme" over its tiles).
    property string headerShownAbove

    readonly property bool editable: !overridden && disabledReason.length === 0
    readonly property bool segmented: type === "enum" && options.length <= 4
                                      && options.reduce((n, o) => n + o.label.length, 0) <= (compact ? 30 : 46)
    readonly property bool inline: type === "bool" || (type === "enum" && !segmented)
    readonly property string valueText: {
        if (type !== "int" && type !== "real")
            return ""
        const v = Number(value)
        if (unit === "%")
            return Math.round(v) + " %"
        if (minimum === 0 && maximum === 1)
            return Math.round(v * 100) + " %"
        const text = type === "int" ? Math.round(v).toString() : v.toLocaleString(Qt.locale(Language.localeName), 'f', 2)
        return unit.length > 0 ? text + " " + unit : text
    }

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

        // Group header: mono uppercase title, as in the reference's drawer.
        Item {
            visible: row.sectionStart && row.section !== row.headerShownAbove
            width: parent.width
            height: visible ? header.implicitHeight + (row.compact ? Theme.space.lg : Theme.space.xl) : 0
            Column {
                id: header
                anchors.left: parent.left
                anchors.right: resetSection.visible ? resetSection.left : parent.right
                anchors.rightMargin: Theme.space.md
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.space.xs
                spacing: Theme.space.xxs
                Text {
                    width: parent.width
                    text: row.showCategory ? row.categoryLabel + " · " + row.sectionLabel : row.sectionLabel
                    font: Theme.font.groupTitle
                    color: Theme.colors.textTertiary
                    wrapMode: Text.Wrap
                    Accessible.role: Accessible.Heading
                }
                Text {
                    width: parent.width
                    visible: !row.compact && text.length > 0
                    text: row.sectionDescription
                    font: Theme.font.caption
                    color: Theme.colors.textTertiary
                    wrapMode: Text.Wrap
                }
            }
            Button {
                id: resetSection
                visible: !row.compact && !row.showCategory
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.space.xs
                small: true
                text: qsTr("Reset section")
                onClicked: SettingsUi.resetSection(row.section)
            }
        }

        Item {
            id: body
            readonly property real padding: row.compact ? Theme.space.sm : Theme.space.md
            width: parent.width
            height: (row.inline ? Math.max(texts.height, control.height)
                                : texts.height + Theme.space.sm + control.height) + padding * 2

            Column {
                id: texts
                y: row.inline ? (body.height - height) / 2 : body.padding
                width: row.inline ? body.width - control.width - Theme.space.xl : body.width
                spacing: Theme.space.xxs

                Item {
                    width: parent.width
                    height: Math.max(labelFlow.height, valueLabel.implicitHeight)
                    Flow {
                        id: labelFlow
                        width: parent.width - (valueLabel.visible ? valueLabel.implicitWidth + Theme.space.md : 0)
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
                            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.14)
                            border.width: Theme.space.hairline
                            border.color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.42)
                            Text {
                                id: restartText
                                anchors.centerIn: parent
                                text: qsTr("Restart required")
                                font: Theme.font.caption
                                color: Theme.colors.accentText
                            }
                        }
                        IconButton {
                            visible: !row.isDefault && !row.overridden
                            iconName: "rotate-ccw"
                            iconSize: Theme.space.iconSizeSmall - 2
                            implicitWidth: Theme.space.iconSizeSmall + Theme.space.sm
                            implicitHeight: Theme.space.iconSizeSmall + Theme.space.sm
                            iconColor: Theme.targetColors.iconSecondary
                            text: qsTr("Reset to default")
                            onClicked: SettingsUi.reset(row.key)
                        }
                    }
                    Text {
                        id: valueLabel
                        visible: row.valueText.length > 0
                        anchors.right: parent.right
                        anchors.top: parent.top
                        text: row.valueText
                        font: Theme.font.mono
                        color: Theme.colors.accentText
                    }
                }
                Text {
                    width: parent.width
                    visible: text.length > 0 && (!row.compact || row.type === "bool")
                    text: row.description
                    font: Theme.font.caption
                    color: Theme.colors.textTertiary
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

            Loader {
                id: control
                x: row.inline ? body.width - width : 0
                y: row.inline ? (body.height - height) / 2 : texts.y + texts.height + Theme.space.sm
                width: row.inline ? implicitWidth : body.width
                enabled: row.editable
                opacity: row.editable ? 1 : 0.5
                sourceComponent: {
                    switch (row.type) {
                    case "bool": return boolControl
                    case "enum": return row.segmented ? segmentedControl : comboControl
                    case "int":
                    case "real": return numberControl
                    case "color": return colorControl
                    case "list": return listControl
                    default: return textControl
                    }
                }
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
        Slider {
            from: row.minimum
            to: row.maximum
            stepSize: row.step
            snapMode: Slider.SnapAlways
            value: row.value
            Accessible.name: row.label
            onMoved: row.commit(value)
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
