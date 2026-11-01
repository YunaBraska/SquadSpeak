import QtQuick
import QtTest
import "../ui"

// Registered only on macOS 14.2+, where Core Audio exposes per-process streams.
TestCase {
    name: "NativeAudioOutput"
    when: windowShown
    Channels { id: view; visible: true }

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
