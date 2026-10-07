import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    required property var audioSource
    readonly property var capture: audioSource.recording
    spacing: 12

    Label { text: qsTr("Step %1 of %2").arg(root.capture.step + 1).arg(root.capture.stepCount); color: Theme.muted; visible: !root.capture.complete }
    Label { text: root.capture.title; font.pixelSize: 18; font.weight: Font.DemiBold; wrapMode: Text.Wrap; Layout.fillWidth: true }
    Label { text: root.capture.instruction; wrapMode: Text.Wrap; Layout.fillWidth: true }
    Label {
        visible: root.capture.speaking
        text: root.capture.sentence
        font.pixelSize: 18
        wrapMode: Text.Wrap
        Layout.fillWidth: true
    }
    ProgressBar { Layout.fillWidth: true; visible: root.capture.active; value: root.capture.progress; Accessible.name: qsTr("Recording progress") }
    Label {
        visible: root.capture.active
        text: qsTr("%1 s remaining / %2 dBFS").arg(root.capture.remainingSeconds).arg(root.audioSource.inputDb.toFixed(0))
        color: Theme.muted
    }
    Label { visible: root.capture.reviewing; text: qsTr("Keep this take or record it again."); color: Theme.muted; wrapMode: Text.Wrap; Layout.fillWidth: true }
    Label { visible: root.capture.error.length > 0; text: root.capture.error; textFormat: Text.PlainText; color: Theme.danger; wrapMode: Text.Wrap; Layout.fillWidth: true }
    RowLayout {
        Layout.fillWidth: true
        ActionButton {
            objectName: "startRecordingStep"
            visible: !root.capture.active && !root.capture.reviewing && !root.capture.complete
            text: qsTr("Start")
            primary: true
            onClicked: root.audioSource.startRecording()
        }
        ActionButton { objectName: "finishRecordingStep"; visible: root.capture.active; text: qsTr("Stop"); onClicked: root.capture.finish() }
        ActionButton { objectName: "retryRecordingStep"; visible: root.capture.reviewing; text: qsTr("Record again"); destructive: true; onClicked: root.audioSource.startRecording() }
        ActionButton { objectName: "acceptRecordingStep"; visible: root.capture.reviewing; text: qsTr("Use recording"); primary: true; onClicked: root.capture.accept() }
        ActionButton { visible: root.capture.complete; text: qsTr("Reset test"); destructive: true; onClicked: root.capture.reset() }
    }
    ActionButton { visible: root.capture.resultPath.length > 0; text: qsTr("Open recordings"); onClicked: root.audioSource.openRecordings() }
    Label { text: qsTr("Saved on this device only. Nothing is uploaded."); color: Theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
}
