import QtQuick
import Noctara
import "../components"

// Search across SoundCloud: a field, the kind of result, and a lazily paginated list. States: nothing typed yet,
// first page loading (skeleton rows), results, nothing found, and errors with a retry.
Item {
    id: root

    readonly property CollectionModel results: Catalog.searchResults
    readonly property bool hasQuery: Catalog.query.length > 0
    readonly property bool firstLoad: results.loading && results.count === 0

    Column {
        id: header
        width: parent.width
        spacing: Theme.space.lg

        Text {
            text: qsTr("Search")
            font: Theme.font.display
            color: Theme.colors.textPrimary
            Accessible.role: Accessible.Heading
        }

        TextField {
            id: field
            width: Math.min(parent.width, 560 * Theme.scale)
            iconName: "search"
            placeholderText: qsTr("Tracks, playlists, albums and people")
            onTextEdited: Catalog.search(text, kinds.currentId)
            Keys.onReturnPressed: Catalog.searchNow()
            Keys.onEnterPressed: Catalog.searchNow()
            Component.onCompleted: text = Catalog.query
        }

        SegmentedControl {
            id: kinds
            currentId: Catalog.searchKind
            model: [
                { id: "all", label: qsTr("All") },
                { id: "tracks", label: qsTr("Tracks") },
                { id: "playlists", label: qsTr("Playlists") },
                { id: "albums", label: qsTr("Albums") },
                { id: "users", label: qsTr("People") }
            ]
            onActivated: id => Catalog.search(field.text, id)
        }
    }

    GlassPanel {
        id: panel
        anchors.top: header.bottom
        anchors.topMargin: Theme.space.lg
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        radius: Theme.radius.lg
        visible: root.hasQuery

        ListView {
            id: list
            anchors.fill: parent
            anchors.margins: Math.max(Theme.space.sm, panel.cornerInset)
            clip: true
            reuseItems: true
            cacheBuffer: Theme.space.rowHeight * 10
            boundsBehavior: Flickable.StopAtBounds
            model: root.results
            visible: root.results.count > 0
            ScrollBar.vertical: ScrollBar {}
            delegate: CatalogRow {
                width: ListView.view.width
            }
            footer: Column {
                width: list.width
                visible: root.results.loading && root.results.count > 0
                height: visible ? implicitHeight : 0
                Repeater {
                    model: 2
                    delegate: SkeletonRow {}
                }
            }
        }

        // First page on its way.
        Column {
            anchors.fill: list
            visible: root.firstLoad
            Repeater {
                model: 8
                delegate: SkeletonRow {}
            }
        }

        // Nothing found, or the search failed.
        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - Theme.space.xl * 2, 420 * Theme.scale)
            spacing: Theme.space.md
            visible: !root.results.loading && root.results.count === 0
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: root.results.errorCode >= 0
                      ? Format.error(root.results.errorCode)
                      : qsTr("Nothing found for “%1”.").arg(Catalog.query)
                font: Theme.font.body
                color: Theme.colors.textSecondary
                wrapMode: Text.Wrap
            }
            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                visible: root.results.errorCode >= 0
                iconName: "refresh-cw"
                text: qsTr("Try again")
                onClicked: root.results.reload()
            }
        }
    }

    Text {
        anchors.top: panel.bottom
        anchors.right: panel.right
        visible: root.results.stale && root.hasQuery
        text: qsTr("Offline: showing saved results")
        font: Theme.font.caption
        color: Theme.colors.textTertiary
    }

    EmptyPage {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: !root.hasQuery
        iconName: "search"
        title: qsTr("Search SoundCloud")
        message: qsTr("Find tracks, playlists, albums and people. Results load as you scroll.")
    }

    component SkeletonRow: Item {
        width: list.width
        height: Theme.space.rowHeight
        Skeleton {
            id: art
            x: Theme.space.sm
            anchors.verticalCenter: parent.verticalCenter
            width: parent.height - Theme.space.sm * 2
            height: width
            radius: Theme.coverShape === "circle" ? width / 2 : (Theme.coverShape === "square" ? 0 : Theme.radius.sm)
            loading: true
        }
        Column {
            anchors.left: art.right
            anchors.leftMargin: Theme.space.md
            anchors.verticalCenter: parent.verticalCenter
            spacing: Theme.space.sm
            Skeleton { width: 200 * Theme.scale; height: 11 * Theme.scale; radius: height / 2; loading: true }
            Skeleton { width: 120 * Theme.scale; height: 9 * Theme.scale; radius: height / 2; loading: true }
        }
    }
}
