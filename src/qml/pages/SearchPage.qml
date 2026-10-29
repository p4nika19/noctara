import QtQuick
import Noctara
import "../components"

// Search across SoundCloud: a field, the kind of result, and a lazily paginated list. States: nothing typed yet,
// first page loading (skeleton rows), results, nothing found, and errors with a retry.
Item {
    id: root

    readonly property CollectionModel results: Catalog.searchResults
    readonly property bool hasQuery: Catalog.query.length > 0

    // The "search" shortcut: focus the field, ready to type over the last query.
    function focusField() {
        field.forceActiveFocus()
        field.selectAll()
    }

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

        CollectionView {
            anchors.fill: parent
            anchors.margins: Math.max(Theme.space.sm, panel.cornerInset)
            model: root.results
            context: qsTr("Search")
            emptyText: qsTr("Nothing found for “%1”.").arg(Catalog.query)
        }
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
}
