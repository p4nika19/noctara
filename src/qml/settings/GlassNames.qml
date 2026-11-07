import QtQml

// Translated names for the glass ids, never the raw ids (About and Performance pages).
QtObject {
    function styleName(style) {
        switch (style) {
        case "kawase": return qsTr("Dual Kawase blur")
        case "gaussian": return qsTr("Gaussian blur")
        default: return qsTr("Solid (no blur)")
        }
    }
    function surfaceName(surface) {
        switch (surface) {
        case "liquid": return qsTr("Liquid Glass")
        case "flat": return qsTr("flat")
        default: return qsTr("frosted")
        }
    }
}
