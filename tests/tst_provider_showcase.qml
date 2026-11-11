import QtQuick
import QtTest
import "../ui"

// Explicit showcase run against isolated real servers. This is not part of the
// portable controls suite, whose execution does not require external servers.
TestCase {
    id: test
    name: "ProviderWorkspaceShowcase"
    when: windowShown
    property int sentSpeechFrames: 0
    Channels { id: view; visible: true; width: 1120; height: 760 }
    SignalSpy { id: receivedAudioPackets; target: channel; signalName: "audioReceived" }
    Timer {
        id: speechFrames
        interval: 20
        repeat: true
        running: false
        onTriggered: {
            if (fixtures.sendShowcaseSpeechFrame()) ++test.sentSpeechFrames
        }
    }

    function visualChild(parent, name) {
        if (!parent) return null
        if (parent.objectName === name) return parent
        for (const child of parent.children || []) {
            const result = visualChild(child, name)
            if (result) return result
        }
        return null
    }
    function setHostExpanded(groupId, expanded) {
        const toggle = visualChild(view.contentItem, "toggleHost_" + groupId)
        verify(toggle, "The host group has a visible UI toggle")
        if (toggle.Accessible.checked !== expanded)
            verify(fixtures.pressAccessible(toggle), "Set the host group expansion through its UI control")
        tryVerify(function() { return toggle.Accessible.checked === expanded })
    }
    function test_realProvidersShareTheWorkspace() {
        verify(providerShowcase.teamspeak.length > 0 && providerShowcase.mumble.length > 0,
            "Supply isolated TeamSpeak and Mumble endpoints through SQUAD_SHOWCASE_*_ENDPOINT")
        verify(session.setUserName("Alex"))
        verify(session.setAvatar("ember-dragon"))
        verify(remoteSession.setUserName("Mira"))
        verify(remoteSession.setAvatar("moon-owl"))
        verify(fixtures.setSupporter(true))
        verify(fixtures.startHost())
        verify(channel.setChannelName("Campfire"))
        const games = channel.addOwnedChannel("Game night")
        const workshop = channel.addOwnedChannel("Workshop")
        verify(games.length > 0 && workshop.length > 0)
        const previousOutputVolume = audio.outputVolume
        const previousRemoteMuted = remoteSession.muted
        try {
            if (audio.outputAvailable) verify(audio.setOutputVolume(0))
            verify(channel.addMumble(providerShowcase.mumble, providerShowcase.mumblePassword))
            tryVerify(function() {
                const hosts = channel.savedChannels.filter(function(host) { return host.protocol === "mumble" })
                return channel.joined && hosts.length >= 3 && hosts.some(function(host) { return host.online })
            }, 20000, "The local Mumble server supplies an online channel tree")
            verify(channel.addTeamSpeak(providerShowcase.teamspeak, providerShowcase.teamspeakPassword))
            tryVerify(function() {
                const hosts = channel.savedChannels.filter(function(host) { return host.protocol === "teamspeak" })
                return channel.joined && hosts.length >= 3 && hosts.some(function(host) { return host.online })
            }, 20000, "The local TeamSpeak server must provide online channels. Status: " + channel.status)
            verify(channel.decide(remoteChannel.ownId, true))
            verify(remoteSession.setMuted(false))
            verify(channel.joinSaved(channel.ownId))
            verify(remoteChannel.join(channel.ownId, "127.0.0.1", channel.servicePort))
            tryVerify(function() { return channel.chatReady && remoteChannel.chatReady })
            verify(channel.openPrivateChat(channel.ownId, remoteChannel.ownId))
            verify(remoteChannel.openPrivateChat(channel.ownId, channel.ownId))
            tryVerify(function() { return channel.chatReady && channel.privateChat && remoteChannel.chatReady && remoteChannel.privateChat })
            verify(remoteChannel.sendChat("Saved you a seat by the campfire."))
            tryCompare(remoteChannel, "chatPending", false)
            view.openHostChat(channel.ownId)
            verify(remoteChannel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
            tryVerify(function() { return channel.chatReady && !channel.privateChat && remoteChannel.chatReady && !remoteChannel.privateChat })
            verify(channel.sendChat("## Tonight at the campfire\n\n**Good company. No rush.**\n\nWe can meet here, then find a room for game night."))
            tryCompare(channel, "chatPending", false)
            verify(remoteChannel.sendChat("I'm in! **Game night** sounds good. I saved the other servers for later."))
            tryCompare(remoteChannel, "chatPending", false)
            view.openHostChat(channel.ownId)
            verify(fixtures.bindAudioToChannel())
            verify(fixtures.bindShowcaseAudioToChannel())
            verify(!audio.outputAvailable || audio.outputVolume === 0,
                "The received test audio is silent at the local output")
            tryVerify(function() { return channel.participants.some(function(member) {
                return member.id === remoteChannel.ownId && member.available && !member.muted
            }) })
            receivedAudioPackets.clear()
            verify(fixtures.sendShowcaseSpeechFrame(), "The remote test participant sends an encoded voice frame")
            tryVerify(function() { return receivedAudioPackets.count > 0 }, 5000,
                "The host receives the test frame over the real channel transport")
            tryVerify(function() { return audio.playbackLevels[remoteChannel.ownId] > 0.001 }, 5000,
                "The host AudioModel decodes incoming test speech")
            speechFrames.start()
            const speakers = findChild(view, "activeSpeakers")
            tryCompare(speakers, "count", 1, 5000)
            const tile = findChild(speakers, "speaker_" + remoteChannel.ownId)
            verify(tile, "The decoded speaker appears in the active voice strip")
            const speakerAvatar = findChild(tile, "participantAvatar")
            verify(speakerAvatar, "The speaker strip renders Mira's avatar")
            tryVerify(function() { return speakerAvatar.level > 0.001 }, 10000,
                "The decoded level reaches the speaker avatar")
            tryVerify(function() { return speakerAvatar.speaking }, 5000,
                "The decoded level activates the visible speaking state")
            const external = channel.savedChannels.filter(function(host) {
                return host.protocol === "teamspeak" || host.protocol === "mumble"
            })
            for (const protocol of ["teamspeak", "mumble"]) {
                const hosts = external.filter(function(host) { return host.protocol === protocol })
                verify(hosts.length >= 3)
                verify(hosts.some(function(host) {
                    return host.parent && hosts.some(function(parent) { return parent.id === host.parent })
                }), "The " + protocol + " server supplies nested channels")
                const serverId = String(hosts[0].deviceId)
                tryVerify(function() { return visualChild(view.contentItem, "hostGroup_" + serverId) !== null })
                const icon = visualChild(view.contentItem, "hostProvider_" + serverId)
                verify(icon)
                compare(icon.provider, protocol)
                for (const host of hosts) verify(view.channelList.some(function(entry) { return entry.id === host.id }),
                    "The channel list retains " + protocol + " channel " + host.name)
            }
            for (const state of [{tag: "light", width: 1120, height: 760, theme: "light", teamspeak: true, mumble: false},
                                 {tag: "dark", width: 1120, height: 760, theme: "dark", teamspeak: false, mumble: true},
                                 {tag: "compact", width: 390, height: 760, theme: "light", teamspeak: false, mumble: false}]) {
                view.width = state.width; view.height = state.height
                verify(session.setTheme(state.theme))
                view.chatExpanded = true
                if (state.width >= 840) {
                    setHostExpanded(channel.ownId, true)
                    tryVerify(function() {
                        const folder = visualChild(view.contentItem, "channelName_private-folder:" + channel.ownId)
                        return folder && folder.visible
                    }, 5000,
                        "The Messages folder is visible under Alex's host")
                    if (!view.privateFolderIsExpanded(channel.ownId)) view.togglePrivateFolder(channel.ownId)
                    tryVerify(function() { return view.privateFolderIsExpanded(channel.ownId) })
                    for (const protocol of ["teamspeak", "mumble"]) {
                        const hosts = external.filter(function(host) { return host.protocol === protocol })
                        setHostExpanded(String(hosts[0].deviceId), state[protocol])
                    }
                }
                view.contentItem.forceActiveFocus(Qt.OtherFocusReason)
                view.update()
                verify(waitForRendering(view.contentItem))
                const panel = visualChild(view.contentItem, "chatPanel")
                tryVerify(function() { return panel && panel.visible && !panel.showingEmptyContent })
                tryVerify(function() { return test.sentSpeechFrames > 50 }, 5000,
                    "Encoded speech continues while the conversation is visible")
                tryVerify(function() { return audio.playbackLevels[remoteChannel.ownId] > 0.001 }, 5000,
                    "The incoming speech level continues while the conversation is visible")
                const activeSpeakers = findChild(view, "activeSpeakers")
                tryCompare(activeSpeakers, "count", 1, 5000)
                const currentSpeakerTile = findChild(activeSpeakers, "speaker_" + remoteChannel.ownId)
                const currentSpeakerAvatar = currentSpeakerTile && findChild(currentSpeakerTile, "participantAvatar")
                verify(currentSpeakerAvatar, "The resized speaker strip retains Mira's avatar")
                tryVerify(function() {
                    return currentSpeakerTile.visible && currentSpeakerAvatar.speaking
                }, 5000, "The visible chat includes Mira's speaking portrait at " + state.width
                    + "px with decoded voice activity")
                verify(waitForRendering(view.contentItem))
                if (imageDirectory.length > 0) verify(fixtures.saveWindow(view,
                    imageDirectory + "/providers-" + state.tag + ".png"))
            }
        } finally {
            speechFrames.stop()
            fixtures.unbindShowcaseAudioFromChannel()
            fixtures.unbindAudioFromChannel()
            if (audio.outputAvailable) audio.setOutputVolume(previousOutputVolume)
            remoteSession.setMuted(previousRemoteMuted)
            remoteChannel.leave()
            channel.leave()
            const externalIds = channel.savedChannels.filter(function(host) {
                return host.protocol === "teamspeak" || host.protocol === "mumble"
            }).map(function(host) { return host.id })
            for (const id of externalIds) channel.removeChannel(id)
            for (const id of [games, workshop]) channel.removeOwnedChannel(id)
            channel.stopHost()
        }
    }
}
