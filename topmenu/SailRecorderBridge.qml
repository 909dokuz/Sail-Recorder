import QtQuick 2.0
import Nemo.DBus 2.0

Item {
    id: root

    property bool recording: false
    property bool busy: false

    function publishState() {
        dbus.emitSignal("stateChanged", [recording])
    }

    function checkStatus() {
        if (busy)
            return

        var xhr = new XMLHttpRequest()

        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                if (xhr.status === 200)
                    recording = xhr.responseText.indexOf("Hazir") === -1

                publishState()
            }
        }

        xhr.open("GET", "http://127.0.0.1:37822/status")
        xhr.send()
    }

    function command(path) {
        if (busy)
            return

        busy = true

        var xhr = new XMLHttpRequest()

        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE) {
                console.log("Sail Recorder", path, "reply:", xhr.responseText)
                busy = false
                checkStatus()
            }
        }

        xhr.open("GET", "http://127.0.0.1:37822/" + path)
        xhr.send()
    }

    DBusAdaptor {
        id: dbus

        bus: DBus.SessionBus
        service: "org.sailfishos.SailRecorder"
        path: "/org/sailfishos/SailRecorder"
        iface: "org.sailfishos.SailRecorder"

        xml: '<interface name="org.sailfishos.SailRecorder">' +
             '<method name="toggle"/>' +
             '<method name="refresh"/>' +
             '<signal name="stateChanged">' +
             '<arg name="state" type="b"/>' +
             '</signal>' +
             '</interface>'

        function toggle() {
            root.command(root.recording ? "stop" : "start")
        }

        function refresh() {
            root.checkStatus()
        }
    }

    Timer {
        interval: 1500
        repeat: true
        running: true
        onTriggered: root.checkStatus()
    }

    Component.onCompleted: checkStatus()
}
