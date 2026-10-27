import QtQuick
import Noctara

// Static accent seed: preset swatches plus a validated hex input. Emits picked("#rrggbb").
Column {
    id: root

    property string value: "#6774f9"
    signal picked(string hex)

    readonly property var presets: ["#6774f9", "#4a7dff", "#2dc7d4", "#7dd98a", "#ffb454", "#ff6f6f", "#ff6fae", "#b98cff"]

    spacing: Theme.space.md

    Flow {
        width: parent.width
        spacing: Theme.space.md
        Repeater {
            model: root.presets
            delegate: Item {
                id: swatch
                required property string modelData
                readonly property bool selected: root.value.toLowerCase() === modelData
                width: 34 * Theme.scale
                height: width
                activeFocusOnTab: true
                Accessible.role: Accessible.RadioButton
                Accessible.name: modelData
                Accessible.checked: selected
                Keys.onSpacePressed: root.picked(modelData)
                Keys.onReturnPressed: root.picked(modelData)

                // Selected: a dark gap then a light ring, as in the design reference.
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width + 10 * Theme.scale
                    height: width
                    radius: width / 2
                    visible: swatch.selected
                    color: "transparent"
                    border.width: 2 * Theme.scale
                    border.color: Theme.colors.textPrimary
                }
                Rectangle {
                    anchors.fill: parent
                    radius: width / 2
                    color: swatch.modelData
                    border.width: 2 * Theme.scale
                    border.color: Qt.rgba(1, 1, 1, 0.22)
                    scale: area.pressed ? 0.9 : (area.containsMouse ? 1.06 : 1)
                    Behavior on scale { NumberAnimation { duration: Theme.motion.fast; easing.type: Theme.motion.easingEmphasized } }
                }
                FocusRing { target: swatch; targetRadius: swatch.width / 2 }
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
            radius: width / 2
            color: hex.acceptableInput ? hex.text : root.value
            border.width: 2 * Theme.scale
            border.color: Qt.rgba(1, 1, 1, 0.22)
        }
        TextField {
            id: hex
            implicitWidth: 150 * Theme.scale
            monospace: true
            text: root.value
            placeholderText: qsTr("Hex color")
            validator: RegularExpressionValidator { regularExpression: /#[0-9a-fA-F]{6}/ }
            onEditingFinished: if (acceptableInput) root.picked(text.toLowerCase())
        }
    }
}
