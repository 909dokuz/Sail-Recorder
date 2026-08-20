import QtQuick 2.6
import Sailfish.Silica 1.0

ApplicationWindow {
    id: app

    property bool busy: false
    property bool recording: false
    property double startedAt: 0
    property int elapsedSeconds: 0

    function timeText(seconds) {
        var h = Math.floor(seconds / 3600)
        var m = Math.floor((seconds % 3600) / 60)
        var s = seconds % 60

        function pad(v) {
            return v < 10 ? "0" + v : "" + v
        }

        return h > 0
                ? pad(h) + ":" + pad(m) + ":" + pad(s)
                : pad(m) + ":" + pad(s)
    }

    function checkStatus() {
        var xhr = new XMLHttpRequest()

        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE &&
                    xhr.status === 200) {

                if (xhr.responseText.indexOf("Hazir") === -1) {
                    recording = true

                    if (startedAt === 0) {
                        startedAt = Date.now()
                        elapsedSeconds = 0
                    }
                } else {
                    recording = false
                    startedAt = 0
                    elapsedSeconds = 0
                }
            }
        }

        xhr.open("GET", "http://127.0.0.1:37822/status")
        xhr.send()
    }

    function callApi(path) {
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
                        elapsedSeconds = 0
                    } else {
                        recording = false
                        startedAt = 0
                        elapsedSeconds = 0
                    }
                }
            }
        }

        xhr.open("GET", "http://127.0.0.1:37822/" + path)
        xhr.send()
    }

    Timer {
        interval: 1000
        repeat: true
        running: app.recording

        onTriggered: {
            if (app.startedAt > 0)
                app.elapsedSeconds =
                        Math.floor((Date.now() - app.startedAt) / 1000)
        }
    }

    Component.onCompleted: checkStatus()


    function refreshRecordingState() {
        if (busy)
            return

        var xhr = new XMLHttpRequest()

        xhr.onreadystatechange = function() {
            if (xhr.readyState === XMLHttpRequest.DONE &&
                    xhr.status === 200) {

                var active = xhr.responseText.indexOf("Hazir") === -1

                if (recording !== active) {
                    recording = active

                    if (active) {
                        statusText = "Kayıt devam ediyor"
                        detailText = "Kayıt aktif."
                    } else {
                        statusText = "Hazır"
                        detailText = "Kayıt durduruldu ve video kaydedildi."
                    }
                }
            }
        }

        xhr.open("GET", "http://127.0.0.1:37822/status")
        xhr.send()
    }

    Timer {
        id: recorderStateSync
        interval: 1000
        repeat: true
        running: true
        onTriggered: app.refreshRecordingState()
    }

    cover: Qt.resolvedUrl("SailRecorderCover.qml")
    initialPage: Component {
        Page {
            PageHeader {
                id: header
                title: "Sail Recorder"
            }

            Column {
                anchors.top: header.bottom
                anchors.topMargin: Theme.paddingLarge * 2
                width: parent.width
                spacing: Theme.paddingLarge

                Rectangle {
                    width: Theme.itemSizeHuge * 1.25
                    height: width
                    radius: width / 2
                    anchors.horizontalCenter: parent.horizontalCenter

                    color: app.recording
                           ? Theme.highlightColor
                           : Theme.rgba(Theme.primaryColor, 0.15)

                    Label {
                        anchors.centerIn: parent
                        text: "REC"
                        font.pixelSize: Theme.fontSizeLarge
                        font.bold: true
                        color: Theme.primaryColor
                    }

                    SequentialAnimation on opacity {
                        running: app.recording && !app.busy
                        loops: Animation.Infinite

                        NumberAnimation {
                            to: 0.35
                            duration: 600
                        }

                        NumberAnimation {
                            to: 1.0
                            duration: 600
                        }
                    }
                }

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: app.recording
                          ? app.timeText(app.elapsedSeconds)
                          : "00:00"
                    font.pixelSize: Theme.fontSizeHuge
                    color: Theme.primaryColor
                }

                Label {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: "Sail Recorder"
                    font.pixelSize: Theme.fontSizeLarge
                    font.bold: true
                    color: Theme.primaryColor
                }
            }

            Button {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.paddingLarge * 3

                text: app.busy
                      ? "Please wait..."
                      : (app.recording ? "Stop" : "Start")

                enabled: !app.busy

                onClicked: {
                    app.callApi(app.recording ? "stop" : "start")
                }
            }

            BusyIndicator {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Theme.paddingSmall
                running: app.busy
                size: BusyIndicatorSize.Small
            }
        }
    }
}
