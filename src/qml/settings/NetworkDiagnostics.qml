import QtQuick
import Noctara
import "../components"

// Bottom of the Network page: the latest requests and a redacted report for bug reports.
Column {
    id: root

    spacing: Theme.space.sm
    topPadding: Theme.space.xl

    Item {
        width: root.width
        height: Math.max(titles.implicitHeight, copy.implicitHeight)
        Column {
            id: titles
            anchors.left: parent.left
            anchors.right: copy.left
            anchors.rightMargin: Theme.space.md
            spacing: Theme.space.xxs
            Text {
                width: parent.width
                text: qsTr("Diagnostics")
                font: Theme.font.groupTitle
                color: Theme.colors.textTertiary
                wrapMode: Text.Wrap
                Accessible.role: Accessible.Heading
            }
            Text {
                width: parent.width
                text: qsTr("Recent requests. Query values are hidden; passwords and tokens are never recorded.")
                font: Theme.font.caption
                color: Theme.colors.textTertiary
                wrapMode: Text.Wrap
            }
        }
        Button {
            id: copy
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            small: true
            iconName: "copy"
            text: qsTr("Copy report")
            onClicked: Network.copyReport()
        }
    }

    Text {
        visible: Network.recentRequests.length === 0
        text: qsTr("No requests yet.")
        font: Theme.font.caption
        color: Theme.colors.textSecondary
    }
    Repeater {
        model: Network.recentRequests
        delegate: Column {
            id: entry
            required property var modelData
            width: root.width
            spacing: Theme.space.xxs
            Text {
                width: parent.width
                text: entry.modelData.time + "  " + entry.modelData.method + " " + entry.modelData.url
                font: Theme.font.monoSmall
                color: Theme.colors.textPrimary
                elide: Text.ElideMiddle
            }
            Text {
                width: parent.width
                //: %1 = status or error, %2 = duration, %3 = "direct" or the proxy
                text: qsTr("%1 · %2 · %3").arg(entry.modelData.result).arg(entry.modelData.latency).arg(entry.modelData.via)
                font: Theme.font.caption
                color: entry.modelData.ok ? Theme.colors.textTertiary : Theme.colors.danger
                wrapMode: Text.Wrap
            }
        }
    }
}
