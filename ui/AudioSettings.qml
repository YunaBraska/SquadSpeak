import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    readonly property var audioSource: audio
    property var sessionSource: session
    property int settingsPage: 0
    property bool recordingMode: false
    onRecordingModeChanged: {
        if (recordingMode) { audio.recording.reset(); recordingDialog.open() }
        else recordingDialog.close()
    }
    PanelDialog {
        id: recordingDialog
        objectName: "recordingDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(500, parent.width - 16)
        height: Math.min(parent.height - 16, recordingBody.implicitHeight + header.height + footer.height + topPadding + bottomPadding + 16)
        modal: true
        title: qsTr("Guided recording")
        buttons: Dialog.Close
        onClosed: { audio.stop(); audio.recording.reset(); root.recordingMode = false }
        contentItem: ScrollView {
            contentWidth: availableWidth; clip: true
            RecordingPanel { id: recordingBody; width: parent.width; audioSource: root.audioSource }
        }
    }
    PanelDialog {
        id: microphonePermission
        objectName: "microphonePermissionDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(380, parent.width - 24)
        modal: true; title: qsTr("Microphone access is off")
        buttons: Dialog.Cancel
        contentItem: ColumnLayout {
            Label { text: qsTr("Allow SquadSpeak in your system's microphone settings, then try again."); wrapMode: Text.Wrap; Layout.fillWidth: true }
            ActionButton { text: qsTr("Open system settings"); onClicked: { audio.openMicrophoneSettings(); microphonePermission.close() } }
        }
    }
    Connections {
        target: audio
        function onMicrophoneAccessDenied() { if (root.Window.window && root.Window.window.visible) microphonePermission.open() }
    }
    component SectionLabel: Label {
        font.pixelSize: 16
        font.weight: Font.DemiBold
        topPadding: 8
    }

    component Divider: Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: Theme.border
    }

    component Meter: ColumnLayout {
        id: meter
        property string caption
        property real decibels: -96
        property real minimumDb: -60
        property real thresholdDb: NaN
        property color barColor: Theme.accent
        spacing: 6
        RowLayout {
            Layout.fillWidth: true
            Label { text: caption; Layout.fillWidth: true }
            Label { text: decibels <= -96 ? qsTr("Silent") : qsTr("%1 dBFS").arg(decibels.toFixed(1)); font.family: "monospace" }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 8
            color: Theme.raised
            radius: Theme.smallRadius
            Rectangle {
                width: parent.width * Math.max(0, Math.min(1, (decibels - minimumDb) / -minimumDb))
                height: parent.height
                radius: Theme.smallRadius
                color: barColor
            }
            Rectangle {
                visible: isFinite(meter.thresholdDb)
                x: visible ? Math.max(0, Math.min(parent.width - width,
                    parent.width * (meter.thresholdDb - minimumDb) / -minimumDb)) : 0
                y: -3
                width: 2
                height: parent.height + 6
                color: Theme.text
            }
        }
    }


    ColumnLayout {
        anchors.fill: parent
        spacing: 6
        ColumnLayout {
            visible: root.settingsPage === 0
            Layout.fillWidth: true
            spacing: 4
            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                ChoiceBox {
                    id: inputDevice
                    Layout.fillWidth: true
                    enabled: !audio.recording.active
                    model: audio.inputs; textRole: "label"; valueRole: "deviceId"
                    currentIndex: audio.inputIndex
                    displayText: currentIndex < 0 ? qsTr("Device disconnected") : currentText
                    Accessible.name: qsTr("Input device")
                    TextToolTip { parent: inputDevice; text: audio.inputName; visible: inputDevice.hovered }
                    onActivated: {
                        audio.selectInput(currentValue)
                        currentIndex = Qt.binding(function() { return audio.inputIndex })
                    }
                }
                ActionButton {
                    objectName: "guidedTest"
                    text: qsTr("Guided recording")
                    glyph: "record"; iconOnly: true; flat: true
                    onClicked: root.recordingMode = true
                }
            }
            RowLayout {
                Layout.fillWidth: true
                Label { text: audio.inputName; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true; color: Theme.muted; font.pixelSize: 11 }
                Glyph { symbol: "mute"; color: Theme.muted; description: qsTr("Microphone paused for others"); implicitWidth: 14; implicitHeight: 14 }
            }
            SpectrumView {
                Layout.fillWidth: true
                audio: root.audioSource
                chartHeight: root.height < 350 ? 100 : 138
                showReadouts: root.height >= 350
            }
        }
        Label {
            visible: root.settingsPage === 1
            text: qsTr("Microphone paused for others")
            color: Theme.muted; font.pixelSize: 12
        }
        ScrollView {
            objectName: "audioSettingsScroll"
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: parent.width
                spacing: 10

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.margins: 4
                    spacing: 10

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        visible: root.settingsPage === 0
                        Label {
                            visible: audio.clipped
                            text: qsTr("Clipping. Reduce microphone gain.")
                            color: Theme.danger; wrapMode: Text.Wrap; Layout.fillWidth: true
                        }
                        Meter {
                            Layout.fillWidth: true
                            caption: audio.running ? (audio.activationOpen ? qsTr("Would transmit") : qsTr("Would stay silent")) : qsTr("Voice level")
                            minimumDb: -80
                            decibels: audio.activationLevelDb
                            thresholdDb: audio.activationEnabled ? audio.activationThresholdDb : NaN
                        }
                        OptionCheck {
                            objectName: "voiceActivation"
                            enabled: audio.inputAvailable
                            text: qsTr("Voice activation")
                            checked: audio.activationEnabled
                            onCheckedChanged: {
                                if (checked === audio.activationEnabled) return
                                audio.setActivationEnabled(checked)
                                checked = Qt.binding(function() { return audio.activationEnabled })
                            }
                        }
                        SettingSlider {
                            objectName: "voiceThreshold"
                            Layout.fillWidth: true
                            visible: audio.activationEnabled
                            enabled: audio.inputAvailable
                            caption: qsTr("Voice threshold")
                            from: -80; to: 0; stepSize: 1
                            value: audio.activationThresholdDb
                            valueText: qsTr("%1 dBFS").arg(audio.activationThresholdDb.toFixed(1))
                            autoAvailable: true; automatic: audio.activationAutomatic
                            applyAutomatic: function(value) { return audio.setActivationAutomatic(value) }
                            applyValue: function(value) { return audio.setActivationThresholdDb(value) }
                        }
                        OptionCheck {
                            objectName: "echoCancellation"
                            text: qsTr("Echo cancellation")
                            enabled: audio.inputAvailable
                            checked: audio.echoCancellation
                            onCheckedChanged: {
                                if (checked === audio.echoCancellation) return
                                audio.setEchoCancellation(checked)
                                checked = Qt.binding(function() { return audio.echoCancellation })
                            }
                        }
                        Label {
                            objectName: "echoReference"
                            text: audio.echoReference
                            visible: text.length > 0
                            color: Theme.muted
                            font.pixelSize: 12
                        }
                        GridLayout {
                            Layout.fillWidth: true
                            columns: width >= 420 ? 2 : 1
                            columnSpacing: 20; rowSpacing: 8
                            SettingSlider {
                                objectName: "microphoneGain"
                                Layout.fillWidth: true; Layout.preferredWidth: 1
                                enabled: audio.inputAvailable
                                caption: qsTr("Microphone gain")
                                from: -24; to: 24; stepSize: 0.5
                                autoAvailable: true; automatic: audio.gainAutomatic
                                applyAutomatic: function(value) { return audio.setGainAutomatic(value) }
                                value: automatic ? audio.effectiveGainDb : audio.gainDb
                                valueText: qsTr("%1 dB").arg(value.toFixed(1))
                                applyValue: function(value) { return audio.setGainDb(value) }
                            }
                            SettingSlider {
                                Layout.fillWidth: true; Layout.preferredWidth: 1
                                enabled: audio.inputAvailable
                                caption: qsTr("Noise reduction")
                                from: 0; to: 1; stepSize: 0.05
                                value: audio.noiseSuppression
                                valueText: value === 0 ? qsTr("Off") : qsTr("%1 %").arg(Math.round(value * 100))
                                applyValue: function(value) { return audio.setNoiseSuppression(value) }
                            }
                            SettingSlider {
                                Layout.fillWidth: true; Layout.preferredWidth: 1
                                enabled: audio.inputAvailable
                                caption: qsTr("Low cut")
                                objectName: "microphoneLowCut"
                                autoAvailable: true; automatic: audio.highPassAutomatic
                                applyAutomatic: function(value) { return audio.setHighPassAutomatic(value) }
                                from: 0; to: Math.min(1000, audio.maximumFrequency); stepSize: 10
                                value: automatic ? audio.effectiveHighPassHz : audio.highPassHz
                                valueText: value === 0 ? qsTr("Off") : qsTr("%1 Hz").arg(Math.round(value))
                                applyValue: function(value) { return audio.setHighPassHz(value) }
                            }
                            SettingSlider {
                                Layout.fillWidth: true; Layout.preferredWidth: 1
                                enabled: audio.inputAvailable
                                caption: qsTr("High cut")
                                from: 0; to: audio.maximumFrequency; stepSize: 100
                                value: audio.lowPassHz
                                valueText: value === 0 ? qsTr("Off") : qsTr("%1 Hz").arg(Math.round(value))
                                applyValue: function(value) { return audio.setLowPassHz(value) }
                            }
                        }
                        Divider {}
                        OptionCheck {
                            objectName: "pushToTalkMode"
                            text: qsTr("Push to talk")
                            checked: root.sessionSource.pushToTalk
                            onCheckedChanged: {
                                if (checked === root.sessionSource.pushToTalk) return
                                root.sessionSource.setPushToTalk(checked)
                                checked = Qt.binding(function() { return root.sessionSource.pushToTalk })
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: root.sessionSource.pttKeyName || qsTr("Not assigned"); textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight }
                            ActionButton { text: pttKey.capturing && !pttKey.capturingRemote ? qsTr("Cancel") : qsTr("Local key"); onClicked: pttKey.capture(false) }
                            ActionButton { text: qsTr("Clear local key"); destructive: true; glyph: "close"; iconOnly: true; onClicked: pttKey.clear(false) }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Label { text: root.sessionSource.remotePttKeyName || qsTr("Not assigned"); textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight }
                            ActionButton { objectName: "chooseRemoteKey"; text: pttKey.capturingRemote ? qsTr("Cancel") : qsTr("Remote PTT key"); onClicked: pttKey.capture(true) }
                            ActionButton { text: qsTr("Clear remote key"); destructive: true; glyph: "close"; iconOnly: true; onClicked: pttKey.clear(true) }
                        }
                        Label { visible: pttKey.capturing || (!pttKey.globalAvailable && (root.sessionSource.pushToTalk || root.sessionSource.pttKeyName.length > 0 || root.sessionSource.remotePttKeyName.length > 0)); text: pttKey.status; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted }
                        ActionButton { visible: root.sessionSource.pttInputHeld; text: qsTr("Release held transmit state"); onClicked: root.sessionSource.releasePttInput() }
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        visible: root.settingsPage === 1
                        SectionLabel { text: qsTr("Your playback") }
                        ChoiceBox {
                            Layout.fillWidth: true
                            model: audio.outputs
                            textRole: "label"
                            valueRole: "deviceId"
                            currentIndex: audio.outputIndex
                            displayText: currentIndex < 0 ? qsTr("Selected device is not connected") : currentText
                            Accessible.name: qsTr("Output device")
                            onActivated: {
                                audio.selectOutput(currentValue)
                                currentIndex = Qt.binding(function() { return audio.outputIndex })
                            }
                        }
                        Label { textFormat: Text.PlainText; text: audio.outputName; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.muted }
                        SettingSlider {
                            Layout.fillWidth: true
                            enabled: audio.outputAvailable
                            caption: qsTr("Playback volume")
                            from: 0; to: 1; stepSize: 0.01
                            value: audio.outputVolume
                            valueText: qsTr("%1 %").arg(Math.round(audio.outputVolume * 100))
                            applyValue: function(value) { return audio.setOutputVolume(value) }
                        }
                        SettingSlider {
                            objectName: "musicVolume"
                            Layout.fillWidth: true
                            enabled: audio.outputAvailable
                            caption: qsTr("Music volume")
                            from: 0; to: 1; stepSize: 0.01
                            value: audio.musicVolume
                            valueText: qsTr("%1 %").arg(Math.round(audio.musicVolume * 100))
                            applyValue: function(value) { return audio.setMusicVolume(value) }
                        }
                        OptionCheck { text: qsTr("Level audio automatically"); checked: audio.automaticVolume; onClicked: { audio.setAutomaticVolume(checked); checked = Qt.binding(function() { return audio.automaticVolume }) } }
                        ActionButton { text: qsTr("Test output"); enabled: audio.outputAvailable; onClicked: audio.testOutput() }
                    }
                    Label {
                        textFormat: Text.PlainText
                        text: audio.status
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        color: Theme.muted
                    }
                }
            }
        }
    }
}
