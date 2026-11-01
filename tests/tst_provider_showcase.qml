import QtQuick
import QtTest
import "../ui"

// Explicit showcase run against isolated real servers. This is not part of the
// portable controls suite, whose execution does not require external servers.
TestCase {
    id: test
    name: "ProviderWorkspaceShowcase"
    when: windowShown
    Channels { id: view; visible: true; width: 1120; height: 760 }

    function visualChild(parent, name) {
        if (!parent) return null
        if (parent.objectName === name) return parent
        for (const child of parent.children || []) {
            const result = visualChild(child, name)
            if (result) return result
        }
        return null
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
        try {
            verify(channel.addMumble(providerShowcase.mumble, providerShowcase.mumblePassword))
            tryVerify(function() { return channel.joined && channel.savedChannels.filter(function(host) {
                return host.protocol === "mumble"
            }).length >= 3 }, 20000, "The real Mumble server supplies its channel tree")
            verify(channel.addTeamSpeak(providerShowcase.teamspeak, providerShowcase.teamspeakPassword))
            tryVerify(function() { return channel.joined && channel.savedChannels.filter(function(host) {
                return host.protocol === "teamspeak"
            }).length >= 3 }, 20000, "The real TeamSpeak server supplies its channel tree")
            verify(channel.decide(remoteChannel.ownId, true))
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
                for (const host of hosts) verify(visualChild(view.contentItem, "channelRow_" + host.id))
            }
            for (const state of [{tag: "light", width: 1120, height: 760, theme: "light"},
                                 {tag: "dark", width: 1120, height: 760, theme: "dark"},
                                 {tag: "compact", width: 390, height: 760, theme: "light"}]) {
                view.width = state.width; view.height = state.height
                verify(session.setTheme(state.theme))
                view.chatExpanded = state.width >= 840
                view.contentItem.forceActiveFocus(Qt.OtherFocusReason)
                view.update()
                verify(waitForRendering(view.contentItem))
                for (const id of [channel.ownId, remoteChannel.ownId]) {
                    tryVerify(function() {
                        const portrait = visualChild(view.contentItem, "channelAvatar_" + channel.ownId + "_" + id)
                        return portrait && fixtures.portraitHasDetail(portrait)
                    }, 5000, "Participant artwork is loaded before capturing the workspace")
                }
                if (imageDirectory.length > 0) verify(fixtures.saveWindow(view,
                    imageDirectory + "/providers-" + state.tag + ".png"))
            }
        } finally {
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
