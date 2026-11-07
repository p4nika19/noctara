import QtQuick
import QtQuick.Templates as T
import Noctara

// Floating glass navigation. destinations: [{id, icon, label}]; footer holds Settings. Vertical (left or right
// edge) or horizontal (bottom edge); compact shows icons, expanded adds the labels.
// Right-clicking a destination emits secondaryActivated (the gear opens the quick Appearance panel).
GlassPanel {
    id: root

    property var destinations: []
    property var footer: []
    property string current
    property bool horizontal: false
    property bool expanded: false
    signal navigate(string id)
    signal secondaryActivated(string id)

    // The pill's position is a plain binding on the destinations' geometry, so it is always exact: it follows
    // resizes (the footer moves with the window size) and never depends on when delegates were created. A
    // selection change animates only a temporary offset from the old position back to zero, so nothing that
    // happens during the glide (a resize, another click) can leave the pill stranded between items.
    readonly property real labelHeight: expanded && horizontal ? Theme.font.caption.pixelSize * 1.4 : 0
    readonly property real itemExtent: horizontal
        ? (expanded ? 84 * Theme.scale : Theme.space.controlHeight + Theme.space.md)
        : Theme.space.controlHeight + Theme.space.md
    readonly property real pillLength: horizontal ? pill.width : pill.height
    readonly property real length: horizontal ? width : height
    function positionOf(id) {
        let index = destinations.findIndex(d => d.id === id)
        if (index >= 0)
            return Theme.space.md + index * itemExtent + (itemExtent - pillLength) / 2
        index = footer.findIndex(d => d.id === id)
        if (index >= 0)
            return length - Theme.space.md - (footer.length - index) * itemExtent + (itemExtent - pillLength) / 2
        return 0
    }
    function inRail(id) {
        return destinations.some(d => d.id === id) || footer.some(d => d.id === id)
    }
    readonly property real pillPosition: positionOf(current)
    // A page hidden from the rail (layout.navigation.items) can still be current: then nothing is selected.
    readonly property bool pillShown: inRail(current)
    property real glideOffset: 0
    property string shownCurrent: current

    onCurrentChanged: {
        if (inRail(shownCurrent) && inRail(current) && Theme.motion.slow > 0) {
            // Start where the pill is drawn now (even mid-glide). Derived from the previous selection, not
            // read from the pill, which may already have been re-evaluated for the new one.
            glideOffset = positionOf(shownCurrent) + glideOffset - positionOf(current)
            glide.restart()
        } else {
            glide.stop()  // to or from a page outside the rail: the pill appears in place
            glideOffset = 0
        }
        shownCurrent = current
    }
    NumberAnimation {
        id: glide
        target: root
        property: "glideOffset"
        to: 0
        duration: Theme.motion.slow
        easing.type: Theme.motion.easingEmphasized
    }

    level: 0
    liquid: true
    radius: Theme.radius.lg
    implicitWidth: horizontal ? 0 : (expanded ? Theme.space.navRailWidthExpanded : Theme.space.navRailWidth)
    implicitHeight: horizontal ? Theme.space.controlHeight + Theme.space.md * 2 + labelHeight : 0
    Accessible.role: Accessible.PageTabList
    Accessible.name: qsTr("Navigation")

    component Destination: T.AbstractButton {
        id: item
        required property var modelData
        readonly property bool selected: root.current === modelData.id
        width: root.horizontal ? root.itemExtent : root.width
        height: root.horizontal ? root.height : root.itemExtent
        focusPolicy: Qt.TabFocus
        hoverEnabled: true
        text: modelData.label
        Accessible.role: Accessible.PageTab
        Accessible.name: modelData.label
        Accessible.checked: selected
        onClicked: root.navigate(modelData.id)

        LazyTooltip {
            host: item
            shown: item.hovered && !root.expanded
            text: item.modelData.label
        }

        // The moving pill is the selection; a hover surface on the newly selected item would appear instantly
        // and look like a second highlight while the pill is still on its way.
        Item {
            anchors.centerIn: parent
            width: pill.width
            height: pill.height
            StateLayer {
                radius: Math.min(Theme.radius.button, height / 2)
                hovered: item.hovered && !item.selected
                pressed: item.down
            }
            FocusRing { target: item; targetRadius: Math.min(Theme.radius.button, parent.height / 2) }
        }
        Icon {
            id: icon
            x: root.horizontal || !root.expanded ? (parent.width - width) / 2
                                                 : (parent.width - pill.width) / 2 + Theme.space.md
            y: root.horizontal ? (parent.height - height - root.labelHeight) / 2 : (parent.height - height) / 2
            name: item.modelData.icon
            size: Theme.space.iconSize
            color: item.selected ? Theme.targetColors.accent : Theme.targetColors.iconSecondary
        }
        Text {
            visible: root.expanded
            x: root.horizontal ? (parent.width - width) / 2 : icon.x + icon.width + Theme.space.md
            y: root.horizontal ? icon.y + icon.height + Theme.space.xxs : (parent.height - height) / 2
            width: root.horizontal ? parent.width - Theme.space.xs * 2
                                   : parent.width - x - (parent.width - pill.width) / 2 - Theme.space.sm
            horizontalAlignment: root.horizontal ? Text.AlignHCenter : Text.AlignLeft
            text: item.modelData.label
            font: root.horizontal ? Theme.font.caption : Theme.font.label
            color: item.selected ? Theme.colors.accent : Theme.colors.textSecondary
            elide: Text.ElideRight
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: root.secondaryActivated(item.modelData.id)
        }
    }

    // Active item: lit glass with a light edge and an accent halo (no overshoot when it moves).
    Item {
        id: pill
        objectName: "selectionPill"
        visible: root.pillShown
        width: root.horizontal ? root.itemExtent - Theme.space.xs
                               : (root.expanded ? root.width - Theme.space.sm * 2 : Theme.space.controlHeight + Theme.space.xs)
        height: root.horizontal ? root.height - Theme.space.sm * 2 : Theme.space.controlHeight + Theme.space.xxs
        x: root.horizontal ? root.pillPosition + root.glideOffset : (root.width - width) / 2
        y: root.horizontal ? (root.height - height) / 2 : root.pillPosition + root.glideOffset

        Shadow {
            anchors.fill: parent
            z: -1
            radius: pillFill.radius
            blur: 30 * Theme.scale
            spread: -10 * Theme.scale
            offsetY: 8 * Theme.scale
            color: Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b,
                           Theme.glass.glowControls && !Theme.glass.flat ? Theme.glass.glow * 0.9 : 0)
        }
        Rectangle {
            id: pillFill
            anchors.fill: parent
            radius: Math.min(Theme.radius.button, height / 2)
            border.width: Theme.space.hairline
            border.color: Theme.dark ? Qt.rgba(1, 1, 1, 0.15)
                                     : Qt.rgba(Theme.colors.accent.r, Theme.colors.accent.g, Theme.colors.accent.b, 0.3)
            gradient: Gradient {
                GradientStop { position: 0.0; color: Theme.dark ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(1, 1, 1, 0.95) }
                GradientStop { position: 1.0; color: Theme.dark ? Qt.rgba(1, 1, 1, 0.04) : Qt.rgba(1, 1, 1, 0.6) }
            }
        }
    }

    Grid {
        x: root.horizontal ? Theme.space.md : 0
        y: root.horizontal ? 0 : Theme.space.md
        rows: root.horizontal ? 1 : Math.max(1, root.destinations.length)
        columns: root.horizontal ? Math.max(1, root.destinations.length) : 1
        Repeater {
            model: root.destinations
            delegate: Destination {}
        }
    }
    Grid {
        x: root.horizontal ? root.width - Theme.space.md - width : 0
        y: root.horizontal ? 0 : root.height - Theme.space.md - height
        rows: root.horizontal ? 1 : Math.max(1, root.footer.length)
        columns: root.horizontal ? Math.max(1, root.footer.length) : 1
        Repeater {
            model: root.footer
            delegate: Destination {}
        }
    }
}
