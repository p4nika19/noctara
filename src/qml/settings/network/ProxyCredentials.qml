import QtQuick
import Noctara
import "../../components"

// Under the proxy port row: credentials for the manual proxy, kept in the system keyring only.
Column {
    id: root

    spacing: Theme.space.sm

    Text {
        width: parent.width
        text: qsTr("Username and password")
        font: Theme.font.bodyStrong
        color: Theme.colors.textPrimary
        wrapMode: Text.Wrap
    }
    Text {
        width: parent.width
        text: Network.keyringAvailable
              ? qsTr("Optional. Saved in the system keyring, never in the configuration file.")
              : qsTr("No keyring is available, so saved credentials are kept only until Noctara quits.")
        font: Theme.font.caption
        color: Network.keyringAvailable ? Theme.colors.textTertiary : Theme.colors.warning
        wrapMode: Text.Wrap
    }
    Flow {
        width: parent.width
        spacing: Theme.space.sm
        TextField {
            id: username
            width: Math.min(implicitWidth, root.width)
            iconName: "user"
            placeholderText: qsTr("Username")
            text: Network.proxyUsername
        }
        TextField {
            id: password
            width: Math.min(implicitWidth, root.width)
            iconName: "lock"
            echoMode: TextInput.Password
            placeholderText: Network.hasProxyCredentials ? qsTr("Saved – type to replace") : qsTr("Password")
            onAccepted: save.clicked()
        }
    }
    Flow {
        width: parent.width
        spacing: Theme.space.sm
        Button {
            id: save
            small: true
            text: qsTr("Save credentials")
            enabled: password.text.length > 0
            onClicked: {
                Network.saveProxyCredentials(username.text, password.text)
                password.text = ""
            }
        }
        Button {
            small: true
            variant: "ghost"
            visible: Network.hasProxyCredentials
            text: qsTr("Forget credentials")
            onClicked: Network.forgetProxyCredentials()
        }
    }
}
