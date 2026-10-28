import QtQuick
import Noctara
import "../components"

// Home: date and greeting, the now-playing card and recently played. Until an account is connected the
// recently played cards show skeleton placeholders.
Flickable {
    id: root

    contentHeight: column.implicitHeight + Theme.space.xxl
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    readonly property date now: new Date()
    readonly property int hour: now.getHours()

    Column {
        id: column
        width: root.width
        spacing: Theme.space.xl

        Column {
            spacing: Theme.space.xs
            Text {
                text: root.now.toLocaleDateString(Qt.locale(Language.localeName), "dddd, d MMMM")
                font: Theme.font.eyebrow
                color: Theme.colors.textTertiary
            }
            Text {
                text: root.hour < 5 ? qsTr("Good night")
                    : root.hour < 12 ? qsTr("Good morning")
                    : root.hour < 18 ? qsTr("Good afternoon") : qsTr("Good evening")
                font: Theme.font.display
                color: Theme.colors.textPrimary
                Accessible.role: Accessible.Heading
            }
        }

        GlassPanel {
            id: hero
            width: column.width
            height: heroRow.implicitHeight + Theme.space.xl * 2
            radius: Theme.radius.lg

            Row {
                id: heroRow
                x: Theme.space.xl
                y: Theme.space.xl
                width: hero.width - Theme.space.xl * 2
                spacing: Theme.space.xl
                Item {
                    width: 180 * Theme.scale
                    height: width
                    Shadow {
                        anchors.fill: parent
                        z: -1
                        radius: cover.radius
                        blur: 70 * Theme.scale
                        offsetY: 24 * Theme.scale
                        spread: -8 * Theme.scale
                        color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                                       Theme.glass.glowCover ? Theme.glass.glow * 0.9 : 0)
                    }
                    Skeleton {
                        id: cover
                        anchors.fill: parent
                        visible: !Player.hasTrack
                        radius: Theme.coverShape === "circle" ? width / 2 : (Theme.coverShape === "square" ? 0 : Theme.radius.lg)
                    }
                    CoverArt {
                        anchors.fill: parent
                        visible: Player.hasTrack
                        radius: Theme.radius.lg
                        source: Player.artworkUrl
                    }
                }
                Column {
                    width: heroRow.width - 180 * Theme.scale - heroRow.spacing
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: Theme.space.md
                    Text {
                        text: qsTr("Now playing")
                        font: Theme.font.eyebrow
                        color: Theme.colors.textTertiary
                    }
                    Text {
                        width: parent.width
                        text: Player.hasTrack ? Player.title : qsTr("Nothing is playing yet")
                        font: Theme.font.headline
                        color: Theme.colors.textPrimary
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                    }
                    Text {
                        width: parent.width
                        text: Player.hasTrack
                              ? (Player.context.length > 0 ? qsTr("%1 · from %2").arg(Player.artist).arg(Player.context)
                                                           : Player.artist)
                              : (Account.signedIn ? qsTr("Find something in Search and it starts here.")
                                                  : qsTr("Connect your SoundCloud account to see your feed, likes and playlists."))
                        font: Theme.font.body
                        color: Theme.colors.textSecondary
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                    }
                }
            }
        }

        Column {
            width: column.width
            spacing: Theme.space.md
            Text {
                text: qsTr("Recently played")
                font: Theme.font.title
                color: Theme.colors.textPrimary
            }
            Flow {
                width: column.width
                spacing: Theme.space.lg
                Repeater {
                    model: 6
                    delegate: Column {
                        spacing: Theme.space.sm
                        Skeleton {
                            width: 172 * Theme.scale
                            height: width
                            radius: Theme.coverShape === "circle" ? width / 2
                                  : (Theme.coverShape === "square" ? 0 : Theme.radius.lg * 0.7)
                        }
                        Skeleton { width: 132 * Theme.scale; height: 12 * Theme.scale; radius: height / 2 }
                        Skeleton { width: 90 * Theme.scale; height: 10 * Theme.scale; radius: height / 2 }
                    }
                }
            }
        }
    }
}
