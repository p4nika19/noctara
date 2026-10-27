import QtQuick
import Noctara
import "../../components"

// Under the proxy mode row: reach the API and media servers through the current proxy and DNS settings.
Column {
    spacing: Theme.space.sm

    Button {
        small: true
        iconName: "globe"
        text: Network.connectionTesting ? qsTr("Testing…") : qsTr("Test connection")
        enabled: !Network.connectionTesting
        onClicked: Network.testConnection()
    }
    TestResults {
        width: parent.width
        results: Network.connectionResults
    }
}
