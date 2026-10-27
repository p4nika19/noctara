import QtQuick
import Noctara

// Static accent seed: preset swatches plus a validated hex input. Emits picked("#rrggbb").
Column {
    id: root

    property string value: "#4a7dff"
    signal picked(string hex)

    readonly property var presets: ["#4a7dff", "#7c6cf0", "#0fa3b1", "#22a55e", "#e0a100", "#f26b1d", "#e5484d", "#d6409f"]

    spacing: Theme.space.sm

    Row {
        spacing: Theme.space.sm
        Repeater {
            model: root.presets
            delegate: Rectangle {
                id: swatch
                required property string modelData
                readonly property bool selected: root.value.toLowerCase() === modelData
                width: 28 * Theme.scale
                height: width
                radius: width / 2
                color: modelData
                border.width: selected ? 3 * Theme.scale : Theme.space.hairline
                border.color: selected ? Theme.colors.textPrimary : Theme.colors.outline
                scale: area.pressed ? 0.88 : (area.containsMouse ? 1.08 : 1)
                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checked: selected
                Keys.onSpacePressed: root.picked(modelData)
                Keys.onReturnPressed: root.picked(modelData)
                Behavior on scale { NumberAnimation { duration: Theme.motion.fast; easing.type: Theme.motion.easingEmphasized } }
                FocusRing { target: swatch; targetRadius: swatch.radius }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.picked(swatch.modelData)
                }
            }
        }
    }
    Row {
        spacing: Theme.space.sm
        Rectangle {
            width: Theme.space.controlHeight
            height: width
            radius: Theme.radius.sm
            color: hex.acceptableInput ? hex.text : root.value
            border.width: Theme.space.hairline
            border.color: Theme.colors.outline
        }
        TextField {
            id: hex
            implicitWidth: 140 * Theme.scale
            monospace: true
            text: root.value
            placeholderText: qsTr("Hex color")
            validator: RegularExpressionValidator { regularExpression: /#[0-9a-fA-F]{6}/ }
            onEditingFinished: if (acceptableInput) root.picked(text.toLowerCase())
        }
    }
}
