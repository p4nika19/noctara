import QtQuick
import Noctara
import "../components"

// Top of the Performance page: what is rendering, what the glass effects cost right now and why they may be
// reduced (quality governor, energy saving, software rendering). Updates once per second while frames are drawn.
Column {
    id: root

    spacing: Theme.space.sm
    bottomPadding: Theme.space.md

    readonly property var locale: Qt.locale(Language.localeName)

    function styleName(style) {
        switch (style) {
        case "kawase": return qsTr("Dual Kawase blur")
        case "gaussian": return qsTr("Gaussian blur")
        case "liquid": return qsTr("Liquid Glass")
        default: return qsTr("Solid (no blur)")
        }
    }
    function qualityName(quality) {
        return quality === 0 ? qsTr("low quality") : quality === 1 ? qsTr("medium quality") : qsTr("high quality")
    }
    function number(value, decimals) { return value.toLocaleString(root.locale, "f", decimals) }

    component InfoRow: Row {
        property string name
        property string info
        property color infoColor: Theme.colors.textPrimary
        width: root.width
        spacing: Theme.space.md
        Text {
            width: Math.min(200 * Theme.scale, parent.width * 0.35)
            text: parent.name
            font: Theme.font.label
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }
        Text {
            width: parent.width - Math.min(200 * Theme.scale, parent.width * 0.35) - parent.spacing
            text: parent.info
            font: Theme.font.mono
            color: parent.infoColor
            wrapMode: Text.Wrap
        }
    }

    Text {
        topPadding: Theme.space.lg
        bottomPadding: Theme.space.xs
        text: qsTr("Right now")
        font: Theme.font.groupTitle
        color: Theme.colors.textTertiary
        Accessible.role: Accessible.Heading
    }
    InfoRow {
        name: qsTr("Renderer")
        info: Graphics.hardware ? Graphics.api + (Graphics.deviceName.length > 0 ? " · " + Graphics.deviceName : "")
                                : qsTr("Software (CPU)")
    }
    InfoRow {
        name: qsTr("Glass")
        info: Theme.glass.blurred ? root.styleName(Theme.glass.style) + " · " + root.qualityName(Theme.glass.quality)
                                  : root.styleName(Theme.glass.style)
    }
    InfoRow {
        name: qsTr("Frame rate")
        info: Performance.fps >= 1
              //: %1 = frames per second
              ? qsTr("%1 fps").arg(root.number(Performance.fps, 0))
              : qsTr("Idle: nothing is being redrawn")
    }
    InfoRow {
        visible: Graphics.hardware
        name: qsTr("GPU time per frame")
        info: Performance.gpuFrameMs >= 0
              //: %1 = measured GPU time, %2 = time available per frame, both in milliseconds
              ? qsTr("%1 ms of %2 ms").arg(root.number(Performance.gpuFrameMs, 2)).arg(root.number(Performance.frameBudgetMs, 1))
              : qsTr("Not reported yet")
    }
    InfoRow {
        visible: Performance.frameCap > 0
        name: qsTr("Frame rate limit")
        info: qsTr("%1 fps").arg(Performance.frameCap)
    }

    Text {
        width: root.width
        topPadding: Theme.space.xs
        visible: text.length > 0
        text: !Graphics.hardware
              ? qsTr("Hardware acceleration is off, so glass is drawn as solid surfaces and the backdrop does not move.")
              : Performance.energySaving
                ? qsTr("Energy saving is active: the backdrop is still, blur is limited to Dual Kawase at medium quality and animations run at up to 30 fps.")
                : Performance.governed
                  ? qsTr("Effects were lowered automatically because frames took too long. Quality is raised again step by step when there is headroom.")
                  : ""
        font: Theme.font.caption
        color: Theme.colors.textSecondary
        wrapMode: Text.Wrap
    }
    Button {
        visible: Performance.governed && !Performance.energySaving
        variant: "secondary"
        iconName: "refresh-cw"
        text: qsTr("Restore full quality")
        onClicked: Performance.resetGovernor()
    }
}
