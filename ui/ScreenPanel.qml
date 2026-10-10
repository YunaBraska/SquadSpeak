import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtMultimedia

Item {
    id: panel
    objectName: "screenPreview"
    required property var share
    required property var network
    property string hostId: ""
    readonly property var info: { const revision = network.screenView; return network.screenInfo(hostId) }
    property string attachedHost: ""
    implicitHeight: 112
    function attachPreview() {
        if (!share) return
        if (attachedHost.length && (!visible || !info.available || attachedHost !== hostId)) {
            attachedHost = ""
            share.detach(preview.videoSink)
        }
        if (visible && info.available && attachedHost !== hostId) {
            attachedHost = hostId
            if (!share.attach(hostId, preview.videoSink, true)) attachedHost = ""
        }
    }
    onHostIdChanged: Qt.callLater(attachPreview)
    onVisibleChanged: Qt.callLater(attachPreview)
    onInfoChanged: Qt.callLater(attachPreview)
    Component.onCompleted: Qt.callLater(attachPreview)
    Component.onDestruction: if (share) share.detach(preview.videoSink)
    function quality(details) {
        if (details.status === "full") return qsTr("Media capacity reached")
        if (details.tier === 4 || (details.available && !details.watching)) return qsTr("Video paused")
        if (!details.available) return qsTr("Stream ended")
        return details.width > 0 ? details.width + " x " + details.height : qsTr("Connecting")
    }
    Rectangle { anchors.fill: parent; color: Theme.background; radius: Theme.controlRadius; border.color: Theme.border }
    VideoOutput { id: preview; objectName: "screenThumbnail"; anchors.fill: parent; anchors.margins: 4; fillMode: VideoOutput.PreserveAspectFit }
    Label {
        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6
        text: panel.quality(panel.info); color: Theme.text; padding: 4
        background: Rectangle { color: Theme.surface; radius: Theme.smallRadius }
    }
    function openViewer() {
        if (viewer.visible && viewer.hostId !== panel.hostId) viewer.close()
        viewer.hostId = panel.hostId
        viewer.show(); viewer.raise(); viewer.requestActivate()
    }
    MouseArea { anchors.fill: parent; onClicked: panel.openViewer() }
    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: qsTr("Open screen stream")
    Accessible.onPressAction: panel.openViewer()
    Keys.onReturnPressed: panel.openViewer()
    Keys.onSpacePressed: panel.openViewer()
    PanelWindow {
        id: viewer
        objectName: "screenViewer"
        property string hostId: ""
        readonly property var info: { const revision = panel.network.screenView; return panel.network.screenInfo(hostId) }
        title: qsTr("Screen stream")
        width: 960; height: 600; minimumWidth: 320; minimumHeight: 240
        onVisibleChanged: {
            if (!panel.share) return
            if (visible) panel.share.attach(hostId, fullSize.videoSink)
            else panel.share.detach(fullSize.videoSink)
        }
        onHostIdChanged: if (visible && panel.share) panel.share.attach(hostId, fullSize.videoSink)
        VideoOutput { id: fullSize; objectName: "screenFullSize"; anchors.fill: parent; fillMode: VideoOutput.PreserveAspectFit }
        footer: Label { text: panel.quality(viewer.info); color: Theme.muted; padding: 8 }
    }
}
