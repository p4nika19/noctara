import QtQuick
import Noctara
import "../components"

// Home: greeting and the feed. Until an account is connected it shows skeleton placeholders.
Flickable {
    id: root

    contentHeight: column.implicitHeight + Theme.space.xxl
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    readonly property int hour: new Date().getHours()

    Column {
        id: column
        width: root.width
        spacing: Theme.space.xl

        Column {
            spacing: Theme.space.xs
            Text {
                text: root.hour < 5 ? qsTr("Good night")
                    : root.hour < 12 ? qsTr("Good morning")
                    : root.hour < 18 ? qsTr("Good afternoon") : qsTr("Good evening")
                font: Theme.font.display
                color: Theme.colors.textPrimary
                Accessible.role: Accessible.Heading
            }
            Text {
                width: column.width
                text: qsTr("Connect your SoundCloud account to see your feed, likes and playlists.")
                font: Theme.font.body
                color: Theme.colors.textSecondary
                wrapMode: Text.Wrap
            }
        }

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
                        width: 180 * Theme.scale
                        height: width
                        radius: Theme.coverShape === "circle" ? width / 2 : (Theme.coverShape === "square" ? 0 : Theme.radius.md)
                    }
                    Skeleton { width: 140 * Theme.scale; height: 12 * Theme.scale; radius: height / 2 }
                    Skeleton { width: 96 * Theme.scale; height: 10 * Theme.scale; radius: height / 2 }
                }
            }
        }
    }
}
