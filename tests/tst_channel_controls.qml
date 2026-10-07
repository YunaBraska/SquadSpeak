import QtQuick
import QtQuick.Controls
import QtTest
import "../ui"
import "../ui/AvatarAtlas.js" as Atlas

TestCase {
    id: test
    name: "DirectChannelControls"
    when: windowShown
    width: 560
    height: 760
    readonly property var view: scene.item ? scene.item.view : null
    readonly property var controllerView: scene.item ? scene.item.controllerView : null
    readonly property var portrait: scene.item ? scene.item.portrait : null
    readonly property var portraitGallery: scene.item ? scene.item.portraitGallery : null
    readonly property var galleryPortraits: scene.item ? scene.item.galleryPortraits : null
    SignalSpy { id: previewRequests; target: view; signalName: "previewRequested" }
    SignalSpy { id: openedLinks; target: fixtures; signalName: "externalUrlOpened" }
    SignalSpy { id: dialogRejections; signalName: "rejected" }
    Component { id: variablePortrait; VoiceAvatar { width: 96; height: 112 } }
    Component { id: atlasImage; Image { visible: false } }
    Component { id: literalName; Text { textFormat: Text.PlainText } }
    QtObject {
        id: updateDisplay
        property bool available: false
        property int checks: 0
        function check() { ++checks; return true }
    }
    QtObject {
        id: captureDisplay
        property bool active: false
        property bool watching: false
        property bool audioAvailable: true
        property bool audioEnabled: false
        property bool computerAudio: true
        property string hostId: ""
        property string error: ""
        property var sources: []
        property int selectedSource: -1
        function start(index, hostId) { selectedSource = index; return true }
        function refreshSources() { return true }
        function setAudioEnabled(enabled) { audioEnabled = enabled; return true }
    }
    QtObject {
        id: licenseDisplay
        readonly property bool directDistribution: supporterLicense.directDistribution
        readonly property bool configured: true
        readonly property bool active: false
        readonly property bool busy: false
        readonly property bool pending: false
        readonly property bool recoveryNeeded: false
        readonly property string status: ""
        readonly property string supportReference: "Order 50 / License 40"
        readonly property url purchaseUrl: "https://example.lemonsqueezy.com/checkout/buy/example"
    }
    Loader {
        id: scene
        sourceComponent: Item {
            property alias view: view
            property alias controllerView: controllerView
            property alias portrait: portrait
            property alias portraitGallery: portraitGallery
            property alias galleryPortraits: galleryPortraits

            Channels { id: view; visible: true }
            Channels { id: controllerView; screen: null; network: remoteChannel; voice: remoteSession; visible: false }
            VoiceAvatar { id: portrait; parent: view.contentItem; visible: false; width: 96; height: 112; name: "Testperson" }

            Rectangle {
                id: portraitGallery
                property bool online: true
                parent: view.contentItem
                anchors.fill: parent; visible: false; z: 100; color: Theme.surface
                Grid {
                    anchors.top: parent.top; anchors.left: parent.left; anchors.margins: 8
                    columns: 5; spacing: 6
                    Repeater {
                        id: galleryPortraits
                        model: portraitGallery.visible ? session.avatars : []
                        Column {
                            required property string modelData
                            width: (portraitGallery.width - 40) / 5; spacing: 6
                            VoiceAvatar { width: parent.width; height: width; avatar: modelData; animated: false; circular: true; online: portraitGallery.online }
                            Text { width: parent.width; text: modelData.split("-").map(function(word) { return word.charAt(0).toUpperCase() + word.slice(1) }).join(" "); color: Theme.text; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap }
                        }
                    }
                }
            }
        }
    }


    function init() {
        verify(fixtures.setSupporter(false))
        verify(fixtures.expireChat())
        view.show()
        const shelf = findChild(view, "avatarShelf")
        shelf.currentIndex = session.avatars.indexOf(session.avatar)
        shelf.positionViewAtIndex(shelf.currentIndex, PathView.Center)
        session.setAnimatedAvatars(true)
        findChild(view, "channelsScroll").contentItem.contentY = 0
        session.setMuted(true)
        session.setPttButtonHeld(false)
        session.setPushToTalk(false)
        waitForRendering(view.contentItem)

    }
    function cleanup() {
        for (const window of [view, controllerView]) {
            for (const action of ["autoJoinMenuItem", "inspectMember"]) {
                const menu = findChild(window, action).menu
                menu.close()
                tryCompare(menu, "visible", false)
            }
        }
        findChild(view, "screenPicker").close()
        view.screen = screenShare
        view.supporter = supporterLicense
        view.updates = null
        session.setLanguage("en")
        portrait.online = true; portrait.avatar = "mossling"; portrait.systemMessage = false; portrait.level = 0
        portrait.visible = false
        portraitGallery.visible = false
        portraitGallery.online = true
        controllerView.close()
        controllerView.chatExpanded = false
        remoteSession.releasePttInput()
        for (const device of channel.controllers) channel.decideControl(device.id, false)
        remoteChannel.clearControlTarget()
        view.chatExpanded = false
        findChild(view, "joinPasswordDialog").close()
        findChild(view, "hostPasswordDialog").close()
        findChild(view, "discoverDialog").close()
        findChild(view, "settingsDialog").close()
        findChild(view, "channelInfoDialog").close()
        const requestInfo = findChild(view, "requestInfoDialog"); if (requestInfo) requestInfo.close()
        findChild(view, "stationEditor").close()
        findChild(view, "radioBrowser").close()
        findChild(view, "newOwnChannelDialog").close()
        findChild(view, "removeOwnChannelDialog").close()
        for (const owner of channel.ownedChannels) if (owner.id !== channel.ownId) verify(channel.removeOwnedChannel(owner.id))
        radio.cancelStationCheck()
        session.setAudioSettingsOpen(false)
        session.setAudioTestActive(false)
        channel.setHostPassword("")
        remoteChannel.setHostPassword("")
        chatContent.clearImage()
        for (const host of channel.savedChannels) channel.closeChat(host.id)
        for (const host of remoteChannel.savedChannels) remoteChannel.closeChat(host.id)
        remoteChannel.leave()
        remoteChannel.stopHost()
        channel.leave()
        channel.stopHost()
        view.width = 520
        view.height = 700
    }

    function test_screenSourceTitleDisplaysLiterally() {
        const title = "<b>Example window</b>"
        captureDisplay.sources = [{index: 7, name: title}]
        captureDisplay.selectedSource = -1
        view.screen = captureDisplay
        const picker = findChild(view, "screenPicker")
        try {
            picker.open()
            tryCompare(picker, "opened", true)
            tryVerify(function() { return visualChild(picker.contentItem, "screenSource_7") !== null })
            const row = visualChild(picker.contentItem, "screenSource_7")
            const reference = createTemporaryObject(literalName, test, {text: title, font: row.font})
            verify(reference)
            waitForRendering(row)
            fuzzyCompare(row.contentItem.implicitWidth, reference.implicitWidth, 0.1)
            mouseClick(row)
            compare(captureDisplay.selectedSource, 7)
            tryCompare(picker, "visible", false)
        } finally {
            picker.close()
            captureDisplay.sources = []
            captureDisplay.selectedSource = -1
        }
    }

    function test_screenAudioControlsFollowSourceOwnershipAndPlatform() {
        if (!supporterLicense.directDistribution) return
        verify(fixtures.startHost())
        verify(fixtures.setSupporter(true))
        captureDisplay.active = false
        captureDisplay.audioEnabled = false
        captureDisplay.audioAvailable = true
        captureDisplay.hostId = channel.ownId
        view.screen = captureDisplay
        view.selectedHost = channel.ownId
        const share = findChild(view, "shareScreen")
        share.menu.popup()
        tryCompare(share.menu, "opened", true)
        verify(share.visible)
        verify(share.enabled)
        waitForRendering(share)
        mouseClick(share)
        const picker = findChild(view, "screenPicker")
        tryCompare(picker, "opened", true)
        verify(findChild(picker, "screenAudioBeforeStart") === null)
        compare(captureDisplay.audioEnabled, false)
        captureDisplay.audioAvailable = false
        picker.close()
        tryCompare(picker, "visible", false)
        captureDisplay.audioAvailable = true
        captureDisplay.active = true
        captureDisplay.computerAudio = false
        const menuAudio = findChild(view, "screenAudioWhileSharing")
        share.menu.popup()
        tryCompare(share.menu, "opened", true)
        verify(menuAudio.visible)
        compare(menuAudio.text, "App audio")
        captureDisplay.audioAvailable = false
        verify(!menuAudio.visible)
        verify(captureDisplay.active)
        compare(captureDisplay.audioEnabled, false)
        captureDisplay.audioAvailable = true
        verify(menuAudio.visible)
        captureDisplay.computerAudio = true
        compare(menuAudio.text, "Computer audio")
        waitForRendering(menuAudio)
        mouseClick(menuAudio)
        compare(captureDisplay.audioEnabled, true)
        view.selectedHost = remoteChannel.ownId
        captureDisplay.hostId = remoteChannel.ownId
        share.menu.popup()
        tryCompare(share.menu, "opened", true)
        verify(!menuAudio.visible)
        share.menu.close()
    }

    function cleanupTestCase() {
        // Destroy the windows while the event loop can still release canvases.
        scene.active = false
        wait(0)
    }

    function openOwnChannelInfo() {
        view.closeSettings()
        view.selectedHost = channel.ownId
        const info = findChild(view, "channelInfoDialog")
        if (!info.visible) info.open()
        tryCompare(info, "opened", true)
        return info
    }

    function reveal(item) {
        let scrollName = "settingsScroll"
        let ancestor = item
        while (ancestor) {
            if (ancestor.objectName === "audioSettings") { view.openSettings(1); scrollName = "audioSettingsScroll"; break }
            if (ancestor.objectName === "hostPage") { openOwnChannelInfo(); scrollName = "channelInfoScroll"; break }
            if (ancestor.objectName === "generalSettings") { view.openSettings(0); break }
            if (ancestor.objectName === "discoverDialog") { ancestor.open(); break }
            ancestor = ancestor.parent
        }
        waitForRendering(view.contentItem)
        const scroll = findChild(view, scrollName)
        const flick = scroll.contentItem
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, item.mapToItem(flick.contentItem, 0, 0).y - 20))
        waitForRendering(item)
    }

    function visualChild(parent, name) {
        if (parent.objectName === name) return parent
        for (const item of parent.children || []) {
            const match = visualChild(item, name)
            if (match) return match
        }
        return null
    }

    function test_addManageAndConfirmRemovalOfOwnedChannel() {
        verify(fixtures.startHost())
        if (!supporterLicense.directDistribution) {
            verify(!fixtures.setSupporter(true))
            mouseClick(findChild(view, "addChannel"))
            tryCompare(findChild(view, "discoverDialog"), "opened", true)
            verify(!findChild(view, "createOwnChannel").visible)
            compare(channel.ownedChannels.length, 1)
            return
        }
        verify(fixtures.setSupporter(true))
        mouseClick(findChild(view, "addChannel"))
        const discovery = findChild(view, "discoverDialog")
        tryCompare(discovery, "opened", true)
        const createButton = findChild(view, "createOwnChannel")
        waitForRendering(createButton)
        mouseClick(createButton)
        const create = findChild(view, "newOwnChannelDialog")
        tryCompare(create, "opened", true)
        findChild(create, "newOwnChannelName").text = "Quiet room"
        mouseClick(findChild(create, "saveOwnChannel"))
        tryVerify(function() { return channel.ownedChannels.length === 2 })
        const id = channel.ownedChannels.find(function(c) { return c.id !== channel.ownId }).id
        tryCompare(channel, "chatHostId", id)
        tryCompare(channel, "chatReady", true)
        const owner = channel.ownChannel(id)
        view.selectedHost = id
        const info = findChild(view, "channelInfoDialog")
        info.open(); tryCompare(info, "opened", true)
        compare(findChild(info, "channelName").text, "Quiet room")
        compare(findChild(info, "hostPort").value, channel.configuredPort)
        const ttl = findChild(info, "messageLifetime")
        ttl.currentIndex = 2; ttl.activated(2)
        compare(owner.messageLifetimeDays, 30)
        compare(ttl.count, 6)
        for (let i = 3; i < 6; ++i) {
            ttl.currentIndex = i; ttl.activated(i)
            compare(owner.messageLifetimeDays, [90, 180, 360][i - 3])
        }
        compare(channel.messageLifetimeDays, 1)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/owned-channel-info.png"))
        info.close(); tryCompare(info, "visible", false)
        const remove = findChild(view, "removeChannelMenuItem")
        remove.menu.popup(); tryCompare(remove.menu, "opened", true)
        waitForRendering(remove); mouseClick(remove)
        const confirm = findChild(view, "removeOwnChannelDialog")
        tryCompare(confirm, "opened", true)
        compare(channel.ownedChannels.length, 2)
        confirm.reject(); tryCompare(confirm, "visible", false)
        compare(channel.ownedChannels.length, 2)
        remove.menu.popup(); tryCompare(remove.menu, "opened", true)
        mouseClick(remove); tryCompare(confirm, "opened", true)
        mouseClick(findChild(confirm, "confirmRemoveOwnChannel"))
        tryVerify(function() { return channel.ownedChannels.length === 1 })
        verify(channel.ownChannel(id) === null)
        verify(channel.hosting)
    }
    function test_secondaryChannelRequestApprovesOnlyItsOwnChannel() {
        if (!supporterLicense.directDistribution) return
        verify(fixtures.startHost())
        verify(fixtures.setSupporter(true))
        verify(channel.decide(remoteChannel.ownId, false))
        const id = channel.addOwnedChannel("Second room")
        verify(id.length > 0)
        const owner = channel.ownChannel(id)
        verify(remoteChannel.openAddress("localhost:" + channel.servicePort))
        tryCompare(remoteChannel, "directBusy", false)
        verify(remoteChannel.openChat(id, "localhost", channel.servicePort))
        tryVerify(function() { return owner.requests.length === 1 })
        const inspect = findChild(findChild(view, "pendingRequest"), "inspectRequest")
        mouseClick(inspect)
        const dialog = findChild(view, "requestInfoDialog")
        tryCompare(dialog, "opened", true)
        compare(dialog.entry.hostId, id)
        mouseClick(findChild(dialog, "approveInspectedRequest"))
        tryCompare(remoteChannel, "chatReady", true)
        compare(owner.hostClients.length, 1)
        compare(channel.hostClients.length, 0)
        verify(remoteChannel.openChat(channel.ownId, "localhost", channel.servicePort))
        tryVerify(function() { return channel.requests.some(function(r) { return r.hostId === channel.ownId }) })
        verify(!remoteChannel.chatReady)
    }

    function test_pendingRequestCanBeInspectedAndApprovedFromFooter() {
        verify(fixtures.startHost())
        verify(fixtures.startRemoteHost())
        verify(channel.decide(remoteChannel.ownId, false))
        session.setMuted(false)
        verify(remoteChannel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryVerify(function() { return channel.requests.length === 1 })
        const footer = findChild(view, "pendingRequest")
        verify(footer && footer.visible)
        const inspect = findChild(footer, "inspectRequest")
        waitForRendering(inspect); mouseClick(inspect)
        const dialog = findChild(view, "requestInfoDialog")
        tryCompare(dialog, "opened", true)
        const requestCanvas = findChild(findChild(dialog, "requestAvatar"), "avatarCanvas")
        tryVerify(function() { return requestCanvas.isImageLoaded(requestCanvas.atlas) })
        waitForRendering(requestCanvas)
        compare(findChild(dialog, "requestName").text, remoteSession.userName)
        compare(findChild(dialog, "requestIdentity").text, remoteChannel.ownId)
        verify(findChild(dialog, "requestAddress").text.includes("127.0.0.1"))
        compare(findChild(dialog, "requestChannel").text, remoteChannel.channelName)
        verify(!remoteChannel.chatReady)
        for (const size of [[520, 700], [360, 420]]) {
            view.width = size[0]; view.height = size[1]
            waitForRendering(dialog.contentItem)
            verify(dialog.width <= view.width && dialog.height <= view.height)
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/request-info-" + size[0] + ".png"))
        }
        for (const language of ["de", "ar", "en"]) {
            verify(session.setLanguage(language))
            waitForRendering(dialog.contentItem)
            for (const name of ["approveInspectedRequest", "denyInspectedRequest"]) {
                const button = findChild(dialog, name)
                const position = button.mapToItem(view.contentItem, 0, 0)
                verify(position.x >= 0 && position.x + button.width <= view.width)
                verify(position.y >= 0 && position.y + button.height <= view.height)
            }
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/request-info-" + language + ".png"))
        }
        dialog.close(); tryCompare(dialog, "visible", false)
        const approve = findChild(footer, "approveRequest")
        waitForRendering(approve)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/request-footer.png"))
        mouseClick(approve)
        tryCompare(remoteChannel, "chatReady", true)
        tryVerify(function() { return !footer.visible })
        verify(session.available && session.transmissionAllowed)
    }

    function test_systemPortraitIsQuietWhileMusicIsActive() {
        portrait.systemMessage = true; portrait.level = 0.25
        portrait.muted = false; portrait.available = true; portrait.online = true
        portrait.animated = true; portrait.visible = true
        compare(portrait.activeDuration, 600)
        verify(portrait.cycleLength >= 12000 && portrait.cycleLength < 18000)
        let moving = 0, resting = 0, previous = -1
        for (let t = 0; t < 36000; t += 25) {
            portrait.animationTime = t
            compare(portrait.stateRow, 1)
            verify(portrait.activity > 0)
            if (portrait.column === previous) ++resting; else ++moving
            previous = portrait.column
        }
        verify(moving > 1 && resting > moving * 20, "The bot holds its pose for most of the time, including during music")
        portrait.systemMessage = false; portrait.level = 0
    }

    function test_inspectedRequestCanBeDeniedAtMinimumWindowSize() {
        verify(fixtures.startHost())
        verify(fixtures.startRemoteHost())
        verify(channel.decide(remoteChannel.ownId, false))
        verify(remoteChannel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryVerify(function() { return channel.requests.length === 1 })
        view.width = 360; view.height = 420
        const inspect = findChild(view, "inspectRequest")
        waitForRendering(inspect); mouseClick(inspect)
        const dialog = findChild(view, "requestInfoDialog")
        tryCompare(dialog, "opened", true)
        const deny = findChild(dialog, "denyInspectedRequest")
        verify(deny)
        waitForRendering(deny); mouseClick(deny)
        tryVerify(function() { return channel.requests.length === 0 })
        tryCompare(dialog, "visible", false)
        verify(!remoteChannel.chatReady)
    }

    function test_inspectClosesWhenTheRequesterDisconnects() {
        verify(fixtures.startHost())
        verify(fixtures.startRemoteHost())
        verify(channel.decide(remoteChannel.ownId, false))
        verify(remoteChannel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryVerify(function() { return channel.requests.length === 1 })
        const inspect = findChild(view, "inspectRequest")
        waitForRendering(inspect); mouseClick(inspect)
        const dialog = findChild(view, "requestInfoDialog")
        tryCompare(dialog, "opened", true)
        verify(remoteChannel.closeChat(channel.ownId))
        tryVerify(function() { return channel.requests.length === 0 })
        tryCompare(dialog, "visible", false)
        verify(!findChild(view, "pendingRequest").visible)
        verify(!remoteChannel.chatReady)
    }

    function test_avatarShelfHasTwentySelectableAnimatedPortraits() {
        const paid = fixtures.setSupporter(true)
        view.openSettings(0)
        const shelf = findChild(view, "avatarShelf")
        verify(shelf !== null)
        compare(shelf.count, 20)
        const previous = session.avatar
        for (let i = 0; i < shelf.count; ++i) {
            shelf.currentIndex = i
            tryVerify(function() { return shelf.currentItem && shelf.currentItem.index === i
                && Math.abs(shelf.currentItem.x + shelf.currentItem.width / 2 - shelf.width / 2) < 1 })
            tryVerify(function() {
                const current = shelf.currentItem
                const canvas = current && current.index === i ? findChild(current, "avatarCanvas") : null
                const portrait = canvas && canvas.parent
                return portrait && fixtures.portraitHasDetail(portrait, !portrait.online)
            }, 5000, "Visible carousel portrait " + i)
            const canvas = findChild(shelf.currentItem, "avatarCanvas")
            verify(canvas.crop.width >= canvas.width && canvas.crop.height >= canvas.height,
                "Portrait crop contains enough pixels for its displayed size")
            waitForRendering(shelf)
            mouseClick(shelf, shelf.width / 2, 44)
            compare(session.avatar, session.avatars[paid ? i : Math.min(i, 9)])
        }
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/avatar-shelf-end.png"))
        shelf.positionViewAtIndex(0, PathView.Center)
        waitForRendering(shelf)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/avatar-shelf-start.png"))
        const selected = session.avatar
        const offset = shelf.offset
        mouseDrag(shelf, shelf.width - 30, shelf.height / 2, -shelf.width / 2, 0, Qt.LeftButton)
        tryVerify(function() { return shelf.offset !== offset })
        tryCompare(shelf, "moving", false)
        compare(session.avatar, selected, "Swiping scrolls without selecting a portrait")
        verify(session.setAvatar(previous))
        view.closeSettings()
        view.width = 640; view.height = 720
        portraitGallery.visible = true
        tryCompare(galleryPortraits, "count", session.avatars.length)
        for (let i = 0; i < session.avatars.length; ++i) {
            tryVerify(function() { return galleryPortraits.itemAt(i) !== null })
            const canvas = findChild(galleryPortraits.itemAt(i), "avatarCanvas")
            tryVerify(function() { return canvas.isImageLoaded(canvas.atlas) })
        }
        waitForRendering(portraitGallery)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/avatar-catalog.png"))
        portraitGallery.online = false
        for (let i = 0; i < session.avatars.length; ++i) {
            const avatar = findChild(galleryPortraits.itemAt(i), "avatarCanvas").parent
            tryVerify(function() { return fixtures.portraitHasDetail(avatar, true) }, 5000, session.avatars[i] + " remains visible offline")
        }
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/avatar-catalog-offline.png"))
        portraitGallery.visible = false
    }

    function test_freeAvatarChoicesCannotGrantSupporterAndAboutShowsPurchaseAvailability() {
        view.openSettings(0)
        tryCompare(findChild(view, "settingsDialog"), "opened", true)
        const shelf = findChild(view, "avatarShelf")
        const previous = session.avatar
        shelf.currentIndex = 10
        shelf.positionViewAtIndex(10, PathView.Center)
        tryVerify(function() { return shelf.currentItem && shelf.currentItem.index === 10
            && Math.abs(shelf.currentItem.x + shelf.currentItem.width / 2 - shelf.width / 2) < 1 })
        verify(!shelf.currentItem.unlocked)
        compare(shelf.currentItem.Accessible.role, Accessible.Button)
        compare(shelf.currentItem.Accessible.description, "This avatar requires Supporter.")
        try {
            tryVerify(function() {
                const current = shelf.currentItem
                if (!current || current.index !== 10 || Math.abs(current.x + current.width / 2 - shelf.width / 2) >= 1) return false
                const canvas = findChild(current, "avatarCanvas")
                return canvas && fixtures.portraitHasDetail(canvas.parent, true)
            }, 5000, "Locked choices show their portrait in grayscale")
        } finally {
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/avatar-locked.png"))
        }
        waitForRendering(shelf)
        mouseClick(shelf, shelf.width / 2, 44)
        compare(session.avatar, previous)
        compare(session.supporterEnabled, false)
        if (supporterLicense.directDistribution) {
            compare(view.settingsPage, 3)
            verify(findChild(view, "supporterSettings").visible)
            compare(findChild(view, "supporterState").text, "Not available yet")
            verify(!findChild(view, "activateLicense").enabled)
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/supporter-about.png"))
        } else {
            verify(!findChild(view, "supporterSettings").visible)
        }
    }

    function test_configuredPurchaseControlsFitNarrowSettings_data() {
        return [{tag: "English", language: "en"}, {tag: "German", language: "de"}, {tag: "Arabic", language: "ar"}]
    }
    function test_configuredPurchaseControlsFitNarrowSettings(data) {
        verify(session.setLanguage(data.language))
        view.supporter = licenseDisplay
        view.width = 360; view.height = 560
        view.openSettings(3)
        tryCompare(findChild(view, "settingsDialog"), "opened", true)
        if (!supporterLicense.directDistribution) {
            verify(!findChild(view, "supporterSettings").visible)
            return
        }
        waitForRendering(view.contentItem)
        for (const name of ["licenseKey", "activateLicense", "buyLicense", "checkLicense", "deactivateLicense"]) {
            const control = findChild(view, name)
            verify(control.visible)
            const point = control.mapToItem(view.contentItem, 0, 0)
            verify(point.x >= 0 && point.x + control.width <= view.width, name + " fits the window")
        }
        const input = findChild(view, "licenseKey")
        compare(input.echoMode, TextInput.Password)
        verify(!findChild(view, "activateLicense").enabled)
        openedLinks.clear()
        mouseClick(findChild(view, "buyLicense"))
        tryCompare(openedLinks, "count", 1)
        compare(openedLinks.signalArguments[0][0], licenseDisplay.purchaseUrl.toString())
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/supporter-" + data.language + ".png"))
    }

    function test_avatarShelfWrapsWithoutGrowingOrChangingSelection() {
        view.openSettings(0)
        const shelf = findChild(view, "avatarShelf")
        const selected = session.avatar
        shelf.currentIndex = 0
        view.requestActivate()
        tryCompare(view, "active", true)
        shelf.forceActiveFocus()
        tryCompare(shelf, "activeFocus", true)
        for (let cycle = 0; cycle < 3; ++cycle) {
            for (let i = 0; i < 20; ++i) keyClick(Qt.Key_Left)
            compare(shelf.currentIndex, 0)
            compare(shelf.count, 20)
            verify(shelf.pathItemCount + shelf.cacheItemCount < 20)
            waitForRendering(shelf)
            tryVerify(function() {
                const allocated = shelf.children.filter(function(child) { return child.objectName.startsWith("avatarChoice_") }).length
                return allocated > 0 && allocated <= shelf.pathItemCount + shelf.cacheItemCount
            }, 5000, "Completed scrolling releases portraits outside the visible range and cache")
        }
        keyClick(Qt.Key_Left)
        compare(shelf.currentIndex, 19)
        keyClick(Qt.Key_Right)
        compare(shelf.currentIndex, 0)
        mouseWheel(shelf, shelf.width / 2, shelf.height / 2, 120, 0)
        compare(shelf.currentIndex, 19)
        mouseWheel(shelf, shelf.width / 2, shelf.height / 2, 0, -120)
        compare(shelf.currentIndex, 0)
        compare(session.avatar, selected)
        keyClick(Qt.Key_Right)
        keyClick(Qt.Key_Return)
        compare(session.avatar, session.avatars[1])
        session.setAvatar(selected)
    }

    function test_memberInspectionReusesCopyableDetailsWithoutAdmissionActions() {
        verify(fixtures.startHost())
        verify(channel.joinAddress("127.0.0.1:" + channel.servicePort))
        tryCompare(channel, "joinedHostId", channel.ownId)
        tryVerify(function() { return channel.participants.some(function(item) { return item.id === channel.ownId }) })
        const member = channel.participants.find(function(item) { return item.id === channel.ownId })
        verify(member)
        view.memberOptions(member, channel.ownId)
        const inspect = findChild(view, "inspectMember")
        tryCompare(inspect.menu, "opened", true)
        mouseClick(inspect)
        const dialog = findChild(view, "requestInfoDialog")
        tryCompare(dialog, "opened", true)
        compare(findChild(dialog, "requestName").text, session.userName)
        compare(findChild(dialog, "requestIdentity").text, channel.ownId)
        verify(!findChild(dialog, "approveInspectedRequest").visible)
        verify(!findChild(dialog, "denyInspectedRequest").visible)
    }

    function test_channelAndRequestDetailsCanBeCopied() {
        verify(fixtures.startHost())
        const info = openOwnChannelInfo()
        function copy(item) {
            verify(item !== null)
            view.requestActivate()
            tryCompare(view, "active", true)
            item.forceActiveFocus()
            tryCompare(item, "activeFocus", true)
            keySequence(StandardKey.SelectAll)
            keySequence(StandardKey.Copy)
            compare(fixtures.clipboardText(), item.text)
            verify(item.readOnly)
        }
        copy(findChild(info, "channelAddress"))
        copy(findChild(info, "channelIdentity"))
        info.close()
        verify(fixtures.startRemoteHost())
        verify(channel.decide(remoteChannel.ownId, false))
        verify(remoteChannel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryVerify(function() { return channel.requests.length === 1 })
        mouseClick(findChild(view, "inspectRequest"))
        const dialog = findChild(view, "requestInfoDialog")
        tryCompare(dialog, "opened", true)
        for (const name of ["requestName", "requestChannel", "requestAddress", "requestIdentity"])
            copy(findChild(dialog, name))
    }

    function test_internetHelpIsOptionalAndLinksRequireClick() {
        verify(fixtures.startHost())
        const info = openOwnChannelInfo()
        const help = findChild(info, "internetHelp")
        verify(help !== null)
        verify(!help.visible)
        openedLinks.clear()
        mouseClick(findChild(info, "internetHelpToggle"))
        verify(help.visible)
        compare(openedLinks.count, 0)
        verify(findChild(help, "internetPortHelp").text.includes(channel.servicePort.toString()))
        verify(findChild(help, "internetPortHelp").text.includes("TCP/UDP"))
        for (const link of [["ddnsHelp", "https://www.duckdns.org/why.jsp"],
                            ["vpnHelp", "https://tailscale.com/kb/1017/install"]]) {
            const button = findChild(help, link[0])
            const scroll = findChild(info, "channelInfoScroll")
            scroll.contentItem.contentY = button.mapToItem(scroll.contentItem.contentItem, 0, 0).y - 30
            waitForRendering(button); openedLinks.clear(); mouseClick(button)
            compare(openedLinks.count, 1)
            compare(openedLinks.signalArguments[0][0], link[1])
        }
        info.close(); info.open(); tryCompare(info, "opened", true)
        verify(!help.visible)
    }

    function test_arabicMirrorsSettingsAndKeepsChannelInputUsable() {
        verify(session.setLanguage("ar"))
        view.width = 360; view.height = 520
        view.openSettings(0)
        waitForRendering(view.contentItem)
        const tabs = findChild(view, "settingsSection")
        const first = tabs.itemAt(0), last = tabs.itemAt(3)
        verify(first.x > last.x, "Right-to-left settings start on the right")
        for (const page of [0, 1, 2, 3]) {
            view.settingsPage = page
            waitForRendering(view.contentItem)
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/arabic-360-" + page + ".png"))
        }
        verify(session.setLanguage("en"))
        waitForRendering(view.contentItem)
        tryVerify(function() { return tabs.itemAt(0).x < tabs.itemAt(3).x },
                  2000, "English restores left-to-right order")
    }

    function test_audioValidationUsesTheSelectedLanguageAndKeepsTheProfile() {
        verify(session.setLanguage("de"))
        const gain = audio.gainDb
        verify(!audio.setGainDb(100))
        compare(audio.gainDb, gain)
        if (!audio.inputAvailable) return
        compare(audio.status, "Ungültige Verstärkung oder ungültiger Frequenzbereich")
        verify(session.setLanguage("en"))
        verify(!audio.setGainDb(100))
        compare(audio.status, "Invalid gain or frequency range")
        compare(audio.gainDb, gain)
    }

    function test_languageChangeKeepsSettingsKeyboardFocus() {
        view.openSettings(0)
        tryCompare(findChild(view, "settingsDialog"), "opened", true)
        const tabs = findChild(view, "settingsSection")
        const tab = tabs.itemAt(2)
        view.requestActivate()
        tryCompare(view, "active", true)
        tab.forceActiveFocus()
        tryCompare(tab, "activeFocus", true)
        verify(session.setLanguage("de"))
        verify(tabs.itemAt(2).activeFocus)
        compare(view.settingsPage, 0)
    }

    function test_dialogButtonsFollowTheSelectedLanguage_data() {
        return session.languages.map(function(entry) { return {tag: entry.code, language: entry.code} })
    }
    function test_dialogButtonsFollowTheSelectedLanguage(data) {
        verify(session.setLanguage(data.language))
        view.selectedHost = channel.ownId
        for (const scenario of [["channelInfoDialog", Dialog.Close, "Close"],
                                ["newOwnChannelDialog", Dialog.Cancel, "Cancel"],
                                ["recordingDialog", Dialog.Close, "Close"],
                                ["microphonePermissionDialog", Dialog.Cancel, "Cancel"]]) {
            if (scenario[0] === "recordingDialog") {
                view.openSettings(1)
                tryCompare(findChild(view, "settingsDialog"), "opened", true)
            }
            const dialog = findChild(view, scenario[0])
            verify(dialog)
            dialog.open(); tryCompare(dialog, "opened", true, 5000, scenario[0])
            const button = findChild(dialog, scenario[1] === Dialog.Close ? "dialogCloseButton" : "dialogCancelButton")
            verify(button)
            wait(50)
            compare(button.text, qsTranslate("Channels", scenario[2]))
            verify(session.setLanguage("en"))
            wait(50)
            compare(button.text, scenario[2])
            verify(session.setLanguage(data.language))
            wait(50)
            compare(button.text, qsTranslate("Channels", scenario[2]))
            if (imageDirectory.length > 0 && data.language === "ar" && scenario[0] === "channelInfoDialog") {
                verify(fixtures.saveWindow(view, imageDirectory + "/channel-info-ar-buttons.png"))
            }
            dialogRejections.target = dialog
            dialogRejections.clear()
            mouseClick(button)
            tryCompare(dialog, "visible", false)
            compare(dialogRejections.count, 1)
        }
    }

    function test_languageChangesInPlaceAndKeepsMicrophoneState() {
        verify(fixtures.startHost())
        view.openSettings(0)
        const language = findChild(view, "languageChoice")
        compare(language.count, session.languages.length)
        verify(session.setMuted(false))
        const germanIndex = session.languages.findIndex(function(entry) { return entry.code === "de" })
        language.currentIndex = germanIndex
        language.activated(germanIndex)
        compare(session.language, "de")
        compare(findChild(view, "themeChoice").textAt(0), "System (automatisch)")
        verify(!session.muted)
        for (const entry of session.languages) {
            verify(session.setLanguage(entry.code))
            waitForRendering(view.contentItem)
            verify(fixtures.textHasGlyphs(entry.label, language.font), entry.code + " language name has missing glyphs")
            const tabs = findChild(view, "settingsSection")
            for (let tab = 0; tab < 4; ++tab) {
                const item = tabs.itemAt(tab)
                verify(fixtures.textHasGlyphs(item.text, item.font), entry.code + " settings tab has missing glyphs: " + item.text)
            }
            for (const size of [[360, 420], [520, 700]]) {
                view.width = size[0]; view.height = size[1]
                for (const page of [0, 1, 2, 3]) {
                    view.settingsPage = page
                    waitForRendering(view.contentItem)
                    if (imageDirectory.length > 0 && page < 2) verify(fixtures.saveWindow(view,
                        imageDirectory + "/language-" + entry.code + "-" + size[0] + "-" + page + ".png"))
                }
                const info = openOwnChannelInfo()
                const lifetime = findChild(info, "messageLifetimeLabel")
                waitForRendering(info.contentItem)
                verify(lifetime.contentWidth <= lifetime.width + 1, entry.code + " lifetime width")
                verify(lifetime.contentHeight <= lifetime.height + 1, entry.code + " lifetime height")
                if (imageDirectory.length > 0 && ["de", "ar"].includes(entry.code)) verify(fixtures.saveWindow(view,
                    imageDirectory + "/channel-info-" + entry.code + "-" + size[0] + ".png"))
                info.close(); view.openSettings(0)
            }
            verify(!session.muted)
        }
        verify(session.setLanguage("de"))
        for (const size of [[360, 420], [520, 700]]) {
            view.width = size[0]; view.height = size[1]
            for (const page of [0, 1, 2, 3]) {
                view.settingsPage = page
                waitForRendering(view.contentItem)
                if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/german-" + size[0] + "-" + page + ".png"))
            }
        }
        view.settingsPage = 0
        language.currentIndex = 0
        language.activated(0)
        compare(session.language, "en")
        compare(findChild(view, "themeChoice").textAt(0), "System (automatic)")
        verify(!session.muted)
    }

    function test_addChannelListScrollsAndOnlyOffersNewEndpoints() {
        verify(fixtures.startHost())
        verify(fixtures.advertiseNearbyChannels(48))
        const dialog = findChild(view, "discoverDialog")
        const list = findChild(view, "discoveryList")
        dialog.open()
        tryVerify(function() { return list.count >= 48 })
        tryVerify(function() { return list.count === channel.availableHosts.length }, 2000,
            "Wait for the coalesced discovery list to include the latest announcement")
        let endpoints = {}, ids = {}
        for (const host of channel.savedChannels) ids[host.id] = true
        for (const host of list.model) {
            verify(!ids[host.id], "Saved host must not be offered again")
            const endpoint = host.address + ":" + host.port
            verify(!endpoints[endpoint], "An endpoint may appear only once")
            endpoints[endpoint] = true
        }
        for (const size of [[360, 420], [520, 700]]) {
            view.width = size[0]; view.height = size[1]
            waitForRendering(dialog.contentItem)
            verify(list.height > 0 && list.contentHeight > list.height)
            list.positionViewAtEnd()
            tryVerify(function() { return list.contentY > 0 && list.atYEnd })
            wait(1700)
            verify(list.atYEnd, "Periodic discovery must not reset the scroll position")
            if (size[0] === 360) {
                const anchorIndex = list.indexAt(1, list.contentY + list.spacing + 1)
                verify(anchorIndex >= 0)
                const anchorId = list.model[anchorIndex].id
                const positionBefore = list.itemAtIndex(anchorIndex).y - list.contentY
                verify(fixtures.advertiseNearbyChannels(49))
                tryVerify(function() { return list.count > 48 })
                waitForRendering(list)
                const nextIndex = list.model.findIndex(function(host) { return host.id === anchorId })
                verify(nextIndex >= 0)
                const anchor = list.itemAtIndex(nextIndex)
                verify(anchor !== null)
                compare(anchor.y - list.contentY, positionBefore, "A new channel must not move the currently visible entry")
            }
            const last = findChild(list, "discovered_" + "702f".padStart(64, "0"))
            verify(last !== null)
            compare(findChild(last, "addDiscovered_" + "702f".padStart(64, "0")).text, "Add")
            const position = last.mapToItem(list, 0, 0)
            verify(position.y >= -1 && position.y + last.height <= list.height + 1)
            const address = findChild(view, "directAddress")
            const point = address.mapToItem(dialog.contentItem, 0, 0)
            verify(point.y >= 0 && point.y + address.height <= dialog.contentItem.height)
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/add-channel-scroll-" + size[0] + ".png"))
        }
    }

    function test_settingsContainProfileAndAudioInTheChannelWindow() {
        verify(findChild(view, "editDisplayName") === null, "Profile belongs in settings")
        verify(findChild(view, "hostSettings") === null, "One settings entry is sufficient")
        view.openSettings(0)
        const settings = findChild(view, "settingsDialog")
        tryCompare(settings, "opened", true)
        verify(visualChild(settings.contentItem, "avatarChoice_mechanic") !== null)
        verify(session.available)
        view.settingsPage = 1
        tryCompare(session, "available", false)
        verify(findChild(settings, "previewMicrophone") === null, "Input tab owns preview")
        const guided = findChild(settings, "guidedTest")
        reveal(guided); mouseClick(guided)
        tryCompare(view, "recordingMode", true)
        view.settingsPage = 2
        compare(view.recordingMode, false)
        verify(!session.available)
        view.settingsPage = 0
        tryCompare(session, "available", true)
        verify(!audio.running)
        for (const size of [[360, 360], [900, 760]]) {
            view.width = size[0]; view.height = size[1]
            for (const page of [0, 1, 2, 3]) {
                view.settingsPage = page
                waitForRendering(settings.contentItem)
                verify(settings.width <= view.width && settings.height <= view.height)
                const choice = findChild(settings, "settingsSection")
                verify(choice.width > 0 && choice.width <= view.width)
                if (imageDirectory.length > 0 && page === 1) verify(fixtures.saveWindow(view, imageDirectory + "/input-settings-" + size[0] + ".png"))
            }
        }
        settings.close()
        tryCompare(session, "available", true)
    }

    function test_channelInfoOwnsConfigurationWithoutChangingVoiceOrForeignHosts() {
        verify(fixtures.startHost())
        verify(channel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(channel, "joined", true)
        session.setMuted(false)
        const settings = findChild(view, "settingsDialog")
        view.openSettings(0)
        compare(findChild(settings, "settingsSection").count, 4)
        verify(findChild(settings, "hostPage") === null)
        view.closeSettings()
        tryCompare(settings, "visible", false)
        view.selectedHost = channel.ownId
        const menu = findChild(view, "autoJoinMenuItem").menu
        menu.popup(); tryCompare(menu, "opened", true)
        waitForRendering(menu.itemAt(0))
        mouseClick(menu.itemAt(0))
        const info = findChild(view, "channelInfoDialog")
        tryCompare(info, "opened", true)
        tryCompare(menu, "visible", false)
        verify(session.transmissionAllowed && session.available)
        const name = findChild(info, "channelName")
        verify(name && name.visible && name.enabled)
        const previousName = channel.channelName
        waitForRendering(name)
        view.requestActivate()
        tryCompare(view, "active", true)
        mouseClick(name)
        tryCompare(name, "activeFocus", true)
        name.text = "Shared room"
        const ttl = findChild(info, "messageLifetime")
        mouseClick(ttl)
        ttl.popup.close()
        compare(channel.channelName, "Shared room")
        ttl.currentIndex = 1; ttl.activated(1)
        compare(channel.messageLifetimeDays, 7)
        const autoJoin = findChild(info, "channelInfoAutoJoin")
        const previousAutoJoin = autoJoin.checked
        // Exercise the checkbox through its visual control, including its binding.
        mouseClick(autoJoin)
        compare(autoJoin.checked, !previousAutoJoin)
        for (const size of [[520, 700], [360, 360]]) {
            view.width = size[0]; view.height = size[1]
            waitForRendering(info.contentItem)
            const scroll = findChild(info, "channelInfoScroll")
            if (size[0] === 360) verify(scroll.contentItem.contentHeight > scroll.contentItem.height)
            scroll.contentItem.contentY = scroll.contentItem.contentHeight - scroll.contentItem.height
            verify(info.width <= view.width && info.height <= view.height)
            scroll.contentItem.contentY = 0
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/channel-info-own-" + size[0] + ".png"))
        }
        info.close()
        verify(fixtures.startRemoteHost())
        verify(remoteChannel.decide(channel.ownId, true))
        verify(channel.openChat(remoteChannel.ownId, "127.0.0.1", remoteChannel.servicePort))
        tryCompare(channel, "chatHostId", remoteChannel.ownId)
        tryCompare(channel, "chatReady", true)
        view.selectedHost = remoteChannel.ownId
        info.open(); tryCompare(info, "opened", true)
        verify(!findChild(info, "hostPage").visible)
        verify(!name.enabled)
        view.width = 520; view.height = 700
        waitForRendering(info.contentItem)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/channel-info-foreign.png"))
        verify(session.transmissionAllowed && session.available)
        compare(channel.joinedHostId, channel.ownId)
        verify(channel.setChannelName(previousName))
        verify(channel.setMessageLifetimeDays(1))
        verify(channel.setAutoJoin(channel.ownId, previousAutoJoin))
    }

    function test_inputTabOwnsPreviewAndMutesBeforeStartingIt() {
        verify(previewRequests.valid)
        session.setMuted(false)
        view.openSettings(0)
        previewRequests.clear()
        view.settingsPage = 1
        tryVerify(function() { return previewRequests.count > 0 })
        compare(previewRequests.signalArguments[previewRequests.count - 1][0], true)
        verify(!session.available)
        view.settingsPage = 2
        compare(previewRequests.signalArguments[previewRequests.count - 1][0], false)
        verify(!session.available)
        view.settingsPage = 1
        compare(previewRequests.signalArguments[previewRequests.count - 1][0], true)
        view.closeSettings()
        tryCompare(session, "available", true)
        compare(previewRequests.signalArguments[previewRequests.count - 1][0], false)
        verify(!session.muted)
    }

    function test_voiceActivationCanBeToggledAndAdjustedInInput() {
        view.openSettings(1)
        const activation = findChild(view, "voiceActivation")
        verify(activation !== null)
        const original = audio.activationEnabled
        if (!audio.inputAvailable) {
            verify(!activation.enabled)
            verify(!audio.setActivationEnabled(!original))
            compare(audio.activationEnabled, original)
            verify(!audio.running)
            verify(!session.available)
            return
        }
        for (let i = 0; i < 4; ++i) {
            reveal(activation)
            mouseClick(activation)
            compare(audio.activationEnabled, i % 2 === 0 ? !original : original)
            compare(activation.checked, audio.activationEnabled)
            verify(!audio.running)
            verify(!session.available)
        }
        // Native accessibility changes the checked property without a click.
        activation.checked = !original
        compare(audio.activationEnabled, !original)
        activation.checked = original
        compare(audio.activationEnabled, original)
        audio.setActivationEnabled(true)
        const threshold = findChild(view, "voiceThreshold")
        const automatic = findChild(threshold, "automaticControl")
        audio.setActivationAutomatic(true)
        compare(automatic.checked, true)
        const value = findChild(threshold, "valueControl")
        value.value = -32
        compare(audio.activationAutomatic, false)
        compare(automatic.checked, false)
        compare(audio.activationThresholdDb, -32)
        automatic.checked = true
        compare(audio.activationAutomatic, true)
        verify(!audio.running)
        audio.setActivationEnabled(original)
    }

    function test_inputAutomationCanBeOverriddenIndependently() {
        view.openSettings(1)
        const gain = findChild(view, "microphoneGain")
        const cut = findChild(view, "microphoneLowCut")
        verify(gain && cut)
        verify(findChild(gain, "automaticControl").visible)
        verify(findChild(cut, "automaticControl").visible)
        if (!audio.inputAvailable) {
            verify(!audio.setGainAutomatic(true)); verify(!audio.setHighPassAutomatic(true))
            return
        }
        const gainValue = audio.gainDb, cutValue = audio.highPassHz
        const gainAuto = audio.gainAutomatic, cutAuto = audio.highPassAutomatic
        verify(audio.setGainAutomatic(true)); verify(audio.setHighPassAutomatic(true))
        compare(findChild(gain, "automaticControl").checked, true)
        compare(findChild(cut, "automaticControl").checked, true)
        findChild(gain, "valueControl").value = -6
        compare(audio.gainAutomatic, false); compare(audio.gainDb, -6)
        compare(audio.highPassAutomatic, true)
        findChild(cut, "valueControl").value = 80
        compare(audio.highPassAutomatic, false); compare(audio.highPassHz, 80)
        findChild(gain, "automaticControl").checked = true
        compare(audio.gainAutomatic, true)
        if (imageDirectory.length > 0) {
            waitForRendering(gain)
            verify(fixtures.saveWindow(view, imageDirectory + "/input-automation.png"))
        }
        audio.setGainDb(gainValue); audio.setHighPassHz(cutValue)
        audio.setGainAutomatic(gainAuto); audio.setHighPassAutomatic(cutAuto)
    }

    function test_inputCurveTracksGainWhilePreviewIsStopped() {
        view.openSettings(1)
        const chart = findChild(view, "frequencyChart")
        const gain = findChild(view, "microphoneGain")
        const original = audio.gainDb
        const wasAutomatic = audio.gainAutomatic
        if (!audio.inputAvailable) {
            verify(!findChild(gain, "valueControl").enabled)
            verify(!audio.setGainDb(original + 3))
            compare(audio.gainDb, original)
            compare(chart.active, false)
            return
        }
        audio.setGainDb(original)
        const response = chart.response.slice()
        const next = original > 20 ? original - 3 : original + 3
        findChild(gain, "valueControl").value = next
        compare(audio.gainDb, next)
        verify(Math.abs(chart.response[128] - response[128] - (next - original)) < 0.01)
        compare(chart.active, false)
        compare(findChild(gain, "automaticControl").visible, true)
        compare(audio.gainAutomatic, false)
        audio.setGainDb(original)
        audio.setGainAutomatic(wasAutomatic)
        waitForRendering(chart)
        const chartY = chart.mapToItem(view.contentItem, 0, 0).y
        reveal(findChild(view, "chooseRemoteKey"))
        compare(chart.mapToItem(view.contentItem, 0, 0).y, chartY, "Spectrum stays visible while controls scroll")
        for (const size of [[520, 740], [360, 560]]) {
            view.width = size[0]; view.height = size[1]
            findChild(view, "audioSettingsScroll").contentItem.contentY = 0
            waitForRendering(chart)
            verify(chart.width > 200 && chart.width < view.width)
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/compact-input-" + size[0] + ".png"))
        }
    }

    function test_echoCancellationFollowsSelectedMicrophoneProfile() {
        view.openSettings(1)
        const control = findChild(view, "echoCancellation")
        verify(control !== null)
        const original = audio.echoCancellation
        compare(control.checked, original)
        const reference = findChild(view, "echoReference")
        verify(reference !== null)
        compare(reference.text, audio.echoReference)
        compare(reference.visible, audio.echoReference.length > 0)
        if (!audio.inputAvailable) {
            verify(!control.enabled)
            verify(!audio.setEchoCancellation(!original))
            compare(audio.echoCancellation, original)
            return
        }
        reveal(control)
        mouseClick(control)
        compare(audio.echoCancellation, !original)
        compare(control.checked, !original)
        if (!audio.echoCancellation) compare(audio.echoReference, "")
        // Accessibility and keyboard changes use the same persisted setter.
        control.checked = original
        compare(audio.echoCancellation, original)
        verify(!session.available)
        verify(!audio.running)
    }

    function test_remoteKeyIsAlwaysInInputWithoutARemoteSettingsTab() {
        view.openSettings(1)
        const settings = findChild(view, "settingsDialog")
        tryCompare(settings, "opened", true)
        compare(findChild(settings, "settingsSection").count, 4)
        verify(findChild(view, "remotePage") === null)
        const remoteKey = findChild(view, "chooseRemoteKey")
        verify(remoteKey !== null)
        reveal(remoteKey)
        verify(remoteKey.visible && remoteKey.enabled)
        mouseClick(remoteKey)
        tryCompare(pttKey, "capturingRemote", true)
        keyClick(Qt.Key_Escape)
        tryCompare(pttKey, "capturing", false)
        verify(findChild(view, "enableGlobalKeys") === null)
        const ptt = findChild(view, "pushToTalkMode")
        ptt.checked = true
        compare(session.pushToTalk, true)
        ptt.checked = false
        compare(session.pushToTalk, false)
    }

    function test_settingsTabsHostOptionsAndRecordingPopup() {
        verify(fixtures.startHost())
        const info = openOwnChannelInfo()
        const ttl = findChild(view, "messageLifetime")
        ttl.currentIndex = 1; ttl.activated(1)
        compare(channel.messageLifetimeDays, 7)
        const port = findChild(view, "hostPort")
        compare(port.textFromValue(48763), "48763")
        compare(port.valueFromText("49999"), 49999)
        port.value = 49999
        const save = findChild(view, "saveHostPort")
        reveal(save); mouseClick(save)
        compare(channel.configuredPort, 49999)
        verify(channel.setConfiguredPort(48763)); verify(channel.setMessageLifetimeDays(1))
        info.close()
        view.openSettings(0)
        const settings = findChild(view, "settingsDialog")
        tryCompare(settings, "opened", true)
        mouseClick(visualChild(settings.contentItem, "settingsTab_1"))
        tryCompare(view, "settingsPage", 1)
        const guided = findChild(settings, "guidedTest")
        reveal(guided); mouseClick(guided)
        const wizard = findChild(view, "recordingDialog")
        tryCompare(wizard, "opened", true)
        compare(audio.recording.step, 0)
        verify(!audio.recording.active)
        verify(!audio.recording.reviewing)
        verify(!session.available)
        verify(findChild(wizard, "startRecordingStep").visible)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/guided-recording-step.png"))
        wizard.close()
        tryCompare(view, "recordingMode", false)
        settings.close()
        tryCompare(session, "available", true)
    }

    function test_channelMenuKeepsCheckAtTheRightAndDangerActionsRed() {
        verify(fixtures.startHost())
        view.selectedHost = channel.ownId
        const item = findChild(view, "autoJoinMenuItem")
        const menu = item.menu
        menu.popup()
        tryCompare(menu, "opened", true)
        verify(item.indicator.x > item.width / 2)
        compare(item.contentItem.x, item.leftPadding)
        const leave = findChild(view, "leaveMenuItem")
        compare(leave.contentItem.color, Theme.danger)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/channel-menu-actions.png"))
        menu.close()
    }

    function test_systemBotUsesNamedChatIdentityAndStaysOutOfHumanRows() {
        verify(fixtures.startHost())
        verify(channel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(channel, "joined", true)
        tryCompare(channel, "chatReady", true)
        verify(channel.setBotName("Clockwork"))
        verify(fixtures.setBotMusicState("Test station", "playing", true))
        tryVerify(function() { return channel.chatBot.name === "Clockwork" && channel.chatBot.id === fixtures.botId() })
        verify(fixtures.publishSystem("Bot announcement"))
        tryVerify(function() { return channel.messages.length > 0 && channel.messages[channel.messages.length - 1].name === "Clockwork" })
        verify(channel.chatMembers.every(function(member) { return !member.music }))
        const own = channel.savedChannels.find(function(entry) { return entry.id === channel.ownId })
        verify(own)
        verify((own.members || []).every(function(member) { return !member.music }))
        view.selectedHost = channel.ownId
        const menu = findChild(view, "autoJoinMenuItem").menu
        menu.popup(); tryCompare(menu, "opened", true)
        const slider = findChild(view, "channelMusicVolume")
        verify(slider.visible)
        if (audio.outputAvailable) {
            verify(slider.enabled)
            verify(audio.setMusicVolume(0.4))
            tryCompare(slider, "value", 0.4)
        } else {
            verify(!slider.enabled)
            verify(!audio.setMusicVolume(0.4))
        }
        menu.close(); if (audio.outputAvailable) verify(audio.setMusicVolume(1.0))
        for (let i = 0; i < 7; ++i) verify(fixtures.advanceTime())
        tryVerify(function() { return channel.chatBot.sleeping === true }, 3000)
        verify(fixtures.publishSystem("Bot woke up"))
        tryVerify(function() { return channel.chatBot.sleeping === false }, 3000)
        view.chatExpanded = true
        const panel = findChild(view, "chatPanel")
        const history = findChild(panel, "chatHistory")
        tryCompare(history, "count", channel.messages.length)
        history.positionViewAtEnd()
        waitForRendering(panel)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/system-bot-chat.png"))
    }

    function test_channelAutoJoinCanBeToggledWithMouseAndKeyboard() {
        verify(fixtures.startHost())
        verify(channel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(channel, "chatReady", true)
        verify(channel.setAutoJoin(channel.ownId, false))
        view.selectedHost = channel.ownId
        const item = findChild(view, "autoJoinMenuItem")
        const menu = item.menu
        menu.popup(); tryCompare(menu, "opened", true)
        mouseClick(item)
        tryVerify(function() { return channel.savedChannels.some(function(h) { return h.id === channel.ownId && h.autoJoin }) })
        menu.popup(); tryCompare(menu, "opened", true)
        view.requestActivate()
        tryCompare(view, "active", true)
        item.forceActiveFocus()
        tryCompare(item, "activeFocus", true)
        keyClick(Qt.Key_Space)
        tryVerify(function() { return channel.savedChannels.some(function(h) { return h.id === channel.ownId && !h.autoJoin }) })
        menu.popup(); tryCompare(menu, "opened", true)
        verify(fixtures.pressAccessible(item))
        tryVerify(function() { return channel.savedChannels.some(function(h) { return h.id === channel.ownId && h.autoJoin }) })
        verify(channel.setAutoJoin(channel.ownId, false))
        menu.close()
    }

    function test_offlineAvatarIsStaticAndUnknownChoiceHasStableFallback() {
        portrait.visible = true; portrait.online = true; portrait.circular = true
        portrait.avatar = "future-dragon-v2"; portrait.animated = true
        const fallback = portrait.displayAvatar
        verify(["mossling", "courier", "mechanic"].includes(fallback))
        portrait.avatar = "courier"; portrait.avatar = "future-dragon-v2"
        compare(portrait.displayAvatar, fallback)
        portrait.online = false
        compare(portrait.stateRow, 4); compare(portrait.activity, 0); compare(portrait.frame, 0)
        wait(1200); compare(portrait.frame, 0)
        tryVerify(function() { return fixtures.portraitHasDetail(portrait, true) }, 5000, "Offline portrait stays visible in grayscale")
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/avatar-offline.png"))
        portrait.circular = false; portrait.online = true
    }

    function test_grayscalePreservesPortraitGeometry_data() {
        const cases = []
        for (const avatar of session.avatars.concat(["system"]))
            for (const size of (["mossling", "courier", "star-moth", "system"].includes(avatar)
                ? [[32, 32], [48, 48], [88, 88], [96, 112], [120, 48]] : [[48, 48], [88, 88]]))
                for (const circular of [false, true])
                    cases.push({tag: avatar + "-" + size.join("x") + "-" + circular, avatar: avatar, size: size, circular: circular})
        return cases
    }
    function test_grayscalePreservesPortraitGeometry(data) {
        const options = {x: 10, y: 80, width: data.size[0], height: data.size[1], avatar: data.avatar,
            animated: false, sleeping: true, circular: data.circular, systemMessage: data.avatar === "system", z: 100}
        const colored = createTemporaryObject(variablePortrait, view.contentItem, options)
        const gray = createTemporaryObject(variablePortrait, view.contentItem, Object.assign({}, options, {x: 160, online: false}))
        verify(colored && gray)
        const canvas = findChild(gray, "avatarCanvas")
        tryVerify(function() { return canvas.isImageLoaded(canvas.atlas) && fixtures.portraitHasDetail(colored) })
        try {
            tryVerify(function() { return fixtures.portraitGrayscaleMismatch(colored, gray) < 0.05 }, 2000,
                "Grayscale must retain the same crop, aspect and scale")
        } finally {
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/geometry-" + data.tag + ".png"))
        }
    }

    function test_avatarStatesHaveThreeAlternatingClipsAndActualSpeechHighlight() {
        portrait.available = true; portrait.muted = false; portrait.deafened = false; portrait.sleeping = false
        portrait.visible = true; portrait.animated = true; portrait.level = 0
        const canvas = findChild(portrait, "avatarCanvas")
        tryVerify(function() { return canvas.isImageLoaded(canvas.atlas) })
        compare(portrait.stateRow, 0)
        portrait.level = 0.1
        compare(portrait.activity, 1); compare(portrait.stateRow, 1)
        const variants = ({})
        const frames = ({})
        for (let t = 0; t < 7000; t += 35) {
            portrait.animationTime = t
            variants[portrait.variant] = true
            frames[portrait.frame] = true
        }
        compare(Object.keys(variants).length, 3)
        compare(Object.keys(frames).length, Math.max.apply(null, portrait.clipRow.frames))
        portrait.muted = true; compare(portrait.activity, 0); compare(portrait.stateRow, 2)
        portrait.deafened = true; compare(portrait.stateRow, 3)
        portrait.sleeping = true; compare(portrait.stateRow, 4)
        portrait.animated = false; compare(portrait.frame, 0)
        portrait.sleeping = false; portrait.deafened = false; portrait.muted = false
        verify(portrait.activity > 0)
        portrait.available = false; compare(portrait.activity, 0)
        portrait.available = true; portrait.level = 0; portrait.animated = true
        let resting = 0, moving = 0, previous = -1
        for (let t = 0; t < 20000; t += 100) {
            portrait.animationTime = t
            if (portrait.column === previous) ++resting; else ++moving
            previous = portrait.column
        }
        verify(moving > 0 && resting > moving * 4, "Idle motion has long pauses, but never freezes")
    }

    function test_avatarShowsBothAudioRestrictionsAtCompactSizes() {
        for (const size of [32, 48, 64]) {
            const item = createTemporaryObject(variablePortrait, view.contentItem, { width: size, height: size, muted: true, deafened: true, circular: true })
            verify(item)
            const mic = findChild(item, "microphoneBadge")
            const output = findChild(item, "speakerBadge")
            verify(mic, "Microphone restriction is visible on the portrait")
            verify(output, "Speaker restriction is visible on the portrait")
            verify(mic.visible && output.visible)
            verify(mic.x >= 0 && output.x + output.width <= size)
            verify(mic.x + mic.width <= output.x)
            item.muted = false
            verify(!mic.visible && output.visible)
            item.deafened = false
            verify(!output.visible)
        }
    }

    function test_idlePortraitVariesPausesAndHeldPosesWithoutLosingSynchronization() {
        const member = createTemporaryObject(variablePortrait, view.contentItem, {identity: "member-a", avatar: "courier"})
        const chat = createTemporaryObject(variablePortrait, view.contentItem, {identity: "member-a", avatar: "courier", circular: true})
        const other = createTemporaryObject(variablePortrait, view.contentItem, {identity: "member-b", avatar: "courier"})
        for (const state of [0, 2, 3, 4]) {
            for (const item of [member, chat, other]) {
                item.muted = state === 2; item.deafened = state === 3; item.sleeping = state === 4
            }
            const holds = new Set(), poses = new Set(), bursts = new Set()
            let previous = -1, lastChange = 0, burst = 0, unchanged = 0, independent = 0
            for (let t = 0; t < 180000; t += 25) {
                for (const item of [member, chat, other]) item.animationTime = 1700000000000 + t
                compare(member.column, chat.column, "Chat and member list use the same pose")
                if (member.column !== other.column) ++independent
                if (member.column === previous) { ++unchanged; continue }
                if (previous >= 0) {
                    const count = member.clipRow.frames.reduce(function(sum, frames) { return sum + frames }, 0)
                    const distance = Math.abs(member.column - previous)
                    verify(Math.min(distance, count - distance) <= 1, "Movement follows adjacent frames, including at slot boundaries")
                }
                if (lastChange && t - lastChange >= 1000) {
                    holds.add(t - lastChange); poses.add(previous)
                    if (burst > 0) bursts.add(burst)
                    burst = 0
                }
                ++burst; lastChange = t; previous = member.column
            }
            verify(poses.size >= 3, "Idle portraits rest on different drawn poses")
            verify(holds.size >= 4, "Motion starts at irregular intervals")
            verify(bursts.size >= 2, "Some movements are smaller than others")
            verify(unchanged > 7200 * 0.8, "Most time is spent quietly holding a pose")
            verify(independent > 1000, "Different members do not move in unison")
        }
    }

    function test_avatarUnloadsArtworkWhenHiddenAndReloadsAfterStateChanges() {
        const item = createTemporaryObject(variablePortrait, view.contentItem, {
            width: 48, height: 48, circular: true, avatar: "courier"
        })
        verify(item)
        const canvas = findChild(item, "avatarCanvas")
        tryVerify(function() { return fixtures.portraitHasDetail(item) })
        for (const state of [
            {circular: false, online: true, systemMessage: false, animated: true},
            {circular: false, online: false, systemMessage: false, animated: true},
            {circular: true, online: true, systemMessage: true, animated: false},
            {circular: false, online: true, systemMessage: true, animated: false}
        ]) {
            for (const key in state) item[key] = state[key]
            tryVerify(function() { return fixtures.portraitHasDetail(item, !item.online) })
            compare(canvas.loadedAtlas.toString(), item.atlasSource.toString())
        }
        item.visible = false
        compare(canvas.loadedAtlas.toString(), "")
        item.visible = true
        tryVerify(function() { return fixtures.portraitHasDetail(item) })
    }

    function test_denseAvatarFramesRenderAtDisplaySize_data() {
        const cases = []
        for (const id of ["mossling", "courier", "ember-dragon", "system"])
            for (let variant = 0; variant < 3; ++variant)
                cases.push({tag: id + "-clip-" + variant, avatar: id, variant: variant, system: id === "system"})
        return cases
    }
    function test_denseAvatarFramesRenderAtDisplaySize(data) {
        view.width = 1040; view.height = 420
        waitForRendering(view.contentItem)
        portraitGallery.visible = true
        const items = []
        for (let state = 0; state < 5; ++state) {
            const count = Atlas.forAvatar(data.avatar, data.system).rows[state].frames[data.variant]
            for (let frame = 0; frame < count; ++frame) {
                const item = createTemporaryObject(variablePortrait, portraitGallery, {
                    x: 12 + frame * 62, y: 12 + state * 62, z: 2,
                    width: 48, height: 48, avatar: data.system ? "mossling" : data.avatar, identity: "", circular: true, systemMessage: data.system,
                    level: state === 1 ? 0.1 : 0, muted: state === 2,
                    deafened: state === 3, sleeping: state === 4
                })
                verify(item)
                const layout = JSON.parse(JSON.stringify(item.atlasLayout))
                for (const row of layout.rows) row.pause = 0
                item.atlasLayout = layout
                item.animationTime = data.variant * item.cycleLength + (frame + 0.5) * item.clipRow.duration / count
                compare(item.variant, data.variant)
                compare(item.frame, frame)
                compare(item.stateRow, state)
                tryVerify(function() { return fixtures.portraitHasDetail(item, false) }, 5000)
                items.push(item)
            }
        }
        // Hide the ordinary catalog while inspecting the five animation strips.
        for (let i = 0; i < galleryPortraits.count; ++i) galleryPortraits.itemAt(i).visible = false
        waitForRendering(portraitGallery)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/" + data.tag + "-motion.png"))
        for (const item of items) {
            item.muted = false; item.deafened = false
            item.online = false
            compare(item.stateRow, 4); compare(item.frame, 0)
            tryVerify(function() { return fixtures.portraitHasDetail(item, true) }, 5000)
        }
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/" + data.tag + "-offline.png"))
    }

    function test_avatarSupportsOneStillFrameOrAnyClipCountPerState() {
        const item = createTemporaryObject(variablePortrait, view.contentItem, { identity: "" })
        verify(item)
        const counts = [[1], [6], [1, 1], [2, 3, 4, 1], [1]]
        item.atlasLayout = { stride: 32, inset: 0, rows: counts.map(function(frames, row) {
            return { top: row * 32, bottom: (row + 1) * 32,
                     frames: frames, duration: 600, repeats: 1, pause: 0 }
        }) }
        const canvas = findChild(item, "avatarCanvas")
        for (let state = 0; state < counts.length; ++state) {
            item.level = state === 1 ? 0.1 : 0
            item.muted = state === 2; item.deafened = state === 3; item.sleeping = state === 4
            compare(item.stateRow, state)
            for (const time of [0, 100, 599, 600, 1199, 1200, 1800, 2400, 18000]) {
                item.animationTime = time
                verify(item.variant >= 0 && item.variant < counts[state].length)
                verify(item.frame >= 0 && item.frame < counts[state][item.variant])
                compare(canvas.crop.y, state * 32)
                verify(canvas.crop.x >= 0)
                verify(canvas.crop.x + canvas.crop.width <= counts[state].reduce(function(a, b) { return a + b }, 0) * 32)
                if (counts[state].length === 1 && counts[state][0] === 1) {
                    compare(item.variant, 0); compare(item.frame, 0); compare(canvas.crop.x, 0)
                }
            }
        }
        item.sleeping = false; item.level = 0.1; item.animationTime = 599
        compare(item.frame, 5); verify(item.activity > 0)
        item.online = false
        compare(item.stateRow, 4); compare(item.frame, 0); compare(item.activity, 0)
    }

    function test_avatarRowsUseIndependentFrameCountsWithoutChangingDuration() {
        const portrait = createTemporaryObject(variablePortrait, view.contentItem)
        verify(portrait)
        portrait.online = true; portrait.available = true; portrait.animated = true
        portrait.muted = false; portrait.deafened = false; portrait.sleeping = false
        portrait.level = 0.1; portrait.identity = ""
        // The same atlas width holds three clips with unequal frame counts.
        const layout = { stride: 32, inset: 0, rows: [] }
        for (let row = 0; row < 5; ++row)
            layout.rows.push({ top: row * 32, bottom: (row + 1) * 32,
                               frames: row === 1 ? [3, 4, 5] : [2, 2, 2],
                               duration: 600, repeats: 1, pause: row === 1 ? 0 : 4000 })
        portrait.atlasLayout = layout
        compare(Atlas.frameInterval([layout]), 60)
        compare(Atlas.frameInterval(), 21)
        const denseLayout = { rows: [{ frames: [8, 12, 24], duration: 480 }] }
        compare(Atlas.frameInterval([denseLayout]), 16)
        const canvas = findChild(portrait, "avatarCanvas")
        compare(portrait.cycleLength, 600)
        for (const sample of [[0, 0, 0, 0], [599, 0, 2, 64], [600, 1, 0, 96],
                              [1199, 1, 3, 192], [1200, 2, 0, 224], [1799, 2, 4, 352]]) {
            portrait.animationTime = sample[0]
            compare(portrait.variant, sample[1]); compare(portrait.frame, sample[2])
            compare(canvas.crop.x, sample[3]); compare(canvas.crop.y, 32)
            compare(canvas.crop.width, 32); compare(canvas.crop.height, 32)
        }
        portrait.level = 0
        compare(portrait.cycleLength, 4600)
        for (const time of [599, 600, 4599]) {
            portrait.animationTime = time
            verify(portrait.frame >= 0 && portrait.frame < 2)
            compare(canvas.crop.y, 0)
        }
        portrait.online = false; portrait.animationTime = 9500
        compare(portrait.frame, 0); compare(portrait.variant, 0)
        compare(canvas.crop.y, 128)
        portrait.online = true; portrait.animated = false
        compare(portrait.frame, 0); compare(portrait.variant, 0)
    }

    function test_allAvatarCropsFitTheirArtwork() {
        const item = createTemporaryObject(variablePortrait, view.contentItem, { identity: "" })
        verify(item)
        const artwork = createTemporaryObject(atlasImage, view.contentItem)
        const canvas = findChild(item, "avatarCanvas")
        for (const id of session.avatars.concat(["system"])) {
            item.avatar = id; item.systemMessage = id === "system"
            const layout = JSON.parse(JSON.stringify(Atlas.forAvatar(id, id === "system")))
            // Enumerate every crop independently of the randomized idle clock.
            for (const row of layout.rows) row.pause = 0
            item.atlasLayout = layout
            tryVerify(function() { return canvas.isImageLoaded(canvas.atlas) })
            artwork.source = findChild(item, "avatarCanvas").atlas
            tryCompare(artwork, "status", Image.Ready)
            compare(layout.rows.length, 5)
            compare(layout.rows[0].top, 0)
            compare(layout.rows[4].bottom, artwork.sourceSize.height)
            for (let row = 0; row < 5; ++row) {
                if (row > 0) compare(layout.rows[row].top, layout.rows[row - 1].bottom)
                item.level = row === 1 ? 0.1 : 0
                item.muted = row === 2; item.deafened = row === 3; item.sleeping = row === 4
                compare(item.stateRow, row)
                const clip = item.clipRow
                compare(clip.frames.length, 3)
                verify(clip.duration > 0 && clip.repeats >= 1 && clip.pause >= 0)
                for (let variant = 0; variant < 3; ++variant) {
                    verify(Number.isInteger(clip.frames[variant]) && clip.frames[variant] > 0)
                    for (let frame = 0; frame < clip.frames[variant]; ++frame) {
                        item.animationTime = variant * item.cycleLength + (frame + 0.5) * clip.duration / clip.frames[variant]
                        compare(item.variant, variant); compare(item.frame, frame)
                        const crop = canvas.crop
                        compare(crop.y, clip.top + layout.inset, id + ": every variant stays on its state's physical row")
                        verify(crop.x >= 0 && crop.y >= 0 && crop.width > 0 && crop.height > 0)
                        verify(crop.x + crop.width <= artwork.sourceSize.width, id + " horizontal crop")
                        verify(crop.y + crop.height <= artwork.sourceSize.height, id + " vertical crop")
                    }
                }
            }
        }
    }

    function test_channelTreeAndIconControlsFitCompactWindow() {
        verify(fixtures.startHost())
        verify(channel.decide(remoteChannel.ownId, true))
        verify(channel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        verify(remoteChannel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(channel, "chatReady", true)
        tryCompare(remoteChannel, "chatReady", true)
        tryVerify(function() { return channel.participants.length === 2 })
        for (const size of [[460, 560], [360, 360]]) {
            view.width = size[0]; view.height = size[1]
            waitForRendering(view.contentItem)
            const channelScroll = findChild(view, "channelsScroll")
            const send = findChild(view, "sendChat")
            tryVerify(function() {
                const position = send.mapToItem(channelScroll, 0, 0)
                return position.y >= 0 && position.y + send.height <= channelScroll.height
            }, 1500, "The composer stays fully usable after shrinking the channel window")
            for (const name of ["mute", "deafen", "leaveChannel"]) {
                const control = findChild(view, name)
                const pos = control.mapToItem(view.contentItem, 0, 0)
                verify(control.visible)
                verify(pos.x >= 0 && pos.x + control.width <= view.width, name)
                verify(control.iconOnly)
            }
            verify(!findChild(view, "remoteToggle").visible)
            verify(session.available)
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/channels-new-" + size[0] + ".png"))
        }
        channel.leave()
        const row = visualChild(view.contentItem, "channelRow_" + channel.ownId)
        verify(row)
        waitForRendering(row)
        mouseDoubleClickSequence(row, 35, row.height / 2)
        tryCompare(channel, "joined", true)
        mouseClick(findChild(view, "deafen")); compare(session.deafened, true)
        compare(findChild(view, "deafen").glyph, "deafen")
        mouseClick(findChild(view, "deafen")); compare(session.deafened, false)
        mouseClick(findChild(view, "openSettings"))
        tryCompare(findChild(view, "settingsDialog"), "opened", true)
        verify(session.available)
        if (imageDirectory.length > 0) {
            view.width = 560; view.height = 720
            for (let page = 0; page < 4; ++page) {
                view.settingsPage = page
                findChild(view, "settingsScroll").contentItem.contentY = 0
                waitForRendering(view.contentItem)
                verify(fixtures.saveWindow(view, imageDirectory + "/settings-" + ["general", "input", "output", "about"][page] + ".png"))
            }
        }
    }

    function test_aboutUpdateCheckOnlyAppearsForAnAvailableUpdater() {
        session.setMuted(false)
        view.openSettings(3)
        const button = findChild(view, "checkUpdates")
        verify(button && !button.visible)
        updateDisplay.available = false; updateDisplay.checks = 0
        view.updates = updateDisplay
        verify(!button.visible)
        updateDisplay.available = true
        tryCompare(button, "visible", true)
        reveal(button); mouseClick(button)
        compare(updateDisplay.checks, 1)
        verify(session.available && session.transmissionAllowed)
        view.closeSettings()
    }

    function test_aboutUsesLogoVersionRotatingTextAndVerifiedSupportLinksWithoutMuting() {
        session.setMuted(false)
        view.openSettings(3)
        const about = findChild(view, "aboutSettings")
        verify(about && about.visible)
        verify(session.available && session.transmissionAllowed)
        tryCompare(findChild(view, "aboutLogo"), "status", Image.Ready)
        verify(findChild(view, "aboutVersion").text.indexOf(Qt.application.version) >= 0)
        verify(findChild(view, "aboutLicense").text.indexOf("GPL-3.0") >= 0)
        let previous = findChild(view, "aboutSentence").text
        verify(previous.length > 0)
        for (let i = 0; i < 8; ++i) {
            view.closeSettings(); view.openSettings(3)
            const next = findChild(view, "aboutSentence").text
            verify(next.length > 0 && next !== previous)
            previous = next
            verify(session.available && session.transmissionAllowed)
        }
        const expected = {
            repository: "https://github.com/YunaBraska/SquadSpeak",
            issues: "https://github.com/YunaBraska/SquadSpeak/issues/new",
            sponsors: "https://github.com/sponsors/YunaBraska",
            coffee: "https://buymeacoffee.com/YunaBraska",
            kofi: "https://ko-fi.com/YunaBraska",
            liberapay: "https://liberapay.com/YunaBraska"
        }
        for (const key of Object.keys(expected)) {
            const button = visualChild(findChild(view, "settingsDialog").contentItem, "aboutLink_" + key)
            verify(button); reveal(button); openedLinks.clear(); mouseClick(button)
            compare(openedLinks.count, 1)
            compare(openedLinks.signalArguments[0][0], expected[key])
        }
        findChild(view, "settingsScroll").contentItem.contentY = 0
        waitForRendering(view.contentItem)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/settings-about.png"))
        view.width = 360; view.height = 480
        waitForRendering(view.contentItem)
        verify(about.width <= findChild(view, "settingsScroll").availableWidth)
        view.closeSettings()
    }

    function test_palettesAndStationCrudInSettings() {
        view.openSettings(0)
        const palette = findChild(view, "paletteChoice")
        verify(palette)
        session.setMuted(false)
        for (const mode of ["light", "dark"]) {
            verify(session.setTheme(mode))
            for (let i = 0; i < 4; ++i) {
                palette.currentIndex = i
                palette.activated(i)
                compare(session.palette, ["plum", "ocean", "forest", "graphite"][i])
                verify(session.transmissionAllowed)
                waitForRendering(view.contentItem)
                if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/theme-" + mode + "-" + session.palette + ".png"))
            }
        }
        session.setPalette("plum"); session.setTheme("system")
        verify(fixtures.startHost())
        view.closeSettings()
        view.selectedHost = channel.ownId
        const browse = findChild(view, "browseStations")
        const menu = browse.menu
        menu.popup(); tryCompare(menu, "opened", true)
        verify(browse.visible)
        waitForRendering(browse); mouseClick(browse)
        const results = findChild(view, "radioResults")
        tryCompare(results, "count", 50)
        verify(results.total > 2000)
        results.positionViewAtEnd()
        tryVerify(function() { return results.count > 50 })
        const search = findChild(view, "radioSearch")
        search.text = "classical"
        tryVerify(function() { return results.query === "classical" && results.count > 0 })
        search.text = "unfindablexyzq"
        tryCompare(results, "total", 0)
        search.text = ""
        tryCompare(results, "count", 50)
        waitForRendering(view.contentItem)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/radio-browser.png"))
        const add = findChild(view, "addStation")
        mouseClick(add)
        const name = "<b>Station test</b>"
        findChild(view, "stationName").text = name
        findChild(view, "stationUrl").text = fixtures.stationUrl()
        mouseClick(findChild(view, "saveStation"))
        tryCompare(findChild(view, "stationEditor"), "visible", false, 17000)
        compare(radio.stations.length, 1)
        const id = radio.stations[0].id
        tryCompare(results.model.get(0).entry, "id", id)
        results.positionViewAtBeginning()
        results.forceLayout()
        const play = visualChild(findChild(view, "radioBrowser").contentItem, "playStation_" + id)
        verify(play); mouseClick(play)
        verify(radio.active); compare(radio.selectedId, id)
        tryCompare(radio, "state", "playing", 17000)
        verify(findChild(view, "radioStreamStatus").visible)
        verify(findChild(view, "radioStreamStatus").text.indexOf(name) >= 0)
        compare(findChild(view, "radioStreamStatus").textFormat, Text.PlainText)
        mouseClick(findChild(view, "stopCurrentStation"))
        compare(radio.active, false)
        verify(radio.removeStation(id))
        compare(radio.stations.length, 0)
        view.closeSettings()
    }

    function test_profileCanBeEditedWithoutMutingVoice() {
        session.setMuted(false)
        const previous = session.userName
        view.openSettings(0)
        const dialog = findChild(view, "settingsDialog")
        tryCompare(dialog, "opened", true)
        verify(session.available)
        verify(session.transmissionAllowed)
        const avatar = visualChild(dialog.contentItem, "avatarChoice_mechanic")
        verify(avatar)
        waitForRendering(avatar)
        if (imageDirectory.length > 0) fixtures.saveWindow(view, imageDirectory + "/profile-before.png")
        mouseClick(avatar)
        compare(session.avatar, "mechanic")
        const name = findChild(dialog, "userName")
        name.text = "New name"; mouseClick(findChild(dialog, "saveDisplayName"))
        compare(session.userName, "New name")
        verify(session.transmissionAllowed)
        name.text = "  "; mouseClick(findChild(dialog, "saveDisplayName"))
        compare(session.userName, "New name")
        verify(session.error.length > 0)
        name.text = "New name"; verify(session.setUserName("New name"))
        for (const mode of ["light", "dark", "system"]) {
            verify(session.setTheme(mode))
            tryCompare(Theme, "mode", mode)
            if (mode !== "system") compare(Theme.dark, mode === "dark")
            waitForRendering(dialog.contentItem)
            if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/settings-" + mode + ".png"))
        }
        dialog.close()
        session.setUserName(previous)
        session.setAvatar("mossling")
        view.width = 560; view.height = 720
        view.openSettings(0)
        tryCompare(dialog, "opened", true)
        const shelf = findChild(view, "avatarShelf")
        tryVerify(function() {
            let visible = 0
            for (const id of session.avatars) {
                const choice = visualChild(dialog.contentItem, "avatarChoice_" + id)
                if (!choice || !choice.visible) continue
                const canvas = findChild(choice, "avatarCanvas")
                const avatar = canvas && canvas.parent
                if (!avatar) return false
                const position = avatar.mapToItem(shelf, 0, 0)
                if (position.x < 0 || position.x + avatar.width > shelf.width) continue
                if (!fixtures.portraitHasDetail(avatar, !avatar.online)) return false
                ++visible
            }
            return visible >= 3
        }, 5000, "Visible neighbouring portraits reload when settings reopen")
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/settings-general.png"))
    }

    function test_longNamesKeepConversationControlsInsideCompactWindow() {
        const previousUser = session.userName
        const previousChannel = channel.channelName
        const longName = "W".repeat(64)
        try {
            verify(session.setUserName(longName))
            verify(channel.setChannelName(longName))
            verify(fixtures.startHost())
            verify(channel.join(channel.ownId, "127.0.0.1", channel.servicePort))
            tryCompare(channel, "chatReady", true)
            view.width = 400
            view.height = 420
            tryCompare(visualChild(view.contentItem, "channelName_" + channel.ownId), "text", longName)
            waitForRendering(view.contentItem)
            for (const name of ["mute", "leaveChannel"] ) {
                const control = findChild(view, name)
                const pos = control.mapToItem(view.contentItem, 0, 0)
                verify(control.visible)
                verify(pos.x >= 0 && pos.x + control.width <= view.width,
                    name + " bleibt bei langen Namen im Fenster")
            }
        } finally {
            session.setUserName(previousUser)
            channel.setChannelName(previousChannel)
        }
    }

    function test_passwordDialogsSetSaveJoinAndRemove() {
        view.width = 400
        view.height = 420
        const edit = findChild(view, "editHostPassword")
        waitForRendering(view.contentItem)
        reveal(edit)
        mouseClick(edit)
        const hostDialog = findChild(view, "hostPasswordDialog")
        tryCompare(hostDialog, "opened", true)
        findChild(hostDialog, "hostPasswordValue").text = "local secret"
        mouseClick(findChild(hostDialog, "saveHostPassword"))
        tryCompare(channel, "passwordBusy", false)
        compare(channel.passwordProtected, true)
        verify(fixtures.startRemoteHost())
        verify(remoteChannel.decide(channel.ownId, true))
        verify(remoteChannel.setHostPassword("remote secret"))
        tryCompare(remoteChannel, "passwordBusy", false)
        verify(channel.join(remoteChannel.ownId, "127.0.0.1", remoteChannel.servicePort))
        const dialog = findChild(view, "joinPasswordDialog")
        tryCompare(dialog, "opened", true)
        const value = findChild(dialog, "joinPasswordValue")
        compare(value.echoMode, TextInput.Password)
        value.text = "remote secret"
        findChild(dialog, "rememberChannelPassword").checked = true
        const submit = findChild(dialog, "submitChannelPassword")
        const bounds = submit.mapToItem(dialog.contentItem, 0, 0)
        verify(bounds.y >= 0 && bounds.y + submit.height <= dialog.contentItem.height)
        if (imageDirectory.length > 0) fixtures.saveWindow(view, imageDirectory + "/channel-password.png")
        mouseClick(submit)
        tryCompare(channel, "chatReady", true)
        tryCompare(dialog, "opened", false)
        compare(channel.passwordSaved, true)
        verify(session.available)
        reveal(edit)
        mouseClick(edit)
        tryCompare(hostDialog, "opened", true)
        compare(findChild(hostDialog, "hostPasswordValue").text, "")
        mouseClick(findChild(hostDialog, "removeHostPassword"))
        compare(channel.passwordProtected, false)
        channel.forgetPassword()
        channel.leave()
        remoteChannel.stopHost()
    }

    function test_hostGrantsAndRevokesControlThroughAccessibilityWithoutClientPairing() {
        verify(fixtures.startHost())
        verify(channel.decide(remoteChannel.ownId, true))
        verify(remoteChannel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(remoteChannel, "chatReady", true)
        view.memberOptions({id: remoteChannel.ownId, name: remoteSession.userName}, channel.ownId)
        const grant = findChild(view, "allowRemoteControl")
        verify(grant !== null)
        tryCompare(grant.menu, "opened", true)
        verify(grant.visible && grant.enabled)
        waitForRendering(grant)
        verify(fixtures.pressAccessible(grant))
        tryVerify(function() { return remoteChannel.remoteOffers.length === 1 })
        verify(!remoteChannel.remoteMode)
        compare(channel.controlRequests.length, 0)
        controllerView.show()
        const toggle = findChild(controllerView, "remoteToggle")
        tryCompare(toggle, "visible", true)
        remoteSession.setPttButtonHeld(true)
        mouseClick(toggle)
        verify(!remoteChannel.remoteMode)
        verify(controllerView.notice.includes("Release"))
        remoteSession.setPttButtonHeld(false)
        mouseClick(toggle)
        tryCompare(remoteChannel, "remoteAllowed", true)
        verify(!remoteChannel.joined)
        controllerView.close()
        const info = openOwnChannelInfo()
        const manage = visualChild(info.contentItem, "manageMember_" + remoteChannel.ownId)
        verify(manage !== null, "Control can still be revoked after the member switches to controller mode")
        reveal(manage); mouseClick(manage)
        tryCompare(grant.menu, "opened", true)
        verify(grant.checked)
        waitForRendering(grant)
        verify(fixtures.pressAccessible(grant))
        tryCompare(remoteChannel, "remoteMode", false)
        tryCompare(toggle, "visible", false)
        compare(channel.controllers.length, 0)
    }

    function test_memberTooltipDisplaysTheLiteralName_data() {
        return [{tag: "plain", name: "Member & Studio"},
                {tag: "markup", name: "<b>Member</b>"},
                {tag: "font", name: "<font size='1'>Member</font>"},
                {tag: "image", name: "<img src='http://127.0.0.1:1/avatar.png'>"}]
    }
    function test_memberTooltipDisplaysTheLiteralName(data) {
        const previous = remoteSession.userName
        try {
            verify(remoteSession.setUserName(data.name))
            verify(fixtures.startHost())
            verify(channel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
            tryCompare(channel, "chatReady", true)
            verify(channel.decide(remoteChannel.ownId, true))
            verify(remoteChannel.join(channel.ownId, "127.0.0.1", channel.servicePort))
            tryCompare(remoteChannel, "chatReady", true)
            const avatarName = "channelAvatar_" + channel.ownId + "_" + remoteChannel.ownId
            tryVerify(function() { return visualChild(view.contentItem, avatarName) !== null })
            const avatar = visualChild(view.contentItem, avatarName)
            mouseMove(avatar, avatar.width / 2, avatar.height / 2)
            const tip = findChild(avatar, "memberTooltip")
            tryCompare(tip, "visible", true)
            compare(tip.text, data.name)
            const reference = createTemporaryObject(literalName, test, {text: data.name, font: tip.font})
            verify(reference)
            waitForRendering(tip.contentItem)
            fuzzyCompare(tip.contentItem.implicitWidth, reference.implicitWidth, 0.1)
            if (imageDirectory.length > 0 && data.tag === "markup") {
                tryVerify(function() { return fixtures.portraitHasDetail(avatar) })
                verify(fixtures.saveWindow(view, imageDirectory + "/literal-member-tooltip.png"))
            }
            tip.close()
        } finally {
            verify(remoteSession.setUserName(previous))
        }
    }

    function test_remoteViewUsesTargetIdentityAndExclusiveMode() {
        verify(fixtures.startHost())
        verify(channel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(channel, "chatReady", true)
        verify(channel.decide(remoteChannel.ownId, true))
        verify(remoteChannel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(remoteChannel, "chatReady", true)
        view.memberOptions({id: remoteChannel.ownId, name: remoteSession.userName}, channel.ownId)
        const grant = findChild(view, "allowRemoteControl")
        verify(grant !== null)
        tryCompare(grant.menu, "opened", true)
        verify(grant.visible && grant.enabled)
        waitForRendering(grant)
        mouseClick(grant)
        tryVerify(function() { return remoteChannel.remoteOffers.length === 1 })
        verify(remoteChannel.chooseRemoteOffer(channel.ownId))
        tryCompare(remoteChannel, "remoteAllowed", true)
        tryVerify(function() { return remoteChannel.remoteView.joined === true })
        compare(remoteChannel.joined, false)
        controllerView.show()
        tryCompare(controllerView, "activeHost", channel.ownId)
        mouseClick(findChild(controllerView, "mute"))
        tryCompare(session, "muted", false)
        mouseClick(findChild(controllerView, "deafen"))
        tryCompare(session, "deafened", true)
        controllerView.chatExpanded = true
        const chat = findChild(controllerView, "chatPanel")
        tryCompare(chat, "visible", true)
        const draft = findChild(chat, "chatDraft")
        draft.text = "Sent from the remote keyboard"
        const send = findChild(chat, "sendChat")
        tryCompare(send, "enabled", true); mouseClick(send)
        tryCompare(draft, "text", "")
        const last = channel.messages[channel.messages.length - 1]
        compare(last.sender, channel.ownId)
        compare(last.name, session.userName)
        verify(!remoteChannel.joined)
        tryVerify(function() {
            return visualChild(controllerView.contentItem, "channelAvatar_" + channel.ownId + "_" + channel.ownId) !== null
                && visualChild(chat, "messageAvatar_" + channel.ownId) !== null
        })
        const rowPortrait = visualChild(controllerView.contentItem, "channelAvatar_" + channel.ownId + "_" + channel.ownId)
        const messagePortrait = visualChild(chat, "messageAvatar_" + channel.ownId)
        verify(rowPortrait !== null && messagePortrait !== null)
        const levels = ({})
        levels[channel.ownId] = 0.1
        verify(fixtures.publishLevels(levels))
        tryVerify(function() { return rowPortrait.activity > 0 && messagePortrait.activity > 0 })
        compare(messagePortrait.stateRow, rowPortrait.stateRow)
        for (const time of [0, 400, 1000, 1700, 4500, 9100]) {
            controllerView.avatarTime = time
            compare(messagePortrait.frame, rowPortrait.frame)
            compare(messagePortrait.variant, rowPortrait.variant)
        }
        remoteSession.setAnimatedAvatars(false)
        compare(messagePortrait.frame, 0); compare(rowPortrait.frame, 0)
        verify(messagePortrait.activity > 0, "Disabling animation preserves the speaking indicator")
        remoteSession.setAnimatedAvatars(true)
        session.setDeafened(false)
        session.setMuted(true)
        tryCompare(messagePortrait, "stateRow", 2)
        tryVerify(function() {
            const updated = visualChild(controllerView.contentItem, "channelAvatar_" + channel.ownId + "_" + channel.ownId)
            return updated !== null && updated.stateRow === 2 && updated.activity === 0
        })
        if (imageDirectory.length > 0) fixtures.saveWindow(controllerView, imageDirectory + "/remote-chat.png")
        controllerView.chatExpanded = false
        mouseClick(findChild(controllerView, "remoteToggle"))
        tryCompare(remoteChannel, "remoteMode", false)
        verify(channel.decideControl(remoteChannel.ownId, false))
        session.setDeafened(false)
    }

    function test_chatSendsOnlyAfterHostReceiptAndKeepsVoiceAvailable_data() {
        return [{tag: "English", language: "en"}, {tag: "Arabic", language: "ar"}]
    }
    function test_chatSendsOnlyAfterHostReceiptAndKeepsVoiceAvailable(data) {
        verify(session.setLanguage(data.language))
        const previousName = session.userName
        const previousChannel = channel.channelName
        verify(session.setUserName("Alex"))
        verify(channel.setChannelName("Evening lounge"))
        view.width = 560
        view.height = 780
        verify(fixtures.startHost())
        verify(channel.decide(remoteChannel.ownId, true))
        verify(channel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        verify(remoteChannel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(channel, "chatReady", true)
        tryCompare(remoteChannel, "chatReady", true)
        tryCompare(channel, "historyLoading", false)
        tryVerify(function() { return channel.messages.filter(function(m) { return !!m.event }).length === 2 })
        const initialCount = channel.messages.length
        view.chatExpanded = true
        const panel = findChild(view, "chatPanel")
        tryCompare(panel, "visible", true)
        const draft = findChild(panel, "chatDraft")
        const send = findChild(panel, "sendChat")
        const markdown = "## Tonight's plan\n\n**Everyone is welcome.** Start whenever you are ready.\n\n| Time | Plan |\n|---|---|\n| 20:30 | Meet in voice |\n| 21:00 | Pick something together |"
        verify(session.available)
        draft.text = markdown
        tryCompare(send, "enabled", true)
        mouseClick(send)
        tryCompare(draft, "text", "")
        tryVerify(function() {
            const received = remoteChannel.messages.filter(function(message) { return message.text === markdown })
            return received.length > 0
        })
        compare(channel.messages[channel.messages.length - 1].text, markdown)
        compare(remoteChannel.messages[remoteChannel.messages.length - 1].text, markdown)
        verify(remoteChannel.sendChat("> Start whenever you are ready.\n\nSounds good! I brought **snacks**. Notes: https://example.org\n\n- Voice first\n- No rush"))
        tryCompare(remoteChannel, "chatPending", false)
        tryCompare(findChild(panel, "chatHistory"), "count", initialCount + 2)
        compare(channel.messages[initialCount + 1].sender, remoteChannel.ownId)
        draft.text = "Tip: use `Shift+Enter` for a new line.\n\n```text\nGood company. Clear voices.\n```"
        tryCompare(send, "enabled", true)
        mouseClick(send)
        tryCompare(draft, "text", "")
        tryCompare(findChild(panel, "chatHistory"), "count", initialCount + 3)
        verify(session.available)
        const history = findChild(panel, "chatHistory")
        waitForRendering(panel)
        tryVerify(function() {
            history.forceLayout()
            history.positionViewAtBeginning()
            return history.atYBeginning && history.itemAtIndex(0) !== null
        })
        const systemRow = history.itemAtIndex(0)
        verify(systemRow && systemRow.systemMessage && !systemRow.own)
        const systemAvatar = findChild(systemRow, "messageAvatar_" + systemRow.message.sender)
        verify(systemAvatar.systemMessage && systemAvatar.online && systemAvatar.animated)
        compare(findChild(systemRow, "messageText_" + systemRow.message.sequence).textFormat, TextEdit.PlainText)
        const joinedText = data.language === "ar" ? "%1 انضم." : "%1 joined."
        compare(findChild(systemRow, "messageText_" + systemRow.message.sequence).text, joinedText.arg(systemRow.message.event.name))
        const originalEvent = systemRow.message
        const futureText = "<b>Future notice</b> **literal fallback**"
        systemRow.message = Object.assign({}, originalEvent, {event: {kind: "future.notice.v2"}, text: futureText})
        const futureNotice = findChild(systemRow, "messageText_" + originalEvent.sequence)
        compare(futureNotice.textFormat, TextEdit.PlainText)
        compare(futureNotice.text, futureText)
        waitForRendering(panel)
        tryVerify(function() {
            history.positionViewAtBeginning()
            const row = history.itemAtIndex(0)
            const avatar = row ? findChild(row, "messageAvatar_" + row.message.sender) : null
            return avatar !== null && fixtures.portraitHasDetail(avatar)
        }, 5000,
            "Future System events retain a visible bot portrait")
        if (imageDirectory.length > 0) {
            waitForRendering(panel)
            verify(fixtures.saveWindow(view, imageDirectory + "/chat-future-event-" + data.language + ".png"))
        }
        systemRow.message = originalEvent
        tryVerify(function() { return history.itemAtIndex(initialCount) !== null && history.itemAtIndex(initialCount + 1) !== null })
        const own = history.itemAtIndex(initialCount), other = history.itemAtIndex(initialCount + 1)
        verify(own && other)
        verify(findChild(other, "messageAvatar_" + remoteChannel.ownId).animated,
            "Chat portraits follow the channel animation preference")
        compare(own.own, true); compare(other.own, false)
        verify(findChild(own, "messageBody_" + own.message.sequence).x > findChild(other, "messageBody_" + other.message.sequence).x)
        const previousRemoteName = remoteSession.userName
        verify(remoteSession.setUserName("Mira updated")); verify(remoteSession.setAvatar("mechanic"))
        tryVerify(function() { return other.message.name === "Mira updated" })
        tryVerify(function() { return other.message.avatarId === "mechanic" })
        verify(remoteSession.setUserName(previousRemoteName)); verify(remoteSession.setAvatar("courier"))
        tryVerify(function() { return other.message.name === previousRemoteName })
        compare(findChild(view, "audioQuality").bitrate, 32)
        const ownPortrait = findChild(own, "messageAvatar_" + channel.ownId)
        const otherPortrait = findChild(other, "messageAvatar_" + remoteChannel.ownId)
        tryVerify(function() { return fixtures.portraitHasDetail(ownPortrait, false) }, 5000, "Own portrait is rendered")
        tryVerify(function() { return fixtures.portraitHasDetail(otherPortrait, false) }, 5000, "Member portrait is rendered")
        if (imageDirectory.length > 0) {
            findChild(panel, "chatHistory").positionViewAtBeginning()
            waitForRendering(panel)
            verify(fixtures.saveWindow(view, imageDirectory + "/channel-chat.png"))
        }
        verify(remoteChannel.closeChat(channel.ownId))
        tryVerify(function() { return channel.chatPresenceKnown && !channel.chatOnlineIds.includes(remoteChannel.ownId) })
        const offlinePortrait = visualChild(panel, "messageAvatar_" + remoteChannel.ownId)
        verify(offlinePortrait !== null); compare(offlinePortrait.online, false); compare(offlinePortrait.stateRow, 4)
        waitForRendering(panel)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/chat-offline-avatar.png"))
        verify(fixtures.publishSystem("**Server notice**: https://example.org"))
        tryVerify(function() { return channel.messages.some(function(m) { return m.event && m.event.kind === "announcement" }) })
        tryCompare(history, "count", channel.messages.length)
        const announcementIndex = channel.messages.findIndex(function(m) { return m.event && m.event.kind === "announcement" })
        tryVerify(function() {
            history.forceLayout()
            history.positionViewAtEnd()
            return history.atYEnd && history.itemAtIndex(announcementIndex) !== null
        })
        const announcement = history.itemAtIndex(announcementIndex)
        verify(announcement && announcement.systemMessage && !announcement.own)
        const notice = findChild(announcement, "messageText_" + announcement.message.sequence)
        compare(notice.textFormat, TextEdit.RichText)
        verify(notice.text.indexOf("Server notice") >= 0 && notice.text.indexOf("href=") >= 0)
        view.chatExpanded = false
        remoteChannel.leave()
        channel.leave()
        channel.stopHost()
        verify(session.setUserName(previousName))
        verify(channel.setChannelName(previousChannel))
    }

    function test_multipleChatsKeepDraftsAndVoiceSeparate() {
        const previousName = session.userName, previousChannel = channel.channelName
        const previousRemote = remoteChannel.channelName
        session.setUserName("Alex"); channel.setChannelName("Workshop")
        remoteChannel.setChannelName("Evening lounge")
        view.width = 460; view.height = 640
        verify(fixtures.startHost()); verify(fixtures.startRemoteHost())
        verify(remoteChannel.decide(channel.ownId, true))
        verify(channel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        verify(remoteChannel.join(remoteChannel.ownId, "127.0.0.1", remoteChannel.servicePort))
        tryVerify(function() { return channel.chatReady && remoteChannel.chatReady })
        verify(channel.sendChat("Notes stay here while we talk elsewhere.")); tryCompare(channel, "chatPending", false)
        verify(remoteChannel.sendChat("**Welcome back!**\n\nTea, music and a little company. https://example.org")); tryCompare(remoteChannel, "chatPending", false)
        verify(channel.openChat(remoteChannel.ownId, "127.0.0.1", remoteChannel.servicePort))
        tryCompare(channel, "chatReady", true)
        compare(channel.joinedHostId, channel.ownId)
        const panel = findChild(view, "chatPanel"), draft = findChild(panel, "chatDraft")
        tryCompare(panel, "visible", true)
        const loungeSlot = visualChild(view.contentItem, "conversationSlot_" + remoteChannel.ownId)
        verify(loungeSlot !== null, "Chat is inside its expanded channel")
        compare(panel.parent.parent, loungeSlot)
        draft.text = "A draft for the lounge"
        const ownRow = visualChild(view.contentItem, "channelRow_" + channel.ownId)
        waitForRendering(ownRow)
        mouseClick(ownRow, 35, ownRow.height / 2)
        tryCompare(channel, "chatHostId", channel.ownId)
        compare(channel.joinedHostId, channel.ownId)
        compare(draft.text, "")
        draft.text = "A separate workshop draft"
        let other = visualChild(view.contentItem, "channelRow_" + remoteChannel.ownId)
        waitForRendering(other)
        mouseClick(other, 35, other.height / 2)
        tryCompare(channel, "chatHostId", remoteChannel.ownId)
        compare(draft.text, "A draft for the lounge")
        compare(panel.parent.parent, loungeSlot)
        const ownSlot = visualChild(view.contentItem, "conversationSlot_" + channel.ownId)
        verify(!ownSlot.visible && loungeSlot.visible)
        view.showHostChat(remoteChannel.ownId)
        compare(panel.visible, false)
        view.showHostChat(remoteChannel.ownId)
        tryCompare(panel, "visible", true)
        waitForRendering(panel)
        compare(draft.text, "A draft for the lounge")
        draft.text = "Reading here, still talking in **Workshop**."
        const send = findChild(panel, "sendChat")
        tryCompare(send, "enabled", true); mouseClick(send); tryCompare(draft, "text", "")
        tryCompare(findChild(panel, "chatHistory"), "count", 3)
        waitForRendering(panel)
        if (imageDirectory.length > 0) verify(fixtures.saveWindow(view, imageDirectory + "/chat-multiple-hosts.png"))
        other = visualChild(view.contentItem, "channelRow_" + remoteChannel.ownId)
        waitForRendering(other)
        mouseDoubleClickSequence(other, 35, other.height / 2)
        tryCompare(channel, "joinedHostId", remoteChannel.ownId)
        tryCompare(channel, "joined", true)
        session.setUserName(previousName); channel.setChannelName(previousChannel); remoteChannel.setChannelName(previousRemote)
    }

    function test_scrollLoadsOlderMessagesWithoutDownloadingThemAtOpen() {
        verify(fixtures.expireChat())
        verify(fixtures.startHost()); verify(channel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryVerify(function() { return channel.chatReady && !channel.historyLoading })
        for (let i = 0; i < 55; ++i) {
            if (i % 10 === 0) fixtures.advanceTime()
            verify(channel.sendChat("Scroll fixture " + i)); tryCompare(channel, "chatPending", false)
        }
        channel.closeChat(channel.ownId); verify(channel.openChat(channel.ownId, "127.0.0.1", channel.servicePort))
        tryVerify(function() { return channel.chatReady && !channel.historyLoading })
        compare(channel.messages.length, 40)
        verify(channel.hasOlderMessages)
        const panel = findChild(view, "chatPanel"), history = findChild(panel, "chatHistory")
        view.chatExpanded = true
        tryCompare(history, "count", 40)
        tryCompare(panel, "updating", false)
        history.positionViewAtBeginning()
        waitForRendering(history)
        compare(channel.messages.length, 40)
        tryCompare(history, "atYBeginning", true)
        mouseWheel(history, history.width / 2, history.height / 2, 0, 120)
        tryCompare(history, "count", 55)
        verify(!channel.hasOlderMessages)
        compare(channel.messages[0].text, "Scroll fixture 0")
    }

    function test_pushToTalkPreservesMuteAndReleasesOnKeyUp() {
        view.width = 400
        const mode = findChild(view, "pushToTalkMode")
        const button = findChild(view, "pttButton")
        verify(!button.visible)
        reveal(mode)
        mouseClick(mode)
        findChild(view, "settingsDialog").close()
        tryCompare(button, "visible", true)
        const position = button.mapToItem(view.contentItem, 0, 0)
        verify(position.x >= 0 && position.x + button.width <= view.width)
        mousePress(button)
        compare(session.pttHeld, true)
        compare(session.transmissionAllowed, false)
        session.setMuted(false)
        compare(session.transmissionAllowed, true)
        session.setAudioSettingsOpen(true)
        compare(session.transmissionAllowed, false)
        mouseRelease(button)
        compare(session.pttHeld, false)
        session.setAudioSettingsOpen(false)
        compare(session.transmissionAllowed, false)
        reveal(mode)
        mouseClick(mode)
        tryCompare(session, "pushToTalk", false)
        compare(session.transmissionAllowed, false)
        findChild(view, "settingsDialog").close()
        tryCompare(session, "transmissionAllowed", true)
        view.width = 520
    }

    function test_compactChatSendsPreparedImageWithoutText_data() {
        return [{tag: "minimum", height: 360}, {tag: "compact", height: 420}]
    }
    function test_compactChatSendsPreparedImageWithoutText(data) {
        view.width = 400
        view.height = data.height
        verify(fixtures.startHost())
        verify(channel.join(channel.ownId, "127.0.0.1", channel.servicePort))
        tryCompare(channel, "chatReady", true)
        view.chatExpanded = true
        const panel = findChild(view, "chatPanel")
        tryCompare(panel, "visible", true)
        panel.attachmentHost = channel.chatHostId
        verify(chatContent.prepareFile(chatImageFile))
        tryCompare(chatContent, "busy", false)
        compare(chatContent.error, "")
        const draft = findChild(panel, "chatDraft")
        const send = findChild(panel, "sendChat")
        draft.clear()
        tryCompare(send, "enabled", true)
        waitForRendering(panel)
        const position = send.mapToItem(view.contentItem, 0, 0)
        verify(position.x >= 0 && position.x + send.width <= view.width)
        tryVerify(function() {
            const inside = send.mapToItem(panel, 0, 0)
            return inside.y >= 0 && inside.y + send.height <= panel.height
        }, 5000, "Send stays inside the chat at the supported window height")
        if (imageDirectory.length > 0) fixtures.saveWindow(view, imageDirectory + "/channel-chat-compact-draft.png")
        mouseClick(send)
        tryCompare(chatContent, "hasImage", false)
        tryVerify(function() { return channel.messages.some(function(message) { return message.image !== undefined }) })
        const last = channel.messages[channel.messages.length - 1]
        verify(last.text.includes("attachment:" + last.image.hash))
        tryVerify(function() { return channel.imageSource(last.image.hash).length > 0 })
        verify(session.available)
        if (imageDirectory.length > 0) {
            waitForRendering(panel)
            fixtures.saveWindow(view, imageDirectory + "/channel-chat-compact.png")
        }
        view.chatExpanded = false
        channel.leave()
        channel.stopHost()
        view.width = 520
        view.height = 700
    }

    function test_directAddressControlsAndErrors() {
        const field = findChild(view, "directAddress")
        const join = findChild(view, "joinAddress")
        const approve = findChild(view, "approveAddress")
        verify(field && join && approve)
        findChild(view, "discoverDialog").open()
        waitForRendering(field)
        tryCompare(field, "visible", true)
        compare(join.enabled, false)
        compare(approve.enabled, false)
        field.text = "127.0.0.1:65536"
        tryCompare(join, "enabled", true)
        mouseClick(join)
        verify(channel.status.includes("valid host or IP address"))
        verify(!channel.joined)
        verify(!channel.directBusy)
        field.text = "[::1]:0"
        mouseClick(approve)
        verify(channel.status.includes("valid host or IP address"))
        verify(session.available)
        verify(channel.requests.length === 0)
        field.text = "192.168.1.20"
        if (imageDirectory.length > 0) {
            waitForRendering(view.contentItem)
            fixtures.saveWindow(view, imageDirectory + "/channels-direct.png")
        }
        findChild(view, "discoverDialog").close()
        tryCompare(field, "visible", false)
        verify(session.available)
        view.close()
    }
}
