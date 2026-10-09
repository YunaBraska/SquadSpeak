import QtQuick
import "AvatarAtlas.js" as Atlas

Item {
    id: root
    property string name: ""
    property string avatar: "mossling"
    property real level: 0
    property bool available: true
    property bool online: true
    property bool muted: false
    property bool deafened: false
    property bool sleeping: false
    property bool animated: true
    property bool circular: false
    property bool music: false
    property bool systemMessage: false
    property string identity: name
    property double animationTime: 0
    readonly property int animationSeed: {
        let hash = 0
        for (let i = 0; i < identity.length; ++i) hash = (hash * 31 + identity.charCodeAt(i)) >>> 0
        return hash % 100000
    }
    property var atlasLayout: Atlas.forAvatar(displayAvatar, systemMessage)
    readonly property var clipRow: atlasLayout.rows[stateRow]
    readonly property real activeDuration: clipRow.duration * clipRow.repeats
    readonly property real cycleLength: activeDuration + clipRow.pause + (clipRow.pause > 0 ? animationSeed % (systemMessage ? 6000 : 1500) : 0)
    readonly property real phase: (animationTime + animationSeed) % cycleLength
    readonly property real cycle: Math.floor((animationTime + animationSeed) / cycleLength)
    readonly property real activity: online && available && !muted && isFinite(level) && level > 0.001
        ? Math.min(1, (20 * Math.log(level) / Math.LN10 + 60) / 40) : 0
    readonly property int stateRow: !online ? 4 : activity > 0 ? 1 : sleeping ? 4 : deafened ? 3 : muted ? 2 : 0
    readonly property int column: {
        if (!online || !animated) return 0
        if (clipRow.pause > 0) return Atlas.idleColumn(clipRow, cycle, phase, animationSeed)
        const clip = (cycle + animationSeed + Math.floor(cycle / 3)) % clipRow.frames.length
        return clipRow.frames.slice(0, clip).reduce(function(sum, count) { return sum + count }, 0)
            + Math.floor((phase % clipRow.duration) * clipRow.frames[clip] / clipRow.duration)
    }
    readonly property int variant: {
        let remaining = column
        for (let clip = 0; clip < clipRow.frames.length; ++clip) {
            if (remaining < clipRow.frames[clip]) return clip
            remaining -= clipRow.frames[clip]
        }
        return 0
    }
    readonly property int frame: column - clipRow.frames.slice(0, variant).reduce(function(sum, count) { return sum + count }, 0)
    readonly property string displayAvatar: session.avatars.includes(avatar) ? avatar : session.avatarFallback(avatar)
    readonly property url atlasSource: Qt.resolvedUrl("avatars/" + (systemMessage ? (animated ? "system-animated" : "system") : displayAvatar) + ".png")
    readonly property string stateLabel: [qsTr("Listening"), qsTr("Speaking"), qsTr("Muted"), qsTr("Deafened"), qsTr("Sleeping")][stateRow]
    implicitWidth: 48
    implicitHeight: 48
    Accessible.role: Accessible.Graphic
    Accessible.name: name + ", " + (!online ? qsTr("Offline") : available ? stateLabel : qsTr("Unavailable"))
    onOnlineChanged: circle.requestPaint()
    onCircularChanged: circle.requestPaint()
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        width: root.circular ? Math.min(root.width, root.height) * 0.82 : root.width
        height: root.circular ? width : root.height
        radius: root.circular ? width / 2 : Theme.panelRadius
        color: !root.online ? (Theme.dark ? "#333333" : "#ececec") : Theme.raised
        border.color: root.activity > 0 ? Theme.accent : "transparent"
        border.width: 2
    }
    Glyph { anchors.centerIn: parent; width: parent.width * 0.55; height: width; symbol: "music"; color: root.activity > 0 ? Theme.accent : Theme.muted; visible: root.music }
    Canvas {
        id: circle
        objectName: "avatarCanvas"
        anchors.fill: parent
        anchors.margins: 3
        smooth: true
        visible: !root.music
        opacity: root.available ? 1 : 0.5
        property url atlas: root.atlasSource
        readonly property rect crop: Atlas.frameRect(root.atlasLayout, root.clipRow, root.column)
        property url loadedAtlas: ""
        function syncImage() {
            const next = visible ? atlas : ""
            if (loadedAtlas.toString() !== next.toString()) {
                if (loadedAtlas.toString().length > 0) unloadImage(loadedAtlas)
                loadedAtlas = next
                if (loadedAtlas.toString().length > 0) loadImage(loadedAtlas)
            }
            requestPaint()
        }
        onAtlasChanged: syncImage()
        onVisibleChanged: syncImage()
        onCropChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        property real pixelRatio: Screen.devicePixelRatio
        onPixelRatioChanged: requestPaint()
        onImageLoaded: requestPaint()
        Component.onCompleted: syncImage()
        onPaint: {
            const c = getContext("2d")
            c.reset()
            if (!isImageLoaded(atlas)) return
            if (root.circular) {
                c.shadowColor = "rgba(0, 0, 0, 0.24)"
                c.shadowBlur = 2
                c.shadowOffsetY = 1.5
            }
            const still = root.systemMessage && !root.animated
            const aspect = still ? 1 : crop.width / crop.height
            const w = Math.min(width, height * aspect), h = w / aspect
            const x = (width - w) / 2, y = (height - h) / 2
            if (still) c.drawImage(atlas, x, y, w, h)
            else c.drawImage(atlas, crop.x, crop.y, crop.width, crop.height, x, y, w, h)
            if (!root.online) {
                // Canvas reads a physical-pixel rectangle, even on scaled displays.
                const pixels = c.getImageData(0, 0, Math.round(width * pixelRatio), Math.round(height * pixelRatio))
                const data = pixels.data
                for (let i = 0; i < data.length; i += 4) {
                    const gray = Math.round(0.2126 * data[i] + 0.7152 * data[i + 1] + 0.0722 * data[i + 2])
                    data[i] = gray; data[i + 1] = gray; data[i + 2] = gray
                }
                c.clearRect(0, 0, width, height)
                // Draw the complete pixel buffer back at its logical size.
                c.scale(1 / pixelRatio, 1 / pixelRatio)
                c.putImageData(pixels, 0, 0, 0, 0, pixels.width, pixels.height)
            }
        }
    }
    Row {
        anchors.bottom: parent.bottom
        width: parent.width
        spacing: Math.max(0, width - microphoneBadge.width - speakerBadge.width)
        Rectangle {
            id: microphoneBadge
            objectName: "microphoneBadge"
            visible: root.muted
            width: Math.max(14, Math.min(20, root.width * 0.4)); height: width
            radius: width / 2
            color: Theme.surface
            Glyph { anchors.fill: parent; anchors.margins: 2; symbol: "mute"; color: Theme.text; description: qsTr("Muted") }
        }
        Rectangle {
            id: speakerBadge
            objectName: "speakerBadge"
            visible: root.deafened
            width: Math.max(14, Math.min(20, root.width * 0.4)); height: width
            radius: width / 2
            color: Theme.surface
            Glyph { anchors.fill: parent; anchors.margins: 2; symbol: "deafen"; color: Theme.text; description: qsTr("Deafened") }
        }
    }
}
