import QtQuick
import Noctara
import "../../components"

// Per-host results of "Test DNS" / "Test connection": [{host, ok, detail, via, latency}].
Column {
    id: root

    property var results: []

    spacing: Theme.space.sm
    visible: results.length > 0

    Repeater {
        model: root.results
        delegate: Row {
            id: result
            required property var modelData
            width: root.width
            spacing: Theme.space.sm
            Icon {
                name: result.modelData.ok ? "check" : "circle-alert"
                size: Theme.space.iconSizeSmall
                color: result.modelData.ok ? Theme.targetColors.success : Theme.targetColors.danger
            }
            Column {
                width: result.width - Theme.space.iconSizeSmall - result.spacing
                spacing: Theme.space.xxs
                Text {
                    width: parent.width
                    text: result.modelData.host
                    font: Theme.font.mono
                    color: Theme.colors.textPrimary
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: result.modelData.detail
                    font: Theme.font.caption
                    color: result.modelData.ok ? Theme.colors.textSecondary : Theme.colors.danger
                    wrapMode: Text.Wrap
                }
                Text {
                    width: parent.width
                    visible: result.modelData.ok
                    //: %1 = how the answer was obtained (server, cache …), %2 = duration
                    text: qsTr("via %1 · %2").arg(result.modelData.via || "").arg(result.modelData.latency || "")
                    font: Theme.font.caption
                    color: Theme.colors.textTertiary
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
