import QtQuick
import QtTest
import "../ui"

// Registered only on macOS 14.2+, where Core Audio exposes per-process streams.
TestCase {
    name: "NativeAudioOutput"
    when: windowShown
    Channels { id: view; visible: true }
    SignalSpy { id: progress; target: audio; signalName: "notificationProgress" }
    SignalSpy { id: stopped; target: fixtures; signalName: "feedbackStopped" }
    Connections {
        target: audio
        function onNotificationProgress(kind, phase, text) {
            console.info("Notification", kind, phase, "native output", fixtures.nativeAudioOutputRunning())
        }
    }

    function test_startJoinLeaveAndQuitSpeakAfterTheirCue() {
        verify(audio.outputAvailable && audio.speechAvailable)
        const volume = audio.notificationVolume
        const readBot = audio.readBotMessages
        const events = session.eventSounds
        const muted = session.muted
        const deafened = session.deafened
        const language = session.language
        try {
            verify(session.setLanguage("en"))
            verify(session.setMuted(true)); verify(session.setDeafened(true))
            verify(audio.setNotificationVolume(0.01))
            verify(audio.setReadBotMessages(true))
            verify(audio.botVoice.id !== undefined)
            verify(fixtures.bindAudioToChannel())
            verify(fixtures.bindFeedback())
            progress.clear(); stopped.clear()
            verify(session.setEventSounds(false))
            verify(!fixtures.startFeedback())
            compare(progress.count, 0)
            verify(session.setEventSounds(true))
            verify(fixtures.startFeedback())
            verify(fixtures.startHost())
            verify(channel.joinSaved(channel.ownId))
            tryCompare(channel, "joined", true)
            verify(channel.leave())
            tryCompare(channel, "joined", false)
            tryCompare(progress, "count", 9, 15000)
            const kinds = ["start", "join", "leave"]
            for (let i = 0; i < kinds.length; ++i) {
                compare(progress.signalArguments[i * 3][0], kinds[i])
                compare(progress.signalArguments[i * 3][1], "tone")
                compare(progress.signalArguments[i * 3 + 1][1], "speech")
                compare(progress.signalArguments[i * 3 + 2][1], "finished")
                verify(progress.signalArguments[i * 3 + 1][2].length > 0)
            }
            verify(["Hey, you're back.", "Ready when you are.", "Let's talk."].includes(
                progress.signalArguments[1][2]), "Startup uses a short greeting")
            verify(session.setLanguage("de"))
            fixtures.shutdownFeedback()
            tryCompare(stopped, "count", 1, 7000)
            compare(progress.count, 12, "Quit completes its cue and speech before shutdown")
            compare(progress.signalArguments[9][0], "quit")
            compare(progress.signalArguments[10][1], "speech")
            compare(progress.signalArguments[11][1], "finished")
            verify(["Bis später.", "Ich bin dann weg.", "Bis zum nächsten Mal."].includes(
                progress.signalArguments[10][2]), "Goodbye follows the current app language")
            fixtures.shutdownFeedback()
            compare(stopped.count, 1)
            verify(!audio.running && !channel.joined)
            compare(audio.outputStatus, "")
        } finally {
            fixtures.unbindFeedback()
            fixtures.unbindAudioFromChannel()
            channel.leave(); channel.stopHost()
            audio.setReadBotMessages(readBot); audio.setNotificationVolume(volume)
            session.setEventSounds(events); session.setMuted(muted); session.setDeafened(deafened)
            session.setLanguage(language)
        }
    }

    function test_appSoundsAndBotSpeechWhileVoiceIsMuted() {
        verify(audio.outputAvailable)
        const deafened = session.deafened
        const muted = session.muted
        const volume = audio.notificationVolume
        const readBot = audio.readBotMessages
        try {
            verify(session.setMuted(true))
            verify(session.setDeafened(true))
            verify(audio.setNotificationVolume(0.05))
            verify(fixtures.setAudioVoiceState(true, false))
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000)
            for (const kind of ["join", "leave"]) {
                verify(fixtures.playEvent(kind, true))
                tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 1 }, 1000,
                    kind + " must reach native output while microphone and voice output are muted")
                fixtures.cancelNotifications()
                tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000)
            }
            verify(audio.setReadBotMessages(true))
            progress.clear()
            verify(fixtures.queueBotMessage("Hello."))
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 1 }, 1000,
                "Bot speech must open output without unmuting voice")
            verify(!fixtures.queueTestSpeechFrame(), "Remote speech remains rejected during the bot announcement")
            tryVerify(function() { return progress.count === 3 && fixtures.nativeAudioOutputRunning() === 0 }, 10000,
                "Bot speech must drain and release output without leaving a silent stream open")
            compare(progress.signalArguments[1][1], "speech")
            compare(progress.signalArguments[2][1], "finished")
            compare(audio.outputStatus, "")
            verify(session.muted && session.deafened)
            verify(!audio.running, "Notifications must not activate the microphone")
            progress.clear()
            verify(fixtures.queueBotMessage("This announcement will be cancelled while it is being read."))
            tryCompare(progress, "count", 2, 1000)
            compare(progress.signalArguments[1][1], "speech")
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 1 }, 1000)
            verify(audio.setNotificationVolume(0))
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                "Cancelling an announcement must close output without draining cancelled speech")
            verify(!fixtures.playEvent("join", true))
            verify(!fixtures.queueBotMessage("Hello."))
            compare(fixtures.nativeAudioOutputRunning(), 0)
        } finally {
            fixtures.cancelNotifications()
            fixtures.unbindAudioFromChannel()
            audio.setReadBotMessages(readBot)
            audio.setNotificationVolume(volume)
            session.setMuted(muted)
            session.setDeafened(deafened)
        }
    }

    function test_deafenReleasesNativeTestToneOutput() {
        verify(audio.outputAvailable, "Native audio acceptance requires an output device")
        const outputState = fixtures.nativeAudioOutputRunning()
        verify(outputState >= 0, "Core Audio must report the output state on a supported system")
        const previousDeafened = audio.deafened
        const previousEvents = session.eventSounds
        const previousOutputVolume = audio.outputVolume
        try {
            verify(session.setEventSounds(false))
            verify(audio.setOutputVolume(0))
            verify(audio.stop())
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                "AudioModel::stop must leave no native output stream open before the test tone")
            verify(audio.testOutput())
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 1 }, 1000,
                "The test tone must open a native output stream")
            verify(session.setDeafened(true))
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                "Deafen must release the test-tone output stream")
            verify(!audio.testOutput(), "The test tone cannot start while deafened")
            verify(session.setDeafened(false))
            verify(audio.testOutput())
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 1 }, 1000,
                "The test tone can start again after undeafening")
        } finally {
            session.setDeafened(previousDeafened)
            audio.setOutputVolume(previousOutputVolume)
            audio.stop()
            session.setEventSounds(previousEvents)
        }
    }

    function test_deafenReleasesNativeVoiceOutputAndResumesCleanly() {
        verify(audio.outputAvailable, "Native audio acceptance requires an output device")
        const outputState = fixtures.nativeAudioOutputRunning()
        verify(outputState >= 0, "Core Audio must report the output state on a supported system")
        const previousDeafened = session.deafened
        const previousMuted = session.muted
        const previousEvents = session.eventSounds
        try {
            verify(session.setEventSounds(false))
            verify(session.setDeafened(false))
            verify(session.setMuted(true))
            verify(audio.stop())
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                "AudioModel::stop must leave no native output stream open before joining")
            verify(fixtures.bindAudioToChannel())
            verify(fixtures.startHost())
            verify(channel.joinSaved(channel.ownId))
            tryCompare(channel, "joined", true)
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 1 }, 2000,
                "Joining must create the native voice output stream")
            for (let frame = 0; frame < 6; ++frame) verify(fixtures.queueTestSpeechFrame())
            tryVerify(function() { return audio.playbackLevels["deafen-test-peer"] > 0 }, 1000,
                "The test speech reaches the active mixer before deafen")

            verify(fixtures.pressAccessible(findChild(view, "deafen")))
            tryCompare(session, "deafened", true)
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                "Deafen must release local voice output")
            verify(channel.joined, "Deafen keeps the voice connection alive")
            compare(audio.outputStatus, "", "Deafen does not report an output failure")
            compare(audio.playbackLevels["deafen-test-peer"], 0, "Deafen clears previously queued speech")
            verify(!fixtures.queueTestSpeechFrame(), "Speech arriving while deafened is discarded")

            verify(fixtures.pressAccessible(findChild(view, "deafen")))
            tryCompare(session, "deafened", false)
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 1 }, 2000,
                "Undeafen must recreate local voice output")
            compare(audio.playbackLevels["deafen-test-peer"], 0, "Undeafen does not replay queued speech")
            verify(fixtures.pressAccessible(findChild(view, "deafen")))
            tryCompare(session, "deafened", true)
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                "Repeated deafen must release the recreated output")

            const outputs = audio.outputs
            const previousIndex = audio.outputIndex
            const restoreDevice = previousIndex > 0 && previousIndex < outputs.length
                ? outputs[previousIndex].deviceId : ""
            const alternate = outputs.find(function(device) {
                return device.deviceId.length > 0 && device.label !== audio.outputName
            })
            if (alternate) {
                verify(audio.selectOutput(alternate.deviceId))
                tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                    "Changing the selected output while deafened must not reopen the sink")
                verify(audio.selectOutput(restoreDevice))
                tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                    "Restoring the output while deafened must keep the sink closed")
                verify(channel.joined, "Changing output while deafened keeps the voice connection alive")
                compare(audio.outputStatus, "", "Output changes while deafened do not report a playback failure")
            }

            verify(fixtures.pressAccessible(findChild(view, "deafen")))
            tryCompare(session, "deafened", false)
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 1 }, 2000,
                "Undeafen after an output change must recreate the sink")
            compare(audio.outputStatus, "", "Undeafen does not report a reconnect failure")
            channel.leave()
            tryCompare(channel, "joined", false)
            tryVerify(function() { return fixtures.nativeAudioOutputRunning() === 0 }, 1000,
                "Leaving must release local voice output")
        } finally {
            channel.leave()
            fixtures.unbindAudioFromChannel()
            channel.stopHost()
            session.setDeafened(previousDeafened)
            session.setMuted(previousMuted)
            session.setEventSounds(previousEvents)
        }
    }
}
