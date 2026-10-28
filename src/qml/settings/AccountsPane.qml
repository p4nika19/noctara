import QtQuick
import Noctara
import "../components"

// Accounts page body: consented browser import and manual token sign-in share one verified session path.
// Secrets are persisted only through the configured secret store, never in the config file.
Flickable {
    id: root

    readonly property int state: Account.state
    readonly property bool signedIn: state === AccountController.SignedIn
    readonly property bool expired: state === AccountController.Expired

    contentWidth: width
    contentHeight: column.implicitHeight
    boundsBehavior: Flickable.StopAtBounds
    clip: true
    ScrollBar.vertical: ScrollBar {}

    Column {
        id: column
        width: root.width - Theme.space.md
        spacing: Theme.space.lg
        topPadding: Theme.space.lg
        bottomPadding: Theme.space.xl

        // --- Signed in -------------------------------------------------------------------------
        GlassPanel {
            width: parent.width
            level: 1
            radius: Theme.radius.lg
            visible: root.signedIn
            height: visible ? profile.implicitHeight + Theme.space.lg * 2 : 0

            Row {
                id: profile
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Math.max(Theme.space.lg, parent.cornerInset)
                anchors.leftMargin: Math.max(Theme.space.xl, parent.cornerInset)
                spacing: Theme.space.lg

                CoverArt {
                    width: 64 * Theme.scale
                    height: width
                    shape: "circle"
                    source: Account.avatarUrl
                    anchors.verticalCenter: parent.verticalCenter
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - 64 * Theme.scale - signOut.width - parent.spacing * 2
                    spacing: Theme.space.xxs
                    Text {
                        width: parent.width
                        text: Account.displayName.length > 0 ? Account.displayName : qsTr("Signed in")
                        font: Theme.font.title
                        color: Theme.colors.textPrimary
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        visible: Account.username.length > 0
                        //: Shown under the account name; %1 is the SoundCloud username
                        text: qsTr("@%1").arg(Account.username)
                        font: Theme.font.body
                        color: Theme.colors.textSecondary
                        elide: Text.ElideRight
                    }
                    Text {
                        width: parent.width
                        visible: Account.permalinkUrl.length > 0
                        text: qsTr("View profile on SoundCloud")
                        font: Theme.font.caption
                        color: Theme.colors.accent
                        elide: Text.ElideRight
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: Qt.openUrlExternally(Account.permalinkUrl)
                        }
                    }
                }
                Button {
                    id: signOut
                    anchors.verticalCenter: parent.verticalCenter
                    variant: "ghost"
                    iconName: "log-out"
                    text: qsTr("Sign out")
                    onClicked: Account.signOut()
                }
            }
        }

        // --- Session expired warning -----------------------------------------------------------
        GlassPanel {
            width: parent.width
            level: 2
            radius: Theme.radius.md
            visible: root.expired
            height: visible ? expiredRow.implicitHeight + Theme.space.md * 2 : 0

            Row {
                id: expiredRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: Math.max(Theme.space.md, parent.cornerInset)
                spacing: Theme.space.sm
                Icon {
                    name: "triangle-alert"
                    size: Theme.space.iconSize
                    color: Theme.targetColors.warning
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    width: parent.width - Theme.space.iconSize - parent.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Your session has expired. Sign in again below to keep listening to your library and likes.")
                    font: Theme.font.body
                    color: Theme.colors.textPrimary
                    wrapMode: Text.Wrap
                }
            }
        }

        // --- Sign in ---------------------------------------------------------------------------
        Column {
            id: signInColumn
            width: parent.width
            spacing: Theme.space.sm
            visible: !root.signedIn
            property string selectedProfile

            Text {
                width: parent.width
                text: qsTr("Sign in to SoundCloud")
                font: Theme.font.groupTitle
                color: Theme.colors.textTertiary
                Accessible.role: Accessible.Heading
            }
            Text {
                width: parent.width
                text: qsTr("Import your existing SoundCloud session from a browser, or paste an OAuth token. Noctara verifies every token with SoundCloud before saving it, and never stores it in the configuration file.")
                font: Theme.font.body
                color: Theme.colors.textSecondary
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                visible: !Account.persistent
                text: qsTr("No persistent secret store is available, so a saved session is kept only until Noctara quits.")
                font: Theme.font.caption
                color: Theme.colors.warning
                wrapMode: Text.Wrap
            }
            Text {
                width: parent.width
                visible: Account.persistent && !Account.keyringAvailable
                text: qsTr("No desktop keyring is available. Your session will use Noctara's encrypted local store; its owner-only key file does not provide the same protection as a desktop keyring.")
                font: Theme.font.caption
                color: Theme.colors.warning
                wrapMode: Text.Wrap
            }

            Item { width: 1; height: Theme.space.xs }

            Text {
                width: parent.width
                text: qsTr("Sign in from your browser")
                font: Theme.font.bodyStrong
                color: Theme.colors.textPrimary
            }
            Text {
                width: parent.width
                text: qsTr("Scanning only lists installed profiles. Noctara reads the selected profile's SoundCloud oauth_token cookie only after you confirm.")
                font: Theme.font.caption
                color: Theme.colors.textSecondary
                wrapMode: Text.Wrap
            }
            Row {
                width: parent.width
                spacing: Theme.space.sm
                Button {
                    text: Account.browserBusy ? qsTr("Working…") : qsTr("Find browser profiles")
                    enabled: !Account.browserBusy && !Account.busy
                    onClicked: Account.discoverBrowsers()
                }
                Button {
                    visible: Account.browserProfiles.length > 0
                    variant: "primary"
                    text: qsTr("Import session…")
                    enabled: !Account.browserBusy && !Account.busy && signInColumn.selectedProfile.length > 0
                    onClicked: {
                        for (const entry of Account.browserProfiles) {
                            if (entry.id === signInColumn.selectedProfile) {
                                consent.profileId = entry.id
                                consent.browserName = entry.browser
                                consent.profileName = entry.profile
                                consent.open()
                                break
                            }
                        }
                    }
                }
            }
            ComboBox {
                width: Math.min(parent.width, 480 * Theme.scale)
                visible: Account.browserProfiles.length > 0
                model: Account.browserProfiles
                currentId: signInColumn.selectedProfile
                enabled: !Account.browserBusy
                onPicked: id => signInColumn.selectedProfile = id
            }

            Item { width: 1; height: Theme.space.xs }

            Text {
                width: parent.width
                text: qsTr("Or paste an OAuth token")
                font: Theme.font.bodyStrong
                color: Theme.colors.textPrimary
            }
            TextField {
                id: tokenField
                width: parent.width
                iconName: "lock"
                echoMode: TextInput.Password
                placeholderText: qsTr("OAuth token")
                enabled: !Account.busy && !Account.browserBusy
                onAccepted: signIn.clicked()
            }
            Button {
                id: signIn
                variant: "primary"
                text: Account.busy ? qsTr("Checking…") : qsTr("Sign in")
                enabled: !Account.busy && !Account.browserBusy && tokenField.text.trim().length > 0
                onClicked: {
                    Account.signInWithToken(tokenField.text)
                    tokenField.text = ""
                }
            }

            Dialog {
                id: consent
                property string profileId
                property string browserName
                property string profileName
                title: qsTr("Allow browser cookie access?")
                message: qsTr("Noctara will make a temporary read-only copy of the cookie database for %1 (%2), read only the SoundCloud oauth_token cookie, and delete the copy. The token will be verified with SoundCloud and saved in your secret store. No other cookies are read.").arg(browserName).arg(profileName)
                confirmText: qsTr("Read cookie and sign in")
                onAccepted: Account.importBrowserToken(profileId)
            }
        }
    }
}
