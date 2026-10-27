import QtQuick
import Noctara
import "../components"
import "network"

// Top of the Network page: the connection, the DNS and proxy in effect, and how DNS and the proxy interact.
Column {
    id: root

    spacing: Theme.space.sm
    bottomPadding: Theme.space.md

    function connectivityText(state) {
        switch (state) {
        case "full": return qsTr("Online")
        case "limited": return qsTr("Limited: the internet may not be reachable")
        case "portal": return qsTr("The network asks you to sign in first")
        case "none": return qsTr("No network connection")
        default: return qsTr("Unknown")
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
        width: root.width
        name: qsTr("Connection")
        info: Network.offline ? qsTr("Offline mode: Noctara makes no connections")
                              : root.connectivityText(Network.connectivity)
                                + (Network.metered ? " · " + qsTr("metered") : "")
        infoColor: Network.offline || Network.connectivity === "none" ? Theme.colors.warning : Theme.colors.textPrimary
    }
    InfoRow {
        width: root.width
        name: qsTr("DNS")
        info: Network.dnsSummary
    }
    InfoRow {
        width: root.width
        name: qsTr("Proxy")
        info: Network.proxySummary
    }
    Row {
        width: root.width
        visible: Network.dnsProxyHint.length > 0
        topPadding: Theme.space.xs
        spacing: Theme.space.xs
        Icon {
            name: "info"
            size: Theme.space.iconSizeSmall
            color: Theme.targetColors.accent
        }
        Text {
            width: parent.width - Theme.space.iconSizeSmall - parent.spacing
            text: Network.dnsProxyHint
            font: Theme.font.caption
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }
    }
}
