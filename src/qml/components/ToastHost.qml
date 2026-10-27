import QtQuick
import Noctara

// Stacks toasts from App.toastRequested at the bottom centre; each disappears after a timeout
// (longer for errors, never while hovered or when it carries an action).
Item {
    id: root

    property real bottomMargin: Theme.space.xl

    ListModel { id: toasts }

    Connections {
        target: App
        function onToastRequested(kind, message, actionLabel, actionId) {
            toasts.append({ kind: kind, message: message, actionLabel: actionLabel, actionId: actionId })
            if (toasts.count > 4)
                toasts.remove(0)
        }
    }

    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: root.bottomMargin
        spacing: Theme.space.sm

        move: Transition { NumberAnimation { properties: "y"; duration: Theme.motion.normal; easing.type: Theme.motion.easingStandard } }
        add: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.motion.normal }
            NumberAnimation { property: "scale"; from: 0.94; to: 1; duration: Theme.motion.slow; easing.type: Theme.motion.easingEmphasized }
        }

        Repeater {
            model: toasts
            delegate: Toast {
                id: toast
                required property int index
                required property string kind
                required property string message
                required property string actionLabel
                required property string actionId
                anchors.horizontalCenter: parent.horizontalCenter
                onDismissed: toasts.remove(index)

                HoverHandler { id: hover }
                Timer {
                    interval: toast.kind === "error" ? 10000 : 5000
                    running: !hover.hovered && toast.actionLabel.length === 0
                    onTriggered: toasts.remove(toast.index)
                }
            }
        }
    }
}
