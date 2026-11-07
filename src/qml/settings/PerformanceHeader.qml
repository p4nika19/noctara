import QtQuick
import Noctara
import "../components"
import "network"

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
    function qualityName(quality) {
        return quality === 0 ? qsTr("low quality") : quality === 1 ? qsTr("medium quality") : qsTr("high quality")
    }
    function number(value, decimals) { return value.toLocaleString(root.locale, "f", decimals) }

    Text {
        topPadding: Theme.space.lg
        bottomPadding: Theme.space.xs
        text: qsTr("Right now")
        font: Theme.font.groupTitle
        color: Theme.colors.textTertiary
        Accessible.role: Accessible.Heading
    }
    InfoRow {
        width: root.width
        name: qsTr("Renderer")
        info: Graphics.hardware ? Graphics.api + (Graphics.deviceName.length > 0 ? " · " + Graphics.deviceName : "")
                                : qsTr("Software (CPU)")
    }
    InfoRow {
        width: root.width
        name: qsTr("Glass")
        info: [root.styleName(Theme.glass.style), root.surfaceName(Theme.glass.surface)]
              .concat(Theme.glass.blurred ? [root.qualityName(Theme.glass.quality)] : []).join(" · ")
    }
    InfoRow {
        width: root.width
        name: qsTr("Frame rate")
        info: Performance.fps >= 1
              //: %1 = frames per second
              ? qsTr("%1 fps").arg(root.number(Performance.fps, 0))
              : qsTr("Idle: nothing is being redrawn")
    }
    InfoRow {
        width: root.width
        visible: Graphics.hardware
        name: qsTr("GPU time per frame")
        info: Performance.gpuFrameMs >= 0
              //: %1 = measured GPU time, %2 = time available per frame, both in milliseconds
              ? qsTr("%1 ms of %2 ms").arg(root.number(Performance.gpuFrameMs, 2)).arg(root.number(Performance.frameBudgetMs, 1))
              : qsTr("Not reported yet")
    }
    InfoRow {
        width: root.width
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
