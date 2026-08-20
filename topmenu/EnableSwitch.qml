import QtQuick 2.0
import Sailfish.Silica 1.0
import com.jolla.settings 1.0
import com.jolla.settings.system 1.0
import Nemo.DBus 2.0

SettingsToggle {
    id: root

    name: "Sail Recorder"
    icon.source: "image://theme/icon-m-video"

    busy: false
    checked: false

    onToggled: {
        busy = true
        recorderBus.call("toggle")
    }

    DBusInterface {
        id: recorderBus

        bus: DBus.SessionBus
        service: "org.sailfishos.SailRecorder"
        path: "/org/sailfishos/SailRecorder"
        iface: "org.sailfishos.SailRecorder"

        signalsEnabled: true

        function stateChanged(state) {
            root.checked = state
            root.busy = false
        }
    }

    Component.onCompleted: recorderBus.call("refresh")

    onVisibleChanged: {
        if (visible)
            recorderBus.call("refresh")
    }
}
