import QtQuick
import QtQuick.Templates as T
import Noctara

T.ToolTip {
    id: control

    delay: 600
    timeout: 6000
    x: (parent ? parent.width - implicitWidth : 0) / 2
    y: -implicitHeight - Theme.space.xs
    padding: Theme.space.sm
    leftPadding: Theme.space.md
    rightPadding: Theme.space.md
    implicitWidth: Math.min(contentItem.implicitWidth + leftPadding + rightPadding, 360 * Theme.scale)
    implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        font: Theme.font.caption
        color: Theme.colors.textPrimary
        wrapMode: Text.Wrap
    }
    background: Rectangle {
        radius: Theme.radius.sm
        color: Theme.colors.solidOverlay
        border.width: Theme.space.hairline
        border.color: Theme.colors.edge
    }
    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motion.fast } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: Theme.motion.fast } }
}
