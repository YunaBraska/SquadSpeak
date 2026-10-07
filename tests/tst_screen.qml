import QtQuick
import QtTest
import "../ui"

TestCase {
    name: "ScreenRendering"
    when: windowShown
    readonly property var view: scene.item
    Loader { id: scene; sourceComponent: Channels { visible: true } }
    SignalSpy { id: screenChanges; target: channel; signalName: "screenChanged" }
    function initTestCase() {
        session.setMuted(true)
        session.setAnimatedAvatars(false)
    }
    function cleanupTestCase() {
        const viewer = findChild(view, "screenViewer")
        if (viewer) viewer.close()
        channel.leave()
        remoteChannel.leave()
        channel.stopHost()
        remoteChannel.stopHost()
        view.close()
        scene.active = false
        wait(0)
    }

    function screenImageVisible(output) {
        const area = output.contentRect
        if (area.width <= 0 || area.height <= 0) return false
        for (const sample of [[0.25, 0.4, 82, 157, 147], [0.68, 0.6, 212, 164, 90]]) {
            const color = fixtures.renderedColor(output, area.x + area.width * sample[0], area.y + area.height * sample[1])
            if (Math.abs(color.r * 255 - sample[2]) > 25 || Math.abs(color.g * 255 - sample[3]) > 25
                || Math.abs(color.b * 255 - sample[4]) > 25) return false
        }
        return true
    }
    function test_screenPreviewAndSeparateResizableViewer() {
        verify(fixtures.startHost())
        verify(fixtures.startRemoteHost())
        verify(remoteChannel.decide(channel.ownId, true))
        verify(channel.openChat(remoteChannel.ownId, "127.0.0.1", remoteChannel.servicePort))
        tryCompare(channel, "chatReady", true)
        verify(view.openHostChat(remoteChannel.ownId))
        verify(fixtures.shareTestImage())
        tryVerify(function() { return findChild(view, "screenPreview") !== null })
        const preview = findChild(view, "screenPreview")
        tryCompare(preview, "visible", true)
        tryVerify(function() { return channel.screenView.tier === 3 })
        tryVerify(function() { verify(fixtures.shareTestImage()); return channel.screenView.width > 0 }, 5000)
        compare(channel.screenView.width, 640)
        tryVerify(function() { return screenImageVisible(findChild(preview, "screenThumbnail")) }, 5000,
            "Decoded video pixels are visible in the thumbnail")
        mouseClick(preview)
        const viewer = findChild(preview, "screenViewer")
        tryCompare(viewer, "visible", true)
        viewer.width = 730; viewer.height = 420
        waitForRendering(viewer.contentItem)
        compare(viewer.width, 730)
        tryVerify(function() { return screenImageVisible(findChild(viewer, "screenFullSize")) }, 5000,
            "Decoded video pixels are visible in the resized viewer")
        if (imageDirectory.length > 0) {
            verify(fixtures.saveWindow(view, imageDirectory + "/screen-thumbnail.png"))
            verify(fixtures.saveWindow(viewer, imageDirectory + "/screen-viewer.png"))
        }
        verify(channel.joinSaved(channel.ownId))
        tryCompare(channel, "joined", true)
        compare(channel.joinedHostId, channel.ownId)
        tryCompare(preview, "visible", false)
        verify(viewer.visible)
        compare(viewer.hostId, remoteChannel.ownId)
        screenChanges.clear()
        verify(view.openHostChat(remoteChannel.ownId))
        verify(screenChanges.count > 0, "Changing chat notifies the preview before another video frame arrives")
        tryCompare(preview, "visible", true)
        compare(channel.joinedHostId, channel.ownId)
        viewer.close()
        tryCompare(channel.screenView, "tier", 3)
        verify(channel.chatReady)
        verify(fixtures.shareTestImage(false))
        tryCompare(preview, "visible", false)
        verify(channel.chatReady)
    }
}
