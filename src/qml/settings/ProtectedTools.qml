import QtQuick
import Noctara
import "../components"

// Under the protected playback row: force install or reinstall the protected playback helper.
Row {
    spacing: Theme.space.sm

    Button {
        small: true
        iconName: Drm.installing ? "download" : (Drm.installed ? "refresh-cw" : "download")
        text: Drm.installing ? qsTr("Installing…") : (Drm.installed ? qsTr("Reinstall") : qsTr("Install"))
        enabled: !Drm.installing
        onClicked: {
            if (Drm.installed)
                Drm.reinstall()
            else
                Drm.install(true)
        }
    }
}
