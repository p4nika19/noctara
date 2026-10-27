import QtQuick
import Noctara
import "../components"

// Settings: category list + search on the left, generated rows on the right.
Item {
    id: root

    property string category: "general"
    signal categoryRequested(string id)
    readonly property bool searching: SettingsUi.searchText.trim().length > 0
    readonly property var currentCategory: SettingsUi.categories.find(c => c.id === root.category) || null

    GlassPanel {
        id: sidebar
        level: 1
        radius: Theme.radius.lg
        width: 260 * Theme.scale
        anchors.top: parent.top
        anchors.bottom: parent.bottom

        TextField {
            id: search
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Theme.space.md
            iconName: "search"
            placeholderText: qsTr("Search settings")
            text: SettingsUi.searchText
            onTextEdited: SettingsUi.searchText = text
            Keys.onEscapePressed: SettingsUi.searchText = ""
        }

        ListView {
            id: categories
            anchors.top: search.bottom
            anchors.topMargin: Theme.space.md
            anchors.bottom: advanced.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Theme.space.sm
            clip: true
            model: SettingsUi.categories
            spacing: Theme.space.xxs
            keyNavigationEnabled: true
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: qsTr("Settings categories")

            delegate: Item {
                id: entry
                required property var modelData
                readonly property bool selected: !root.searching && root.category === modelData.id
                width: ListView.view.width
                height: Theme.space.controlHeight
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.label
                Accessible.selected: selected

                Rectangle {
                    anchors.fill: parent
                    radius: Theme.radius.sm
                    color: entry.selected ? Theme.colors.accentContainer : "transparent"
                    StateLayer { radius: parent.radius; hovered: area.containsMouse; pressed: area.pressed }
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.space.md
                    spacing: Theme.space.md
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: entry.modelData.icon
                        size: Theme.space.iconSizeSmall
                        color: entry.selected ? Theme.targetColors.onAccentContainer : Theme.targetColors.iconSecondary
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: entry.width - Theme.space.iconSizeSmall - Theme.space.md * 3
                        text: entry.modelData.label
                        font: entry.selected ? Theme.font.bodyStrong : Theme.font.body
                        color: entry.selected ? Theme.colors.onAccentContainer : Theme.colors.textPrimary
                        elide: Text.ElideRight
                    }
                }
                MouseArea {
                    id: area
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        SettingsUi.searchText = ""
                        root.categoryRequested(entry.modelData.id)
                    }
                }
            }
        }

        Row {
            id: advanced
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: Theme.space.md
            spacing: Theme.space.sm
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - advancedSwitch.width - parent.spacing
                text: qsTr("Show advanced settings")
                font: Theme.font.label
                color: Theme.colors.textSecondary
                wrapMode: Text.Wrap
            }
            Switch {
                id: advancedSwitch
                checked: SettingsUi.showAdvanced
                Accessible.name: qsTr("Show advanced settings")
                onToggled: SettingsUi.showAdvanced = checked
            }
        }
    }

    GlassPanel {
        id: content
        level: 0
        radius: Theme.radius.lg
        anchors.left: sidebar.right
        anchors.leftMargin: Theme.space.lg
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom

        Item {
            id: header
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.space.xl
            height: Math.max(title.implicitHeight, resetPage.implicitHeight)

            Text {
                id: title
                anchors.left: parent.left
                anchors.right: resetPage.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.searching ? qsTr("Search results") : (root.currentCategory ? root.currentCategory.label : "")
                font: Theme.font.headline
                color: Theme.colors.textPrimary
                elide: Text.ElideRight
                Accessible.role: Accessible.Heading
            }
            Button {
                id: resetPage
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                visible: !root.searching && root.category !== "about"
                variant: "ghost"
                iconName: "rotate-ccw"
                text: qsTr("Reset page")
                onClicked: SettingsUi.resetCategory(root.category)
            }
        }

        GlassPanel {
            id: restartBanner
            visible: SettingsUi.restartPending
            level: 2
            radius: Theme.radius.md
            anchors.top: header.bottom
            anchors.topMargin: Theme.space.md
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: Theme.space.xl
            anchors.rightMargin: Theme.space.xl
            height: visible ? Math.max(bannerText.implicitHeight, restartButton.implicitHeight) + Theme.space.md * 2 : 0
            Text {
                id: bannerText
                anchors.left: parent.left
                anchors.leftMargin: Theme.space.lg
                anchors.right: restartButton.left
                anchors.rightMargin: Theme.space.md
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Some changes take effect after Noctara restarts.")
                font: Theme.font.body
                color: Theme.colors.textPrimary
                wrapMode: Text.Wrap
            }
            Button {
                id: restartButton
                anchors.right: parent.right
                anchors.rightMargin: Theme.space.md
                anchors.verticalCenter: parent.verticalCenter
                variant: "primary"
                text: qsTr("Restart now")
                onClicked: App.restart()
            }
        }

        Loader {
            anchors.top: restartBanner.visible ? restartBanner.bottom : header.bottom
            anchors.topMargin: Theme.space.md
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: Theme.space.xl
            anchors.rightMargin: Theme.space.lg
            sourceComponent: !root.searching && root.category === "about" ? about : rows
        }

        Component {
            id: rows
            ListView {
                id: list
                clip: true
                reuseItems: true
                cacheBuffer: 400
                boundsBehavior: Flickable.StopAtBounds
                rightMargin: Theme.space.md
                bottomMargin: Theme.space.xl
                model: root.searching ? SettingsUi.searchResults : SettingsUi.page(root.category)
                ScrollBar.vertical: ScrollBar {}
                delegate: SettingsRow {
                    showCategory: root.searching
                    width: list.width - list.rightMargin
                }
                Text {
                    anchors.centerIn: parent
                    visible: root.searching && list.count === 0
                    text: qsTr("No settings match “%1”.").arg(SettingsUi.searchText)
                    font: Theme.font.body
                    color: Theme.colors.textSecondary
                }
            }
        }
        Component {
            id: about
            AboutPane {}
        }
    }
}
