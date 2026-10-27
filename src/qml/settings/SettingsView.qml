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
            anchors.margins: Math.max(Theme.space.md, sidebar.cornerInset)
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
                height: Theme.space.controlHeight + Theme.space.xxs
                Accessible.role: Accessible.ListItem
                Accessible.name: modelData.label
                Accessible.selected: selected

                // Same lit-glass treatment as the active navigation item.
                Shadow {
                    anchors.fill: parent
                    z: -1
                    radius: highlight.radius
                    blur: 30 * Theme.scale
                    spread: -10 * Theme.scale
                    offsetY: 8 * Theme.scale
                    color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                                   entry.selected && Theme.glass.glowControls ? Theme.glass.glow * 0.9 : 0)
                }
                Rectangle {
                    id: highlight
                    anchors.fill: parent
                    radius: Math.min(Theme.radius.button, height / 2)
                    visible: entry.selected
                    border.width: Theme.space.hairline
                    border.color: Theme.dark ? Qt.rgba(1, 1, 1, 0.15)
                                             : Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.3)
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: Theme.dark ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0.95) }
                        GradientStop { position: 1.0; color: Theme.dark ? Qt.rgba(1, 1, 1, 0.04) : Qt.rgba(1, 1, 1, 0.6) }
                    }
                }
                StateLayer {
                    radius: Math.min(Theme.radius.button, height / 2)
                    hovered: area.containsMouse && !entry.selected
                    pressed: area.pressed
                }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    x: Theme.space.md
                    spacing: Theme.space.md
                    Icon {
                        anchors.verticalCenter: parent.verticalCenter
                        name: entry.modelData.icon
                        size: Theme.space.iconSizeSmall
                        color: entry.selected ? Theme.targetColors.accent : Theme.targetColors.iconSecondary
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        width: entry.width - Theme.space.iconSizeSmall - Theme.space.md * 3
                        text: entry.modelData.label
                        font: Theme.font.bodyStrong
                        color: entry.selected ? Theme.colors.textPrimary : Theme.colors.textSecondary
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
            anchors.margins: Math.max(Theme.space.md, sidebar.cornerInset)
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
            anchors.margins: Math.max(Theme.space.xl, content.cornerInset)
            height: Math.max(title.implicitHeight, resetPage.implicitHeight)

            Text {
                id: title
                anchors.left: parent.left
                anchors.right: resetPage.left
                anchors.verticalCenter: parent.verticalCenter
                text: root.searching ? qsTr("Search results") : (root.currentCategory ? root.currentCategory.label : "")
                font: Theme.font.headline
                color: Theme.colors.textPrimary
                // Long titles (Russian) shrink to fit before they are elided, never break mid-word.
                fontSizeMode: Text.HorizontalFit
                minimumPixelSize: Theme.font.title.pixelSize
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
            anchors.rightMargin: Math.max(Theme.space.lg, content.cornerInset)
            anchors.bottomMargin: content.cornerInset
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
                header: Loader {
                    width: list.width - list.rightMargin
                    active: !root.searching && (root.category === "appearance" || root.category === "performance")
                    sourceComponent: root.category === "appearance" ? appearanceHeader : performanceHeader
                }
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
        Component {
            id: appearanceHeader
            AppearanceHeader {}
        }
        Component {
            id: performanceHeader
            PerformanceHeader {}
        }
    }
}
