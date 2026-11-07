import QtQuick
import QtQuick.Templates as T
import Noctara

// Edits one keyboard shortcut: shows the keys; click (or Enter) and press the new combination. Backspace or
// Delete removes the shortcut, Escape cancels. Lone modifier presses wait for the real key.
T.AbstractButton {
    id: control

    property string value  // QKeySequence portable text, "" = none
    property bool recording: false
    signal picked(string sequence)

    implicitWidth: Math.max(140 * Theme.scale, label.implicitWidth + Theme.space.lg * 2)
    implicitHeight: Theme.space.controlHeightSmall
    focusPolicy: Qt.StrongFocus
    hoverEnabled: true
    Accessible.role: Accessible.Button
    Accessible.description: recording ? qsTr("Press the new shortcut") : ""

    onClicked: {
        recording = true
        forceActiveFocus()
    }
    onActiveFocusChanged: if (!activeFocus) recording = false

    Keys.priority: Keys.BeforeItem
    Keys.onPressed: event => {
        if (!recording) {
            if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Space) {
                recording = true
                event.accepted = true
            }
            return
        }
        event.accepted = true
        // Keys on the keypad (Delete with NumLock off) carry the keypad modifier.
        const plain = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier
        if (plain && event.key === Qt.Key_Escape) {
            recording = false
            return
        }
        if (plain && (event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete)) {
            recording = false
            control.picked("")
            return
        }
        const sequence = Shortcuts.fromKeyEvent(event.key, event.modifiers)
        if (sequence.length > 0) {
            recording = false
            control.picked(sequence)
        }
    }
    // While recording, shortcuts of the window must not fire.
    Keys.onShortcutOverride: event => event.accepted = recording

    background: ButtonSurface {
        radius: Theme.radius.sm
        hovered: control.hovered
        pressed: control.down
        active: control.recording
        FocusRing { target: control; targetRadius: parent.radius; visible: control.visualFocus }
    }
    contentItem: Text {
        id: label
        text: control.recording ? qsTr("Press keys…")
              : (control.value.length > 0 ? Shortcuts.display(control.value) : qsTr("None"))
        font: control.recording ? Theme.font.label : Theme.font.mono
        color: control.recording ? Theme.colors.accent
               : (control.value.length > 0 ? Theme.colors.textPrimary : Theme.colors.textTertiary)
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
