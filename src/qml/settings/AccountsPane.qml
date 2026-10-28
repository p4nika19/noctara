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
    readonly property real avatarSize: 64 * Theme.scale

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
            id: accountCard
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
                // `parent` is the panel's inner content item, which has no cornerInset: name the panel.
                anchors.margins: Math.max(Theme.space.lg, accountCard.cornerInset)
                anchors.leftMargin: Math.max(Theme.space.xl, accountCard.cornerInset)
                anchors.rightMargin: Math.max(Theme.space.xl, accountCard.cornerInset)
                spacing: Theme.space.lg

                CoverArt {
                    width: root.avatarSize
                    height: width
                    shape: "circle"
                    source: Account.avatarUrl
                    anchors.verticalCenter: parent.verticalCenter
                }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - root.avatarSize - signOut.width - parent.spacing * 2
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
                    // How the saved session is protected, also after a restart (not only in the sign-in toast).
                    Row {
                        width: parent.width
                        spacing: Theme.space.xs
                        Icon {
                            name: Account.keyringAvailable ? "shield" : "triangle-alert"
                            size: Theme.space.iconSizeSmall
                            color: Account.keyringAvailable ? Theme.targetColors.iconSecondary : Theme.targetColors.warning
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            width: parent.width - Theme.space.iconSizeSmall - parent.spacing
                            anchors.verticalCenter: parent.verticalCenter
                            text: Account.keyringAvailable ? qsTr("Session saved in the desktop keyring.")
                                : Account.persistent ? qsTr("Session saved in Noctara's encrypted local store, because no desktop keyring is available.")
                                : qsTr("Session kept only until Noctara quits: no persistent secret store is available.")
                            font: Theme.font.caption
                            color: Account.keyringAvailable ? Theme.colors.textTertiary : Theme.colors.warning
                            wrapMode: Text.Wrap
                        }
                    }
                    Button {
                        visible: Account.permalinkUrl.length > 0
                        small: true
                        iconName: "external-link"
                        text: qsTr("View profile on SoundCloud")
                        onClicked: Qt.openUrlExternally(Account.permalinkUrl)
                    }
                }
                Button {
                    id: signOut
                    anchors.verticalCenter: parent.verticalCenter
                    iconName: "log-out"
                    text: qsTr("Sign out")
                    onClicked: Account.signOut()
                }
            }
        }

        // --- Session expired warning -----------------------------------------------------------
        GlassPanel {
            id: expiredBanner
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
                anchors.margins: Math.max(Theme.space.md, expiredBanner.cornerInset)
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
            Flow {
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
                                consent.profileLabel = entry.label
                                consent.encrypted = entry.encrypted
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
            // Preselect the first profile after a scan, so Import is usable right away.
            Connections {
                target: Account
                function onBrowserProfilesChanged() {
                    const profiles = Account.browserProfiles
                    if (!profiles.some(entry => entry.id === signInColumn.selectedProfile))
                        signInColumn.selectedProfile = profiles.length > 0 ? profiles[0].id : ""
                }
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
                property string profileLabel
                property bool encrypted
                title: qsTr("Allow browser cookie access?")
                //: %1 is the browser and profile, e.g. "Chromium (Flatpak) — Personal"
                message: encrypted
                         ? qsTr("Noctara will make a temporary read-only copy of the cookie database of %1, read only the SoundCloud oauth_token cookie, and delete the copy. This browser encrypts its cookies, so Noctara will also read the browser's cookie password from your desktop keyring or KWallet, which may ask you to unlock it. The token will be verified with SoundCloud and saved in your secret store. No other cookies are read.").arg(profileLabel)
                         //: %1 is the browser and profile, e.g. "Firefox — default-release"
                         : qsTr("Noctara will make a temporary read-only copy of the cookie database of %1, read only the SoundCloud oauth_token cookie, and delete the copy. The token will be verified with SoundCloud and saved in your secret store. No other cookies are read.").arg(profileLabel)
                confirmText: qsTr("Read cookie and sign in")
                onAccepted: Account.importBrowserToken(profileId)
            }
        }
    }
}
