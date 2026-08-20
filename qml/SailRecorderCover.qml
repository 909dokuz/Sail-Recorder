import QtQuick 2.6
import Sailfish.Silica 1.0

CoverBackground {
    id: cover

    property bool recording: false
    property bool busy: false
    property double startedAt: 0
    property int elapsed: 0

    function pad(v) {
        return v < 10 ? "0" + v : "" + v
    }

    function timeText() {
        var h = Math.floor(elapsed / 3600)
        var m = Math.floor((elapsed % 3600) / 60)
        var s = elapsed % 60

        return h > 0
                ? pad(h) + ":" + pad(m) + ":" + pad(s)
                : pad(m) + ":" + pad(s)
    }

    function checkStatus() {
        var xhr = new XMLHttpRequest()
        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE && xhr.status === 200) {
                var active = xhr.responseText.indexOf("Hazir") === -1

                if (active && !recording) {
                    recording = true
                    startedAt = Date.now()
                    elapsed = 0
                } else if (!active) {
                    recording = false
                    startedAt = 0
                    elapsed = 0
                }
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
                busy = false

                if (xhr.status === 200) {
                    if (path === "start") {
                        recording = true
                        startedAt = Date.now()
                        elapsed = 0
                    } else {
                        recording = false
                        startedAt = 0
                        elapsed = 0
                    }
                }
            }
        }

        xhr.open("GET", "http://127.0.0.1:37822/" + path)
        xhr.send()
    }

    Rectangle {
        anchors.fill: parent
        color: "#16495b"
        opacity: 0.94
    }

    Label {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: parent.height * 0.15

        text: cover.timeText()
        color: "white"
        font.pixelSize: cover.width * 0.17
        font.bold: true
    }

    Timer {
        interval: 1000
        repeat: true
        running: cover.recording

        onTriggered: {
            if (cover.startedAt > 0)
                cover.elapsed = Math.floor((Date.now() - cover.startedAt) / 1000)
        }
    }

    Component.onCompleted: checkStatus()

    Timer {
        id: statusSyncTimer
        interval: 1000
        repeat: true
        running: true
        onTriggered: cover.checkStatus()
    }

    CoverActionList {
        enabled: !cover.busy

        CoverAction {
            iconSource: cover.recording
                        ? "image://theme/icon-cover-cancel"
                        : "image://theme/icon-cover-record"

            onTriggered: cover.command(cover.recording ? "stop" : "start")
        }
    }
}
