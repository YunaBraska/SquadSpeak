import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Item {
    id: root
    readonly property var audioSource: audio
    property var sessionSource: session
    property int settingsPage: 0
    onSettingsPageChanged: {
        settingsScroll.contentItem.cancelFlick()
        settingsScroll.contentItem.contentY = 0
    }
    signal remoteSetupRequested()
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


    ScrollView {
        id: settingsScroll
        objectName: "audioSettingsScroll"
        anchors.fill: parent
        contentWidth: availableWidth
        clip: true
        WheelHandler {
            target: null
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: function(event) {
                const flick = settingsScroll.contentItem
                const delta = event.pixelDelta.y || event.angleDelta.y / 120 * 48
                flick.cancelFlick()
                flick.contentY = Math.max(0, Math.min(flick.contentY - delta, flick.contentHeight - flick.height))
            }
        }
        ColumnLayout {
            width: parent.width
            spacing: 10
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
                }
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: audio.inputName; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true; color: Theme.muted; font.pixelSize: 11 }
                    Glyph { symbol: "mute"; color: Theme.muted; description: qsTr("Microphone paused for others"); implicitWidth: 14; implicitHeight: 14 }
                }
                SpectrumView {
                    Layout.fillWidth: true
                    audio: root.audioSource
                    chartHeight: root.height < 350 ? 120 : 166
                    showReadouts: root.height >= 350
                }
                Meter {
                    objectName: "voiceMeter"
                    Layout.fillWidth: true
                    caption: audio.running ? (audio.activationOpen ? qsTr("Would transmit") : qsTr("Would stay silent")) : qsTr("Voice level")
                    minimumDb: -80
                    decibels: audio.activationLevelDb
                    thresholdDb: audio.activationEnabled ? audio.activationThresholdDb : NaN
                    barColor: audio.activationOpen ? Theme.accent : Theme.muted
                    ToolTip.visible: gateHelp.hovered
                    ToolTip.text: qsTr("Microphone paused for others") + "\n"
                        + (audio.activationAutomatic ? qsTr("Auto also checks for speech, not just volume.")
                                                     : qsTr("Voice threshold") + ": " + qsTr("%1 dBFS").arg(audio.activationThresholdDb.toFixed(1)))
                    HoverHandler { id: gateHelp }
                }
            }
            Label {
                visible: root.settingsPage === 1
                text: qsTr("Microphone paused for others")
                color: Theme.muted; font.pixelSize: 12
            }
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
                    SettingSlider {
                        objectName: "echoCancellation"
                        Layout.fillWidth: true
                        caption: qsTr("Echo cancellation")
                        enabled: audio.inputAvailable
                        from: 0; to: 1; stepSize: 0.05
                        value: audio.echoStrength
                        valueText: value === 0 ? qsTr("Off") : qsTr("%1 %").arg(Math.round(value * 100))
                        autoAvailable: true; automatic: audio.echoAutomatic
                        applyAutomatic: function(value) { return audio.setEchoAutomatic(value) }
                        applyValue: function(value) { return audio.setEchoStrength(value) }
                        ToolTip.visible: echoHelp.hovered
                        ToolTip.text: qsTr("Strength, not the percentage of echo removed.")
                        HoverHandler { id: echoHelp }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: audio.echoReference.length > 0
                        Label {
                            objectName: "echoReference"
                            text: audio.echoReference
                            color: Theme.muted
                            font.pixelSize: 12
                            Layout.fillWidth: true
                        }
                        ActionButton {
                            objectName: "retryEchoReference"
                            text: qsTranslate("Channels", "Retry")
                            visible: audio.echoRetryAvailable
                            onClicked: audio.retryEchoReference()
                        }
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
                RowLayout {
                    Layout.fillWidth: true
                    OptionCheck {
                        objectName: "pushToTalkMode"
                        text: qsTr("Push to talk")
                        Layout.fillWidth: true
                        checked: root.sessionSource.pushToTalk
                        onCheckedChanged: {
                            if (checked === root.sessionSource.pushToTalk) return
                            root.sessionSource.setPushToTalk(checked)
                            checked = Qt.binding(function() { return root.sessionSource.pushToTalk })
                        }
                    }
                    ActionButton {
                        objectName: "configureRemoteControl"
                        text: qsTranslate("Channels", "Remote control")
                        glyph: "remote"
                        onClicked: root.remoteSetupRequested()
                    }
                }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: root.sessionSource.pttKeyName || qsTr("Not assigned"); textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight }
                        ActionButton { objectName: "choosePttKey"; text: pttKey.capturing ? qsTr("Cancel") : qsTr("PTT key"); onClicked: pttKey.capture() }
                        ActionButton { text: qsTr("Clear key"); destructive: true; glyph: "close"; iconOnly: true; onClicked: pttKey.clear() }
                    }
                    Label { visible: pttKey.capturing || (!pttKey.globalAvailable && (root.sessionSource.pushToTalk || root.sessionSource.pttKeyName.length > 0)); text: pttKey.status; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted }
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
                    ActionButton {
                        objectName: "enableOutput"
                        visible: root.sessionSource.deafened
                        text: qsTranslate("Channels", "Enable speakers")
                        glyph: "deafen"
                        onClicked: root.sessionSource.setDeafened(false)
                    }
                    SettingSlider {
                        objectName: "playbackVolume"
                        Layout.fillWidth: true
                        enabled: audio.outputAvailable
                        caption: qsTr("Voice volume")
                        from: 0; to: 1; stepSize: 0.01
                        value: audio.outputVolume
                        valueText: qsTr("%1 %").arg(Math.round(audio.outputVolume * 100))
                        applyValue: function(value) { return audio.setOutputVolume(value) }
                        onEditingFinished: audio.previewOutput(audio.outputVolume)
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
                        onEditingFinished: audio.previewOutput(audio.musicVolume)
                    }
                    SettingSlider {
                        objectName: "notificationVolume"
                        Layout.fillWidth: true
                        enabled: audio.outputAvailable
                        caption: qsTr("App sounds")
                        from: 0; to: 1; stepSize: 0.01
                        value: audio.notificationVolume
                        valueText: qsTr("%1 %").arg(Math.round(audio.notificationVolume * 100))
                        applyValue: function(value) { return audio.setNotificationVolume(value) }
                        onEditingFinished: audio.previewOutput(audio.notificationVolume)
                    }
                    OptionCheck {
                        objectName: "readBotMessages"
                        visible: audio.speechAvailable
                        enabled: audio.outputAvailable
                        text: qsTr("Read bot messages aloud")
                        checked: audio.readBotMessages
                        onClicked: { audio.setReadBotMessages(checked); checked = Qt.binding(function() { return audio.readBotMessages }) }
                    }
                    ChoiceBox {
                        id: botVoiceChoice
                        objectName: "botVoiceChoice"
                        Layout.fillWidth: true
                        visible: audio.readBotMessages
                        enabled: audio.outputAvailable
                        Accessible.name: qsTr("Bot voice")
                        textRole: "label"
                        model: {
                            const voices = audio.language.voices.filter(v => !v.personal)
                                .map(v => ({ id: v.id, label: v.name + " (" + v.language + ")" }))
                                .sort((a, b) => a.label.localeCompare(b.label))
                            if (audio.botVoiceId && !voices.some(v => v.id === audio.botVoiceId))
                                voices.unshift({ id: audio.botVoiceId, label: qsTr("Unavailable") })
                            return [{ id: "", label: qsTr("Automatic") }].concat(voices)
                        }
                        currentIndex: model.findIndex(v => v.id === audio.botVoiceId)
                        onActivated: function(index) {
                            audio.setBotVoice(model[index].id)
                            currentIndex = Qt.binding(function() { return botVoiceChoice.model.findIndex(v => v.id === audio.botVoiceId) })
                        }
                    }
                    Label {
                        objectName: "botVoiceName"
                        Layout.fillWidth: true
                        visible: audio.readBotMessages && !audio.botVoiceId && !!audio.botVoice.name
                        text: audio.botVoice.name ? audio.botVoice.name + " (" + audio.botVoice.language + ")" : ""
                        textFormat: Text.PlainText
                        wrapMode: Text.Wrap
                        color: Theme.muted
                    }
                    Label {
                        objectName: "botVoiceDownloadHint"
                        Layout.fillWidth: true
                        visible: audio.readBotMessages && (audio.botVoiceId ? !audio.botVoice.id : !(audio.botVoice.quality >= 2))
                        text: Qt.platform.os === "ios"
                            ? qsTr("Download voices in your device's Accessibility speech settings.")
                            : qsTr("Download a high-quality voice for this language in macOS settings.")
                        wrapMode: Text.Wrap
                        color: Theme.muted
                    }
                    ActionButton {
                        objectName: "botVoiceSettings"
                        visible: audio.readBotMessages
                        text: Qt.platform.os === "ios" ? qsTr("Voice download help") : qsTr("Open system settings")
                        onClicked: audio.language.openSettings(true)
                    }
                    OptionCheck { text: qsTr("Level audio automatically"); checked: audio.automaticVolume; onClicked: { audio.setAutomaticVolume(checked); checked = Qt.binding(function() { return audio.automaticVolume }) } }
                    ActionButton { text: qsTr("Test output"); enabled: audio.outputAvailable; onClicked: audio.testOutput() }
                }
                Label {
                    objectName: "audioStatus"
                    textFormat: Text.PlainText
                    text: root.settingsPage === 0 ? audio.inputStatus : audio.outputStatus
                    visible: text.length > 0
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                    color: Theme.muted
                }
            }
        }
    }
}
