import QtQuick
import Noctara
import "../../components"

// Under the DNS mode row: check the current DNS settings against Noctara's real hosts.
Column {
    spacing: Theme.space.sm

    Flow {
        width: parent.width
        spacing: Theme.space.sm
        Button {
            small: true
            iconName: "search"
            text: Network.dnsTesting ? qsTr("Testing…") : qsTr("Test DNS")
            enabled: !Network.dnsTesting
            onClicked: Network.testDns()
        }
        Button {
            small: true
            iconName: "refresh-cw"
            text: qsTr("Flush DNS cache")
            onClicked: Network.flushDnsCache()
        }
    }
    TestResults {
        width: parent.width
        results: Network.dnsResults
    }
}
