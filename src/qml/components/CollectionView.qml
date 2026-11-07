import QtQuick
import QtQuick.Window
import Noctara

// A CollectionModel as a list of rows or a grid of cards, with its states: first page loading (skeletons),
// nothing there, failed (with a retry) and offline (saved results). Activating a track plays it with the list's
// other tracks as the queue; playlists and people open their pages; right-click opens the context menu. `header`
// scrolls with the content (detail pages put their header there).
Item {
    id: root

    property CollectionModel model
    property string mode: "list"  // "list" | "grid"
    property string context       // what the tracks play from (queue, MPRIS)
    property string emptyText
    property Component header: null
    readonly property bool firstLoad: model !== null && model.loading && model.count === 0
    readonly property bool failed: model !== null && !model.loading && model.count === 0 && model.errorCode >= 0
    readonly property Item view: mode === "grid" ? grid : list
    // Card width for the "card size" setting; the grid fits as many per row as the width allows.
    readonly property real cardWidth: (Layout.cardSize === "small" ? 140 : Layout.cardSize === "large" ? 220 : 176)
                                      * Theme.scale
    readonly property int columns: Math.max(1, Math.floor((width + Theme.space.lg) / (cardWidth + Theme.space.lg)))

    function activate(index, kind, itemId) {
        if (kind === "track")
            Player.playCollection(root.model, index, root.context)
        else if (kind === "playlist")
            Window.window.openPlaylist(itemId)
        else if (kind === "user")
            Window.window.openUser(itemId)
    }
    function menu(index, item, point) {
        Window.window.showCollectionMenu(root.model, index, root.context, item, point)
    }

    ListView {
        id: list
        anchors.fill: parent
        visible: root.mode === "list"
        clip: true
        reuseItems: true
        cacheBuffer: Theme.space.rowHeight * 10
        boundsBehavior: Flickable.StopAtBounds
        // Bound by mode, not by visibility: dropping the model of a hidden page cancels delegates mid-creation.
        model: root.mode === "list" ? root.model : null
        header: root.header
        ScrollBar.vertical: ScrollBar {}
        delegate: CatalogRow {
            width: ListView.view.width
            onActivated: root.activate(index, kind, itemId)
            onEnqueueRequested: Player.enqueueCollection(root.model, index, false)
            onMenuRequested: point => root.menu(index, this, point)
        }
        footer: Column {
            width: list.width
            visible: root.model !== null && root.model.loading
            height: visible ? implicitHeight : 0
            Repeater {
                model: root.firstLoad ? 8 : 2
                delegate: SkeletonRow { width: list.width }
            }
        }
    }

    GridView {
        id: grid
        anchors.fill: parent
        visible: root.mode === "grid"
        clip: true
        reuseItems: true
        boundsBehavior: Flickable.StopAtBounds
        model: root.mode === "grid" ? root.model : null
        header: root.header
        cellWidth: Math.floor(width / root.columns)
        cellHeight: cellWidth - Theme.space.lg + Theme.font.bodyStrong.pixelSize * 1.4
                    + Theme.font.caption.pixelSize * 1.4 + Theme.space.xl
        cacheBuffer: cellHeight * 2
        ScrollBar.vertical: ScrollBar {}
        delegate: Item {
            id: cell
            required property int index
            required property string kind
            required property var itemId
            required property var ownerId
            required property string permalink
            required property string title
            required property string subtitle
            required property string artwork
            required property bool protectedMedia
            required property bool album
            required property bool pending
            width: GridView.view.cellWidth
            height: GridView.view.cellHeight
            Card {
                x: 0
                width: cell.width - Theme.space.lg
                index: cell.index
                kind: cell.kind
                itemId: cell.itemId
                ownerId: cell.ownerId
                permalink: cell.permalink
                title: cell.title
                subtitle: cell.subtitle
                artwork: cell.artwork
                protectedMedia: cell.protectedMedia
                album: cell.album
                pending: cell.pending
                onActivated: root.activate(cell.index, cell.kind, cell.itemId)
                onPlayRequested: {
                    if (cell.kind === "playlist")
                        Player.playCollection(root.model, cell.index, "")
                    else
                        root.activate(cell.index, cell.kind, cell.itemId)
                }
                onMenuRequested: point => root.menu(cell.index, this, point)
            }
        }
        footer: Item {
            width: grid.width
            height: root.model !== null && root.model.loading && !root.firstLoad ? Theme.space.xxl : 0
        }
    }

    // First page on its way in grid mode (the list shows skeleton rows in its footer).
    Flow {
        anchors.fill: parent
        anchors.topMargin: root.header ? Theme.space.xxxl * 3 : 0
        visible: root.mode === "grid" && root.firstLoad
        spacing: Theme.space.lg
        Repeater {
            model: 8
            delegate: Column {
                spacing: Theme.space.sm
                Skeleton {
                    width: root.cardWidth
                    height: width
                    radius: Theme.coverShape === "circle" ? width / 2 : (Theme.coverShape === "square" ? 0 : Theme.radius.md)
                    loading: true
                }
                Skeleton { width: root.cardWidth * 0.75; height: 12 * Theme.scale; radius: height / 2; loading: true }
            }
        }
    }

    // Nothing there, or the first page failed.
    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        y: (root.header ? Math.min(parent.height / 2, root.view.headerItem ? root.view.headerItem.height + Theme.space.xxl : 0)
                        : parent.height / 2 - height / 2)
        width: Math.min(parent.width - Theme.space.xl * 2, 420 * Theme.scale)
        spacing: Theme.space.md
        visible: root.model !== null && !root.model.loading && root.model.count === 0
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: root.failed ? Format.error(root.model.errorCode) : root.emptyText
            font: Theme.font.body
            color: Theme.colors.textSecondary
            wrapMode: Text.Wrap
        }
        Button {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: root.failed
            iconName: "refresh-cw"
            text: qsTr("Try again")
            onClicked: root.model.reload()
        }
    }

    Text {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: Theme.space.sm
        visible: root.model !== null && root.model.stale
        text: qsTr("Offline: showing saved results")
        font: Theme.font.caption
        color: Theme.colors.textTertiary
    }

    component SkeletonRow: Item {
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
