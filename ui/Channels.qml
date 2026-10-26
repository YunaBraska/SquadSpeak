import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtMultimedia
import "AvatarAtlas.js" as Atlas

PanelWindow {
    id: root
    property var language: audio.language
    onHeightChanged: if (conversationSlot) Qt.callLater(conversationSlot.selectSlot)
    component DetailText: TextArea {
        readOnly: true; selectByMouse: true
        textFormat: TextEdit.PlainText; wrapMode: Text.WrapAnywhere
        Layout.fillWidth: true; padding: 0
        color: Theme.text; selectionColor: Theme.accent; selectedTextColor: Theme.accentText
        background: null
        ToolTip.visible: hovered && Accessible.name.length > 0
        ToolTip.text: Accessible.name
        ToolTip.delay: 600
    }
    component UnreadDot: Rectangle {
        implicitWidth: 7; implicitHeight: 7
        radius: width / 2; color: Theme.accent
        Accessible.role: Accessible.StaticText
        Accessible.name: qsTr("Unread messages")
    }
    component MenuAction: MenuItem {
        id: action
        property bool destructive: false
        leftPadding: 12
        rightPadding: checkable ? 36 : 12
        height: visible ? implicitHeight : 0
        contentItem: Label {
            text: action.text
            font: action.font
            color: action.destructive ? Theme.danger : Theme.text
            opacity: action.enabled ? 1 : 0.45
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle { implicitWidth: 240; implicitHeight: 36; radius: Theme.smallRadius; color: action.highlighted ? Theme.raised : "transparent" }
        indicator: Rectangle {
            x: action.width - width - 12
            y: (action.height - height) / 2
            implicitWidth: 16; implicitHeight: 16
            visible: action.checkable
            color: "transparent"
            border.color: action.checked ? Theme.accent : Theme.muted
            radius: Theme.smallRadius
            Glyph { anchors.centerIn: parent; width: 12; height: 12; symbol: "check"; color: Theme.accent; visible: action.checked }
        }
    }
    component MemberGrid: GridView {
        id: grid
        property string hostId: ""
        clip: true
        cellWidth: width < 350 ? width : width / 2
        cellHeight: 54
        cacheBuffer: 0
        ScrollBar.vertical: ScrollBar { policy: grid.contentHeight > grid.height ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded }
        delegate: Item {
            id: memberEntry
            required property var modelData
            width: grid.cellWidth; height: grid.cellHeight
            objectName: "member_" + modelData.id
            activeFocusOnTab: true
            Accessible.role: Accessible.Button
            Accessible.name: modelData.name
            Accessible.onPressAction: root.memberOptions(modelData, grid.hostId)
            Keys.onReturnPressed: root.memberOptions(modelData, grid.hostId)
            Keys.onSpacePressed: root.memberOptions(modelData, grid.hostId)
            Rectangle { anchors.fill: parent; visible: parent.activeFocus; color: "transparent"; radius: Theme.controlRadius; border.color: Theme.accent }
            RowLayout {
                anchors.fill: parent; anchors.rightMargin: 10; spacing: 5
                VoiceAvatar {
                    objectName: "participantAvatar"
                    Layout.preferredWidth: 44; Layout.preferredHeight: 44; circular: true
                    avatar: modelData.avatarId || modelData.avatar || "mossling"; name: modelData.name; music: !!modelData.music; teamSpeak: !!modelData.teamSpeak
                    available: modelData.available; muted: modelData.muted; deafened: !!modelData.deafened; sleeping: !!modelData.sleeping
                    identity: modelData.id; animationTime: root.avatarTime
                    animated: root.voice.animatedAvatars && memberEntry.y + memberEntry.height >= grid.contentY && memberEntry.y <= grid.contentY + grid.height
                    level: grid.hostId === root.activeHost ? root.memberLevel(modelData.id) : 0
                }
                Label { text: modelData.name; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 12 }
            }
            MouseArea { anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: mouse => root.memberOptions(modelData, grid.hostId, mouse.button === Qt.RightButton) }
            ActionButton {
                anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
                text: qsTranslate("LocalLanguage", "Original text"); glyph: "chat"; iconOnly: true; flat: true
                visible: { root.language.revision; const info = root.language.member(modelData.id); return !!info.transcript || !!info.status }
                onClicked: { root.selectedMember = modelData; originalTranscript.open() }
            }
            TextToolTip { parent: memberEntry; text: modelData.name; visible: memberHover.hovered }
            HoverHandler { id: memberHover }
        }
    }
    PanelDialog {
        id: memberList
        objectName: "membersDialog"
        property string hostId: ""
        readonly property var entry: root.channelList.find(function(c) { return c.id === hostId }) || ({})
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(520, parent.width - 24); height: Math.min(580, parent.height - 24)
        title: (entry.name || "") + " (" + (entry.members || []).length + ")"
        modal: true; focus: true; buttons: Dialog.Close
        onOpened: allMembers.positionViewAtBeginning()
        contentItem: MemberGrid {
            id: allMembers
            objectName: "allMembers"
            hostId: memberList.hostId
            model: memberList.visible ? memberList.entry.members || [] : []
        }
    }
    property var voice: session
    property var supporter: typeof supporterLicense !== "undefined" ? supporterLicense : null
    property var updates: typeof appUpdates !== "undefined" ? appUpdates : null
    component AvatarShelf: PathView {
        id: avatarShelf
        property bool preview: false
        Layout.fillWidth: true; Layout.preferredHeight: 120
        clip: true; dragMargin: height
        model: root.voice.supporterEnabled ? root.voice.avatars : root.voice.avatars.slice(0, 10)
        currentIndex: preview ? 0 : Math.max(0, model.indexOf(root.voice.avatar))
        pathItemCount: Math.ceil(width / 100) + 1
        cacheItemCount: 2
        preferredHighlightBegin: 0.5; preferredHighlightEnd: 0.5
        snapMode: PathView.SnapToItem; highlightMoveDuration: 120
        activeFocusOnTab: true
        Keys.onLeftPressed: decrementCurrentIndex()
        Keys.onRightPressed: incrementCurrentIndex()
        Keys.onSpacePressed: if (!preview) root.voice.setAvatar(model[currentIndex])
        Keys.onReturnPressed: if (!preview) root.voice.setAvatar(model[currentIndex])
        path: Path {
            startX: avatarShelf.width / 2 - avatarShelf.pathItemCount * 50; startY: 55
            PathLine { x: avatarShelf.width / 2 + avatarShelf.pathItemCount * 50; y: 55 }
        }
        MouseArea {
            anchors.fill: parent; acceptedButtons: Qt.NoButton
            property real pending: 0
            onWheel: event => {
                const pixels = event.pixelDelta.x || event.pixelDelta.y
                pending += pixels ? pixels / 100 : (event.angleDelta.x || event.angleDelta.y) / 120
                const steps = Math.trunc(pending)
                if (steps) avatarShelf.currentIndex = (avatarShelf.currentIndex - steps % avatarShelf.count + avatarShelf.count) % avatarShelf.count
                pending -= steps
                event.accepted = true
            }
        }
        delegate: Item {
            required property string modelData
            required property int index
            objectName: (avatarShelf.preview ? "avatarPreview_" : "avatarChoice_") + modelData
            width: 96; height: 110
            readonly property string label: modelData.split("-").map(function(word) { return word.charAt(0).toUpperCase() + word.slice(1) }).join(" ")
            VoiceAvatar { anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; width: 88; height: 88; avatar: modelData; muted: false; animated: false; online: true; Accessible.ignored: true }
            Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 88; height: 88; color: "transparent"; radius: Theme.panelRadius; border.color: (!avatarShelf.preview && root.voice.avatar === modelData) || (avatarShelf.activeFocus && index === avatarShelf.currentIndex) ? Theme.accent : "transparent"; border.width: 2 }
            Label { anchors.bottom: parent.bottom; width: parent.width; text: parent.label; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight }
            TapHandler { enabled: !avatarShelf.preview; onTapped: root.voice.setAvatar(modelData) }
            Accessible.role: avatarShelf.preview ? Accessible.Graphic : Accessible.RadioButton
            Accessible.name: label
            Accessible.description: !avatarShelf.preview ? "" : qsTranslate("VoiceSession", "This avatar requires Supporter.")
            Accessible.checked: !avatarShelf.preview && root.voice.avatar === modelData
            Accessible.onPressAction: if (!avatarShelf.preview) root.voice.setAvatar(modelData)
            ToolTip.visible: portraitHover.hovered
            ToolTip.text: label
            HoverHandler { id: portraitHover }
        }
    }
    property var screen: typeof screenShare !== "undefined" ? screenShare : null
    property var network: channel
    readonly property bool remote: network.remoteMode
    readonly property var current: remote ? network.remoteView : network
    readonly property var channelList: remote ? (current.channels || []) : network.savedChannels
    readonly property var privateChatList: remote ? (current.privateChats || []) : (network.privateChats || [])
    readonly property var members: current.participants || []
    readonly property var chatMembers: current.chatMembers || []
    readonly property string inspectedHost: current.chatHostId || ""
    property bool chatExpanded: true
    property bool touchControls: Qt.platform.os === "ios" || Qt.platform.os === "android"
    PointHandler { parent: root.contentItem; acceptedDevices: PointerDevice.TouchScreen; onActiveChanged: if (active) root.touchControls = true }
    readonly property bool mobileKeyboard: (Qt.platform.os === "android" || Qt.platform.os === "ios") && Qt.inputMethod.visible
    readonly property bool compactInput: mobileKeyboard && height < 360
    property Item conversationSlot: null
    property double avatarTime: Date.now()
    Timer { interval: root.voice.animatedAvatars ? Atlas.frameInterval() : 80; running: root.visible; repeat: true; onTriggered: root.avatarTime = Date.now() }
    readonly property bool showChat: chatExpanded && inspectedHost.length > 0
    onInspectedHostChanged: {
        chatExpanded = true
        Qt.callLater(root.updateChannelRows)
    }
    readonly property string activeHost: current.joinedHostId || ""
    readonly property bool micMuted: remote ? !!current.muted : voice.muted
    readonly property bool outputMuted: remote ? !!current.deafened : voice.deafened
    property int settingsPage: 0
    property int aboutIndex: -1
    readonly property var aboutLines: [qsTr("Small app. Large conversations."),
        qsTr("The mute button has saved friendships."), qsTr("Good company. Fewer decibels."),
        qsTr("Less noise. More conversation.")]
    function pickAboutSentence() {
        aboutIndex = aboutIndex < 0 ? Math.floor(Math.random() * aboutLines.length)
            : (aboutIndex + 1 + Math.floor(Math.random() * (aboutLines.length - 1))) % aboutLines.length
    }
    property string selectedHost: ""
    property bool searchOpen: false
    onSearchOpenChanged: {
        if (searchOpen) Qt.callLater(channelSearch.forceActiveFocus)
        else channelSearch.text = ""
    }
    Shortcut { sequences: [StandardKey.Find]; enabled: root.active && !settings.visible; onActivated: root.searchOpen = true }
    property var privateFolderExpanded: ({})
    property var collapsedHosts: ({})
    readonly property var selectedOwner: { const revision = network.ownedChannels; return remote ? null : network.ownChannel(selectedHost) }
    readonly property var managedHost: selectedOwner || network
    readonly property var memberHost: { const revision = network.ownedChannels; return remote ? null : network.ownChannel(selectedMemberHost) }
    readonly property bool canModerateMember: !remote && !selectedMember.music && selectedMember.id !== network.ownId
        && (memberHost ? memberHost.hostClients : []).some(function(p) { return p.id === root.selectedMember.id })
    readonly property bool canGrantMemberControl: !remote && !selectedMember.music && selectedMember.id !== network.ownId
        && (canModerateMember || network.controllers.some(function(p) { return p.id === root.selectedMember.id }))
    readonly property bool canStopMemberRadio: !remote && !!selectedMember.music && !!memberHost
    readonly property var hostRadio: { const revision = network.ownedChannels; return selectedOwner ? radio.forChannel(selectedHost) : radio }
    property string inspectedRequestHost: ""
    function decideRequest(id, hostId, allow) {
        const owner = network.ownChannel(hostId)
        return owner && owner.decide(id, allow)
    }

    property string inspectedRequestId: ""
    readonly property var incomingRequest: root.network.requests.length > 0 ? root.network.requests[0] : ({})
    property string stationId: ""
    readonly property string radioStateText: !root.hostRadio ? "" : root.hostRadio.state === "playing" ? qsTr("Playing: %1").arg(root.hostRadio.stationName)
        : root.hostRadio.state === "reconnecting" ? qsTr("Reconnecting: %1").arg(root.hostRadio.stationName)
        : root.hostRadio.state === "unavailable" ? qsTr("Waiting for your channel") : qsTr("Connecting: %1").arg(root.hostRadio.stationName)
    property string lastNotice: ""
    property string notice: ""
    property var selectedMember: ({})
    property string selectedMemberHost: ""
    function endpoint(entry) {
        const address = entry.address || ""
        return entry.port ? (address.includes(":") ? "[" + address + "]" : address) + ":" + entry.port : address
    }
    function privateHostId(entry) {
        const deviceId = entry.deviceId
        const channel = channelList.find(function(item) { return !item.private && (item.deviceId || item.id) === deviceId })
        return channel ? channel.id : deviceId
    }
    function privateChatsForHost(hostId) {
        return privateChatList.filter(function(entry) { return privateHostId(entry) === hostId || entry.deviceId === hostId })
    }
    function privateDeviceForHost(hostId) {
        const host = channelList.find(function(entry) { return entry.id === hostId || entry.deviceId === hostId })
        return host ? (host.deviceId || host.id) : hostId
    }
    function toggleHost(id) {
        const next = Object.assign({}, collapsedHosts)
        if (next[id]) delete next[id]
        else next[id] = true
        collapsedHosts = next
        updateChannelRows()
    }
    function privateFolderIsExpanded(id) { return privateFolderExpanded[id] !== false }
    function togglePrivateFolder(id) {
        const next = Object.assign({}, privateFolderExpanded)
        next[id] = !privateFolderIsExpanded(id)
        privateFolderExpanded = next
        updateChannelRows()
    }
    function openPrivate(entry) {
        const hostId = privateHostId(entry)
        const opened = root.network.openPrivateChat(hostId, entry.peerId)
        if (opened) chatExpanded = true
        return opened
    }
    function openPrivateForMember() {
        if (!selectedMember.id || !network.canPrivateChat(selectedMemberHost, selectedMember.id)) {
            showNotice(qsTr("Private messages are unavailable for this contact."))
            return false
        }
        requestInfo.close()
        return network.openPrivateChat(selectedMemberHost, selectedMember.id)
    }
    ListModel { id: channelRows; dynamicRoles: true }
    function orderedChannels(entries) {
        if (!entries.some(function(c) { return c.protocol === "teamspeak" }))
            return entries.slice().sort(function(a, b) { return a.name.localeCompare(b.name) || a.id.localeCompare(b.id) })
        const result = [], seen = new Set(), ids = new Set(entries.map(function(c) { return c.id }))
        function appendSiblings(parent, depth) {
            const siblings = entries.filter(function(c) { return parent ? c.parent === parent : !ids.has(c.parent) })
            let previous = "0"
            while (siblings.length) {
                let index = siblings.findIndex(function(c) { return c.order === previous })
                if (index < 0) index = 0
                const entry = siblings.splice(index, 1)[0]
                if (seen.has(entry.id)) continue
                seen.add(entry.id)
                result.push(Object.assign({}, entry, {depth: Math.min(depth, 8)}))
                previous = entry.tsChannel
                if (depth < 64) appendSiblings(entry.id, depth + 1)
            }
        }
        appendSiblings("", 0)
        for (const entry of entries) if (!seen.has(entry.id)) result.push(entry)
        return result
    }
    function updateChannelRows() {
        const groups = new Map()
        const query = channelSearch.text.trim().toLowerCase()
        const livePrivateChats = []
        for (const entry of channelList) {
            if (entry.private) {
                if (entry.hasMessages || entry.id === inspectedHost) livePrivateChats.push(entry)
                continue
            }
            const key = entry.deviceId || entry.id
            const named = !!entry.ownerName
            const label = entry.ownerName || (entry.owned ? qsTr("This device") : endpoint(entry))
            if (!groups.has(key)) groups.set(key, {key: key, label: label, protocol: entry.protocol || "squadspeak", named: named, owned: false, channels: [], privateChats: []})
            const group = groups.get(key)
            if ((named && !group.named) || (named === group.named && label.localeCompare(group.label) < 0)) {
                group.label = label
                group.named = named
            }
            group.owned = group.owned || !!entry.owned
            group.channels.push(entry)
        }
        const activePrivateId = inspectedHost
        if (current.privateChat && activePrivateId) {
            const active = privateChatList.find(function(entry) { return entry.id === activePrivateId })
            if (active && !livePrivateChats.some(function(entry) { return entry.id === activePrivateId }))
                livePrivateChats.push(active)
        }
        for (const entry of livePrivateChats) {
            const key = entry.deviceId
            if (!key) continue
            const label = entry.ownerName || entry.name || key
            if (!groups.has(key)) groups.set(key, {key: key, label: label, protocol: entry.protocol || "squadspeak", named: !!entry.ownerName, channels: [], privateChats: []})
            const group = groups.get(key)
            if (entry.ownerName && (!group.named || label.localeCompare(group.label) < 0)) {
                group.label = label; group.named = true
            }
            group.privateChats.push(entry)
        }
        const retained = {}
        for (const key of Object.keys(collapsedHosts)) if (groups.has(key)) retained[key] = true
        collapsedHosts = retained
        const ordered = []
        const sortedGroups = Array.from(groups.values()).sort(function(a, b) {
            return Number(!!b.owned) - Number(!!a.owned) || a.label.localeCompare(b.label) || a.key.localeCompare(b.key)
        })
        for (const group of sortedGroups) {
            const privateUnread = group.privateChats.some(function(entry) { return !!entry.unread })
            const groupUnread = privateUnread || group.channels.some(function(entry) { return !!entry.unread })
            const groupExpanded = !!query || !collapsedHosts[group.key]
            if (query && !group.label.toLowerCase().includes(query)) {
                const matches = new Set()
                for (const entry of group.channels) {
                    if (!entry.name.toLowerCase().includes(query) && !endpoint(entry).toLowerCase().includes(query)) continue
                    let ancestor = entry
                    while (ancestor && !matches.has(ancestor.id)) {
                        matches.add(ancestor.id)
                        ancestor = group.channels.find(function(item) { return item.id === ancestor.parent })
                    }
                }
                group.channels = group.channels.filter(function(entry) { return matches.has(entry.id) })
                group.privateChats = group.privateChats.filter(function(entry) { return (entry.name || "").toLowerCase().includes(query) })
            }
            const entries = orderedChannels(group.channels)
            group.privateChats.sort(function(a, b) {
                return (a.name || "").localeCompare(b.name || "") || a.id.localeCompare(b.id)
            })
            if (group.privateChats.length > 0) {
                entries.push({id: "private-folder:" + group.key, name: qsTr("Messages"), privateFolder: true, hostId: group.key, unread: privateUnread})
                if (query || privateFolderIsExpanded(group.key)) for (const entry of group.privateChats)
                    entries.push(Object.assign({}, entry, {id: entry.id, private: true, hostId: privateHostId(entry)}))
            }
            // Retain the first row's heading while releasing collapsed contents.
            for (const [index, entry] of (groupExpanded ? entries : entries.slice(0, 1)).entries())
                ordered.push({channelData: entry, groupLabel: index === 0 ? group.label : "", groupId: group.key,
                    groupProtocol: group.protocol, groupUnread: groupUnread, groupExpanded: groupExpanded})
        }
        for (let i = channelRows.count - 1; i >= 0; --i) {
            const row = channelRows.get(i).channelData
            if (!ordered.some(function(entry) { return entry.channelData.id === row.id })) channelRows.remove(i)
        }
        for (let i = 0; i < ordered.length; ++i) {
            let found = -1
            for (let j = i; j < channelRows.count; ++j) if (channelRows.get(j).channelData.id === ordered[i].channelData.id) { found = j; break }
            if (found < 0) channelRows.insert(i, ordered[i])
            else {
                if (found !== i) channelRows.move(found, i, 1)
                channelRows.set(i, ordered[i])
            }
        }
    }
    onChannelListChanged: updateChannelRows()
    Connections { target: root.network; function onHostsChanged() { root.updateChannelRows() } }
    Component.onCompleted: updateChannelRows()
    Binding { target: Theme; property: "mode"; value: session.theme }
    Binding { target: Theme; property: "paletteName"; value: session.palette }
    signal previewRequested(bool enabled)
    readonly property bool audioSettingsOpen: visible && settings.visible && settingsPage >= 1 && settingsPage <= 2
    function syncAudioSettings() {
        const active = audioSettingsOpen
        const preview = active && settingsPage === 1
        if (!preview) audio.stop()
        previewRequested(preview)
    }
    onAudioSettingsOpenChanged: syncAudioSettings()
    onSettingsPageChanged: {
        if (settings.visible) audio.stop()
        if (settings.visible && settingsPage === 3) pickAboutSentence()
        syncAudioSettings()
    }
    onClosing: { settings.close(); searchOpen = false }
    width: 460
    height: 560
    minimumWidth: 360
    minimumHeight: 360
    visible: false
    title: "SquadSpeak"
    function joinHost(id) {
        if (!network.ready) return false
        if (id === activeHost) return openHostChat(id)
        const entry = channelList.find(function(host) { return host.id === id })
        if (!entry) {
            const privateEntry = privateChatList.find(function(chat) { return chat.id === id })
            if (privateEntry) return openPrivate(privateEntry)
        }
        if (entry && entry.private) return openPrivate(entry)
        if (entry && ["pending", "password", "connecting"].includes(entry.access)) return openHostChat(id)
        return remote ? network.remoteAction("join", {hostId: id}) : network.joinSaved(id)
    }
    function leaveHost() { return remote ? network.remoteAction("leave", {}) : network.leave() }
    function openSettings(page) {
        if (root.remote) return
        settingsPage = page
        settings.open()
    }
    function closeSettings() { settings.close() }
    function controlDevice(id) {
        if (!network.chooseRemoteOffer(id)) {
            if (remoteTargets.visible) remoteTargets.error = network.controlStatus
            else showNotice(network.controlStatus)
            return false
        }
        remoteTargets.close(); settings.close()
        return true
    }
    function showNotice(text) {
        if (text === lastNotice) return
        lastNotice = text; notice = text; noticeTimer.restart()
    }
    Timer { id: noticeTimer; interval: 4000; onTriggered: root.notice = "" }
    function openHostChat(id) {
        if (!network.ready) return false
        const privateEntry = privateChatList.find(function(chat) { return chat.id === id })
        if (privateEntry) return openPrivate(privateEntry)
        chatExpanded = true
        return remote ? network.remoteAction("openChat", {hostId: id}) : network.openSavedChat(id)
    }
    function showHostChat(id) {
        if (id === inspectedHost && chatExpanded) chatExpanded = false
        else openHostChat(id)
    }
    function memberLevel(id) { return remote ? (network.remoteLevels[id] || 0) : id === network.ownId ? audio.transmitLevel : (audio.playbackLevels[id] || 0) }
    function toggleMembers(id) { memberList.hostId = id; memberList.open() }
    function memberOptions(member, host, contextual) {
        selectedMember = member; selectedMemberHost = host; inspectedRequestId = ""
        memberList.close()
        if (contextual) memberMenu.popup()
        else requestInfo.open()
    }
    function accessLabel(value) {
        switch (value) {
        case "pending": return qsTr("Pending")
        case "password": return qsTr("Password")
        case "rejected": return qsTr("Rejected")
        case "full": return qsTr("Full")
        case "kicked": return qsTr("Kicked")
        case "blocked": return qsTr("Blocked")
        case "connecting": return qsTr("Connecting")
        case "error": return qsTr("Unavailable")
        default: return ""
        }
    }

    PanelDialog {
        id: privateMessages
        objectName: "privateMessagesDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(420, parent.width - 24)
        height: Math.min(520, parent.height - 24)
        modal: true; focus: true; title: qsTr("Messages")
        property string hostId: ""
        property string hostDeviceId: ""
        readonly property var entries: root.privateChatsForHost(hostId)
        readonly property bool managesLifetime: !root.remote && hostDeviceId.length > 0 && hostDeviceId === (root.network.ownId || "")
        readonly property var lifetimeDays: session.supporterEnabled ? [1, 7, 30, 90, 180, 360] : [1, 7, 30]
        buttons: Dialog.Close
        contentItem: ScrollView {
            id: privateMessagesScroll
            clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: privateMessagesScroll.availableWidth
                spacing: 6
                Repeater {
                    model: privateMessages.entries
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true; Layout.minimumWidth: 0
                            text: modelData.name || modelData.peerId || ""
                            textFormat: Text.PlainText; elide: Text.ElideRight
                        }
                        Label {
                            text: modelData.blocked ? qsTr("Blocked") : modelData.peerOnline ? qsTr("Online") : qsTr("Offline")
                            color: modelData.blocked ? Theme.danger : Theme.muted; font.pixelSize: 11
                        }
                        ActionButton {
                            objectName: "openPrivateMessage_" + modelData.id
                            text: qsTr("Open"); flat: true
                            onClicked: { root.openPrivate(modelData); privateMessages.close() }
                        }
                        ActionButton {
                            objectName: "togglePrivateBlock_" + modelData.id
                            text: modelData.blockedByMe ? qsTr("Unblock") : qsTr("Block")
                            destructive: !modelData.blockedByMe
                            visible: modelData.protocol !== "teamspeak"
                            onClicked: root.network.setPrivateBlocked(modelData.id, !modelData.blockedByMe)
                        }
                    }
                }
                Label {
                    visible: privateMessages.entries.length === 0
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: qsTr("No private messages")
                    color: Theme.muted
                }
                RowLayout {
                    visible: privateMessages.managesLifetime
                    Layout.fillWidth: true
                    Label { text: qsTr("Keep new messages"); Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.WordWrap }
                    ChoiceBox {
                        objectName: "privateMessageLifetime"
                        Layout.preferredWidth: 128
                        model: session.supporterEnabled
                            ? [qsTr("24 hours"), qsTr("7 days"), qsTr("30 days"), qsTr("90 days"), qsTr("180 days"), qsTr("360 days")]
                            : [qsTr("24 hours"), qsTr("7 days"), qsTr("30 days")]
                        currentIndex: Math.max(0, privateMessages.lifetimeDays.indexOf(root.network.privateLifetimeDays || 1))
                        Accessible.name: qsTr("New message lifetime")
                        onActivated: root.network.setPrivateLifetimeDays(privateMessages.lifetimeDays[currentIndex])
                    }
                }
            }
        }
    }
    PanelDialog {
        id: languageDownload
        objectName: "languageDownloadDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(360, parent.width - 24)
        modal: true; title: qsTranslate("LocalLanguage", "Prepare languages")
        closePolicy: Popup.NoAutoClose
        property string previousError: ""
        function updatePrompt() {
            const error = root.language.error || ""
            if (error.length && error !== previousError) root.showNotice(error)
            previousError = error
            if (Object.keys(root.language.preparation).length && !root.remote && root.visible) {
                channelMenu.close(); open()
            } else close()
        }
        Connections { target: root.language; function onChanged() { languageDownload.updatePrompt() } }
        Connections { target: root; function onVisibleChanged() { languageDownload.updatePrompt() } }
        contentItem: ColumnLayout {
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: qsTr("Download language models for %1 and %2?")
                    .arg(Qt.locale(root.language.preparation.source || "en").nativeLanguageName)
                    .arg(Qt.locale(root.language.preparation.target || "en").nativeLanguageName)
            }
            RowLayout {
                Layout.fillWidth: true
                ActionButton { objectName: "cancelLanguageDownload"; text: qsTr("Cancel"); onClicked: root.language.cancelPreparation() }
                Item { Layout.fillWidth: true }
                ActionButton {
                    objectName: "confirmLanguageDownload"; text: qsTr("Download"); primary: true
                    onClicked: { languageDownload.close(); Qt.callLater(function() { root.language.acceptPreparation() }) }
                }
            }
        }
    }
    PanelDialog {
        id: joinPassword
        objectName: "joinPasswordDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(360, parent.width - 24)
        modal: true
        title: root.network.teamSpeakChat ? qsTr("Password") : qsTr("Channel password")
        closePolicy: Popup.NoAutoClose
        onOpened: { joinSecret.clear(); rememberSecret.checked = root.network.passwordSaved; joinSecret.forceActiveFocus() }
        onClosed: joinSecret.clear()
        contentItem: ColumnLayout {
            spacing: 10
            Label { text: root.network.status; textFormat: Text.PlainText; wrapMode: Text.Wrap; Layout.fillWidth: true }
            EntryField {
                id: joinSecret
                objectName: "joinPasswordValue"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                maximumLength: 1024
                Accessible.name: joinPassword.title
                onAccepted: if (passwordSubmit.enabled) passwordSubmit.clicked()
            }
            OptionCheck { id: rememberSecret; objectName: "rememberChannelPassword"; text: qsTr("Save password") }
            Label {
                visible: root.network.passwordRetrySeconds > 0
                text: qsTr("Try again in %1 s").arg(root.network.passwordRetrySeconds)
                Layout.fillWidth: true
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                ActionButton { text: qsTr("Cancel"); onClicked: { root.network.closeChat(root.inspectedHost); joinPassword.close() } }
                Item { Layout.fillWidth: true }
                ActionButton {
                    id: passwordSubmit
                    objectName: "submitChannelPassword"
                    text: qsTr("Join")
                    enabled: root.network.passwordRequired && root.network.passwordRetrySeconds === 0 && joinSecret.text.length > 0
                    onClicked: root.network.submitPassword(joinSecret.text, rememberSecret.checked)
                }
            }
        }
    }
    PanelDialog {
        id: hostPassword
        objectName: "hostPasswordDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(370, parent.width - 24)
        modal: true
        title: qsTr("Your channel password")
        buttons: Dialog.Cancel
        onOpened: { hostSecret.clear(); hostSecret.forceActiveFocus() }
        onClosed: hostSecret.clear()
        contentItem: ColumnLayout {
            spacing: 10
            Label {
                text: qsTr("A new password applies on the next join. Connected members stay.")
                Layout.fillWidth: true; wrapMode: Text.Wrap
            }
            EntryField {
                id: hostSecret
                objectName: "hostPasswordValue"
                Layout.fillWidth: true
                echoMode: TextInput.Password
                maximumLength: 1024
                Accessible.name: qsTr("New channel password")
                onAccepted: if (hostPasswordSave.enabled) hostPasswordSave.clicked()
            }
            RowLayout {
                Layout.fillWidth: true
                ActionButton {
                    objectName: "removeHostPassword"
                    text: qsTr("Remove")
                    destructive: true
                    visible: root.managedHost.passwordProtected
                    enabled: !root.managedHost.passwordBusy
                    onClicked: if (root.managedHost.setHostPassword("")) hostPassword.close()
                }
                Item { Layout.fillWidth: true }
                ActionButton {
                    id: hostPasswordSave
                    objectName: "saveHostPassword"
                    text: qsTr("Save")
                    enabled: !root.managedHost.passwordBusy && hostSecret.text.length > 0
                    onClicked: if (root.managedHost.setHostPassword(hostSecret.text)) hostPassword.close()
                }
            }
        }
    }
    Connections {
        target: root.network
        function onControlChanged() { if (root.remote) { requestInfo.close(); root.showNotice(root.network.controlStatus) } }
        function onRequestsChanged() {
            if (requestInfo.visible && root.inspectedRequestId && !root.network.requests.some(function(r) { return r.id === root.inspectedRequestId && (!root.inspectedRequestHost || r.hostId === root.inspectedRequestHost) })) requestInfo.close()
        }
        function onStateChanged() {
            if (!root.remote) root.showNotice(root.network.status)
            if (root.network.joined && discover.opened) discover.close()
            if (root.network.passwordRequired && !joinPassword.opened) { discover.close(); joinPassword.open() }
            else if (!root.network.passwordRequired) joinPassword.close()
        }
    }
    PanelDialog {
        id: discover
        objectName: "discoverDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(420, parent.width - 24)
        height: Math.min(480, parent.height - 24)
        modal: true
        title: qsTr("Add a channel")
        buttons: Dialog.Close
        onOpened: { root.network.setDiscoverySearch(true); discoveryList.refresh() }
        onClosed: { discoveryRefresh.stop(); root.network.setDiscoverySearch(false) }
        Timer { id: discoveryRefresh; interval: 50; onTriggered: discoveryList.refresh() }
        contentItem: ColumnLayout {
            spacing: 12
            RowLayout {
                Layout.fillWidth: true
                visible: (Qt.platform.os === "osx" || Qt.platform.os === "ios")
                Label {
                    objectName: "networkPermissionHint"
                    text: qsTr("If asked, allow local network access. You can change this in system settings.")
                    color: Theme.muted; font.pixelSize: 12
                    wrapMode: Text.Wrap; Layout.fillWidth: true
                }
                ActionButton {
                    objectName: "networkPermissionSettings"
                    text: qsTranslate("AudioSettings", "Open system settings")
                    glyph: "settings"; iconOnly: true; flat: true
                    onClicked: Qt.openUrlExternally(Qt.platform.os === "osx"
                        ? "x-apple.systempreferences:com.apple.preference.security?Privacy_LocalNetwork" : "app-settings:")
                }
            }
            Label {
                objectName: "discoveryStatus"
                visible: (root.network.discoverySearching || !!root.network.discoveryError || discoveryList.count === 0)
                text: root.network.discoverySearching ? qsTr("Searching nearby channels...") : root.network.discoveryError || qsTr("No nearby channels yet.")
                color: root.network.discoveryError ? Theme.danger : Theme.muted
                wrapMode: Text.Wrap; Layout.fillWidth: true
            }
            ListView {
                id: discoveryList
                objectName: "discoveryList"
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true; spacing: 4
                model: []
                function refresh() {
                    const choices = root.network.availableHosts
                    if (JSON.stringify(model) === JSON.stringify(choices)) return
                    const previousY = contentY
                    const anchorIndex = indexAt(1, contentY + spacing + 1)
                    const anchor = anchorIndex >= 0 ? itemAtIndex(anchorIndex) : null
                    const anchorId = anchor ? model[anchorIndex].id : ""
                    const offset = anchor ? anchor.y - contentY : 0
                    model = choices
                    forceLayout()
                    const nextIndex = choices.findIndex(function(entry) { return entry.id === anchorId })
                    if (nextIndex >= 0) {
                        positionViewAtIndex(nextIndex, ListView.Beginning)
                        forceLayout()
                        const row = itemAtIndex(nextIndex)
                        contentY = row ? row.y - offset : previousY
                    } else contentY = previousY
                    contentY = Math.max(originY, Math.min(contentY, originY + Math.max(0, contentHeight - height)))
                }
                Component.onCompleted: refresh()
                Connections { target: root.network; function onHostsChanged() { if (discover.opened) discoveryRefresh.start() } }
                ScrollBar.vertical: ScrollBar { id: discoveryBar }
                delegate: RowLayout {
                    required property var modelData
                    objectName: "discovered_" + modelData.id
                    width: discoveryList.width - (discoveryBar.visible ? discoveryBar.width + 4 : 0)
                    Glyph { symbol: "channel"; color: Theme.accent }
                    Label { Layout.fillWidth: true; text: modelData.name; textFormat: Text.PlainText; elide: Text.ElideRight }
                    ActionButton { objectName: "addDiscovered_" + modelData.id; text: qsTr("Add"); primary: true; onClicked: { if (root.network.openChat(modelData.id, modelData.address, modelData.port)) discover.close() } }
                }
            }
            EntryField { id: directAddress; objectName: "directAddress"; Layout.fillWidth: true; placeholderText: qsTr("Host or IP address, optional :port"); Accessible.name: placeholderText; maximumLength: 300; onAccepted: if (joinAddress.enabled) joinAddress.clicked() }
            RowLayout {
                ActionButton { id: joinAddress; objectName: "joinAddress"; text: qsTr("Add"); glyph: "link"; primary: true; enabled: root.network.ready && !root.network.directBusy && directAddress.text.trim().length > 0; onClicked: root.network.openAddress(directAddress.text) }
                ActionButton { objectName: "createOwnChannel"; text: qsTr("Host a channel"); visible: root.voice.supporterEnabled; enabled: root.network.ownedChannels.length < 10; onClicked: newOwnChannel.open() }
                ActionButton { objectName: "approveAddress"; text: qsTr("Allow device"); enabled: joinAddress.enabled; onClicked: root.network.approveAddress(directAddress.text) }
            }
            Label { Layout.fillWidth: true; text: root.network.status; textFormat: Text.PlainText; color: Theme.muted; wrapMode: Text.Wrap }
        }
    }
    PanelDialog {
        id: radioBrowser
        objectName: "radioBrowser"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(480, parent.width - 24)
        height: Math.min(550, parent.height - 24)
        title: qsTr("Radio")
        modal: true
        onOpened: { radioSearch.text = ""; radioResults.reload(); radioSearch.forceActiveFocus() }
        Connections { target: root.hostRadio; function onStationsChanged() { if (radioBrowser.visible) radioResults.reload() } }
        contentItem: ColumnLayout {
            RowLayout {
                Layout.fillWidth: true
                EntryField {
                    id: radioSearch; objectName: "radioSearch"
                    Layout.fillWidth: true; maximumLength: 256
                    placeholderText: qsTr("Name, country or style"); Accessible.name: placeholderText
                    onTextChanged: radioSearchDelay.restart()
                    onAccepted: { radioSearchDelay.stop(); radioResults.reload() }
                }
                ActionButton { objectName: "addStation"; enabled: !!root.hostRadio; text: qsTr("Add station"); glyph: "plus"; iconOnly: true; flat: true; onClicked: { root.stationId = ""; stationName.text = ""; stationUrl.text = ""; stationEditor.open() } }
            }
            Timer { id: radioSearchDelay; interval: 150; onTriggered: if (radioBrowser.visible) radioResults.reload() }
            ListView {
                id: radioResults; objectName: "radioResults"
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true; reuseItems: true; boundsBehavior: Flickable.StopAtBounds
                property int total: 0
                property string query: ""
                property bool loading: false
                model: ListModel { id: stationRows; dynamicRoles: true }
                function reload() {
                    query = radioSearch.text
                    stationRows.clear()
                    loadMore()
                }
                function loadMore() {
                    if (loading || !root.hostRadio) return
                    loading = true
                    const page = root.hostRadio.searchStations(query, stationRows.count, 50)
                    total = page.total
                    for (const station of page.items) stationRows.append({entry: station})
                    loading = false
                }
                onAtYEndChanged: if (atYEnd && !loading && count < total) loadMore()
                ScrollBar.vertical: ScrollBar { id: radioScroll }
                delegate: RowLayout {
                    required property var entry
                    width: radioResults.width - (radioScroll.visible ? radioScroll.width + 4 : 0)
                    height: 54
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 2
                        Label { text: entry.name; textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true; color: (root.hostRadio && root.hostRadio.active) && root.hostRadio.selectedId === entry.id ? Theme.accent : Theme.text }
                        Label { text: entry.manual ? qsTr("My station") : [entry.country, entry.language].filter(Boolean).join(" / "); textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true; color: Theme.muted; font.pixelSize: 12 }
                        HoverHandler { id: stationHover }
                        ToolTip {
                            id: stationTip
                            visible: stationHover.hovered
                            delay: 500
                            text: entry.manual ? entry.url : qsTr("Directory check: %1").arg(entry.checkedAt.slice(0, 10)) + "\n" + entry.tags
                            contentItem: Label { text: stationTip.text; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.text }
                            width: Math.min(320, radioBrowser.width - 24)
                        }
                    }
                    ActionButton { objectName: "playStation_" + entry.id; enabled: !!root.hostRadio; text: (root.hostRadio && root.hostRadio.active) && root.hostRadio.selectedId === entry.id ? qsTr("Stop") : qsTr("Play"); glyph: (root.hostRadio && root.hostRadio.active) && root.hostRadio.selectedId === entry.id ? "stop" : "play"; iconOnly: true; flat: true; onClicked: (root.hostRadio && root.hostRadio.active) && root.hostRadio.selectedId === entry.id ? root.hostRadio.stop() : root.hostRadio.play(entry.id) }
                    ActionButton { visible: entry.manual; text: qsTr("Edit station"); glyph: "more"; iconOnly: true; flat: true; onClicked: { root.stationId = entry.id; stationName.text = entry.name; stationUrl.text = entry.url; stationEditor.open() } }
                }
                Label { anchors.centerIn: parent; visible: radioResults.total === 0; text: qsTr("No stations found"); color: Theme.muted }
            }
            RowLayout {
                visible: (root.hostRadio && root.hostRadio.active)
                Layout.fillWidth: true
                Label { objectName: "radioStreamStatus"; Layout.fillWidth: true; text: root.radioStateText; textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.muted }
                ActionButton { objectName: "stopCurrentStation"; text: qsTr("Stop"); glyph: "stop"; iconOnly: true; flat: true; onClicked: root.hostRadio.stop() }
            }
            Label { Layout.fillWidth: true; visible: (root.hostRadio ? root.hostRadio.error : radio.error).length > 0; text: root.hostRadio ? root.hostRadio.error : radio.error; textFormat: Text.PlainText; color: Theme.danger; wrapMode: Text.Wrap }
            Label { Layout.fillWidth: true; text: qsTr("Radio Browser snapshot: %1").arg(radio.catalogInfo.retrievedAt.slice(0, 10)); color: Theme.muted; font.pixelSize: 12; wrapMode: Text.Wrap }
            RowLayout {
                Layout.fillWidth: true
                ActionButton { text: qsTr("Source"); flat: true; onClicked: Qt.openUrlExternally(radio.catalogInfo.source) }
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("Close"); onClicked: radioBrowser.close() }
            }
        }
    }
    PanelDialog {
        id: stationEditor
        objectName: "stationEditor"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(440, parent.width - 24)
        title: root.stationId.length > 0 ? qsTr("Edit station") : qsTr("Add station")
        modal: true
        onClosed: if (root.hostRadio) root.hostRadio.cancelStationCheck()
        Connections {
            target: root.hostRadio
            function onStationSaved(station, ok) { if (ok && stationEditor.visible) stationEditor.close() }
        }
        contentItem: ColumnLayout {
            EntryField { id: stationName; objectName: "stationName"; enabled: root.hostRadio && !root.hostRadio.checking; Layout.fillWidth: true; placeholderText: qsTr("Station name"); Accessible.name: placeholderText; maximumLength: 128 }
            EntryField { id: stationUrl; objectName: "stationUrl"; enabled: root.hostRadio && !root.hostRadio.checking; Layout.fillWidth: true; placeholderText: qsTr("Stream URL"); Accessible.name: placeholderText; maximumLength: 2048 }
            Label { text: qsTr("Checking stream..."); visible: root.hostRadio && root.hostRadio.checking; color: Theme.muted; Layout.fillWidth: true; Accessible.role: Accessible.StaticText }
            Label { text: root.hostRadio ? root.hostRadio.error : radio.error; textFormat: Text.PlainText; visible: text.length > 0; color: Theme.danger; wrapMode: Text.Wrap; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                ActionButton { text: qsTr("Remove"); destructive: true; enabled: root.hostRadio && !root.hostRadio.checking; visible: root.stationId.length > 0; onClicked: { if (root.hostRadio.removeStation(root.stationId)) stationEditor.close() } }
                Item { Layout.fillWidth: true }
                ActionButton { text: qsTr("Cancel"); onClicked: stationEditor.close() }
                ActionButton { objectName: "saveStation"; text: qsTr("Save"); primary: true; enabled: root.hostRadio && !root.hostRadio.checking; onClicked: root.hostRadio.saveStation(root.stationId, stationName.text, stationUrl.text) }
            }
        }
    }
    PanelDialog {
        id: settings
        objectName: "settingsDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: parent.width
        height: parent.height
        modal: true
        palette: root.palette
        padding: 16
        header: RowLayout {
            height: 52
            Label { text: root.remote ? qsTr("Settings for this device") : qsTr("Settings"); font.pixelSize: 20; font.weight: Font.DemiBold; Layout.fillWidth: true; Layout.leftMargin: 16 }
            ActionButton { text: qsTr("Done"); glyph: "close"; flat: true; Layout.rightMargin: 8; onClicked: settings.close() }
        }
        onOpened: { settingsScroll.contentItem.contentY = 0; if (root.settingsPage === 3) root.pickAboutSentence() }
        onClosed: audio.stop()
        contentItem: ColumnLayout {
            TabBar {
                id: settingsTabs
                objectName: "settingsSection"
                readonly property var labels: [qsTr("Profile"), qsTr("Input"), qsTr("Output"), qsTr("About")]
                Layout.fillWidth: true
                implicitWidth: 320
                currentIndex: root.settingsPage
                Repeater {
                    model: settingsTabs.labels.length
                    TabButton {
                        required property int index
                        objectName: "settingsTab_" + index
                        text: settingsTabs.labels[index]
                        width: settingsTabs.width / settingsTabs.count
                        font.pixelSize: 12
                        contentItem: Text { text: parent.text; font: parent.font; color: parent.checked ? Theme.text : Theme.muted; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight }
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: text
                        background: Rectangle {
                            color: parent.hovered ? Theme.raised : "transparent"
                            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 2; color: parent.parent.checked ? Theme.accent : Theme.border }
                        }
                        onClicked: root.settingsPage = index
                    }
                }
            }
            AudioSettings {
                id: audioSettingsPanel
                objectName: "audioSettings"
                Layout.fillWidth: true; Layout.fillHeight: true
                visible: root.settingsPage >= 1 && root.settingsPage <= 2
                settingsPage: root.settingsPage - 1
                sessionSource: root.voice
                onRemoteSetupRequested: remoteTargets.open()
            }
            ScrollView {
                id: settingsScroll
                objectName: "settingsScroll"
                visible: root.settingsPage === 0 || root.settingsPage === 3
                Layout.fillWidth: true; Layout.fillHeight: true; clip: true; contentWidth: availableWidth
                ColumnLayout {
                    width: settingsScroll.availableWidth
                    spacing: 16
                    ColumnLayout {
                        objectName: "aboutSettings"
                        visible: root.settingsPage === 3
                        Layout.fillWidth: true; spacing: 18
                        RowLayout {
                            Layout.fillWidth: true; spacing: 12
                            Image { objectName: "aboutLogo"; source: "icons/app.png"; Layout.preferredWidth: 52; Layout.preferredHeight: 52; fillMode: Image.PreserveAspectFit; sourceSize.width: 104; sourceSize.height: 104 }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 2
                                Label { text: "SquadSpeak"; font.pixelSize: 20; font.weight: Font.DemiBold }
                                Label { objectName: "aboutVersion"; Layout.fillWidth: true; text: qsTr("Version %1").arg(Qt.application.version); color: Theme.muted; wrapMode: Text.Wrap }
                                Label {
                                    objectName: "aboutLicense"
                                    text: "YunaBraska - <a href='https://spdx.org/licenses/GPL-3.0-only.html'>GPL-3.0</a>"
                                    textFormat: Text.RichText; color: Theme.muted; linkColor: Theme.accent; font.pixelSize: 12
                                    onLinkActivated: link => Qt.openUrlExternally(link)
                                }
                            }
                        }
                        Label { objectName: "aboutSentence"; Layout.fillWidth: true; text: root.aboutIndex >= 0 ? root.aboutLines[root.aboutIndex] : ""; wrapMode: Text.Wrap }
                        RowLayout {
                            visible: root.updates !== null && root.updates.available
                            Layout.fillWidth: true
                            Label { text: qsTr("Updates"); Layout.fillWidth: true }
                            ActionButton { objectName: "checkUpdates"; text: qsTr("Check now"); onClicked: root.updates.check() }
                        }
                        ColumnLayout {
                            objectName: "supporterSettings"
                            visible: root.supporter !== null && root.supporter.directDistribution
                            Layout.fillWidth: true; spacing: 8
                            Label { text: qsTr("Supporter"); font.weight: Font.DemiBold }
                            Label {
                                objectName: "supporterState"
                                Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted
                                text: !root.supporter || !root.supporter.configured ? qsTr("Not available yet")
                                    : root.supporter.active ? qsTr("Valid until %1").arg(Qt.formatDate(root.supporter.expiresAt, Qt.DefaultLocaleShortDate))
                                    : root.supporter.signedIn && root.supporter.account.length > 0 ? qsTr("Signed in as %1").arg(root.supporter.account)
                                    : qsTr("Free version")
                            }
                            DetailText {
                                objectName: "supporterAccount"
                                visible: root.supporter !== null && root.supporter.signedIn && root.supporter.account.length > 0
                                text: qsTr("Account: %1").arg(root.supporter.account)
                                color: Theme.muted; Layout.fillWidth: true; wrapMode: TextEdit.Wrap
                            }
                            ActionButton {
                                objectName: "signInGithub"
                                visible: root.supporter !== null && root.supporter.configured && !root.supporter.signedIn && !root.supporter.pending
                                enabled: !root.supporter.busy
                                text: qsTr("Sign in with GitHub")
                                Layout.fillWidth: true
                                onClicked: root.supporter.signIn()
                            }
                            ColumnLayout {
                                visible: root.supporter !== null && root.supporter.pending
                                Layout.fillWidth: true; spacing: 6
                                Label { text: qsTr("Verification code"); visible: root.supporter.userCode.length > 0; color: Theme.muted }
                                DetailText { objectName: "githubUserCode"; text: root.supporter.userCode; visible: text.length > 0; Accessible.name: qsTr("Verification code"); selectByMouse: true; wrapMode: TextEdit.NoWrap }
                                Label { text: root.supporter.status; visible: root.supporter.userCode.length === 0 && text.length > 0; color: Theme.muted; wrapMode: Text.Wrap }
                                RowLayout {
                                    Layout.fillWidth: true
                                    ActionButton { objectName: "openGithubVerification"; text: qsTr("Open GitHub"); enabled: root.supporter.verificationUrl.toString().length > 0; onClicked: Qt.openUrlExternally(root.supporter.verificationUrl) }
                                    ActionButton { objectName: "cancelGithubSignIn"; text: qsTr("Cancel"); onClicked: root.supporter.cancelSignIn() }
                                }
                            }
                            GridLayout {
                                visible: root.supporter !== null && root.supporter.configured
                                Layout.fillWidth: true; columns: 2
                                ActionButton { objectName: "supportLink"; Layout.columnSpan: 2; Layout.fillWidth: true; text: qsTr("Support development"); visible: root.supporter.purchaseUrl.toString().length > 0; onClicked: Qt.openUrlExternally(root.supporter.purchaseUrl) }
                                ActionButton { objectName: "checkSupporter"; visible: root.supporter.signedIn; text: qsTr("Check now"); enabled: !root.supporter.busy; Layout.fillWidth: true; onClicked: root.supporter.refresh() }
                                ActionButton { objectName: "signOutGithub"; visible: root.supporter.signedIn; text: qsTr("Sign out"); destructive: true; enabled: !root.supporter.busy; Layout.fillWidth: true; onClicked: root.supporter.signOut() }
                            }
                            Label { text: root.supporter ? root.supporter.status : ""; visible: text.length > 0; color: Theme.muted; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            AvatarShelf {
                                objectName: "supporterAvatarPreview"; preview: true
                                visible: !root.voice.supporterEnabled
                                model: root.voice.avatars.slice(10)
                            }
                        }
                        GridLayout {
                            columns: 2; Layout.fillWidth: true
                            Repeater {
                                model: [
                                    {key: "repository", label: qsTr("Repository"), url: "https://github.com/YunaBraska/SquadSpeak"},
                                    {key: "issues", label: qsTr("Report an issue"), url: "https://github.com/YunaBraska/SquadSpeak/issues/new"}
                                ]
                                ActionButton { required property var modelData; objectName: "aboutLink_" + modelData.key; Layout.fillWidth: true; text: modelData.label; onClicked: Qt.openUrlExternally(modelData.url) }
                            }
                        }
                        Label { text: qsTr("Support development"); font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.Wrap }
                        GridLayout {
                            columns: 2; Layout.fillWidth: true
                            Repeater {
                                model: [
                                    {key: "sponsors", label: "GitHub Sponsors", url: "https://github.com/sponsors/YunaBraska"},
                                    {key: "coffee", label: "Buy Me a Coffee", url: "https://buymeacoffee.com/YunaBraska"},
                                    {key: "kofi", label: "Ko-fi", url: "https://ko-fi.com/YunaBraska"},
                                    {key: "liberapay", label: "Liberapay", url: "https://liberapay.com/YunaBraska"}
                                ]
                                ActionButton { required property var modelData; objectName: "aboutLink_" + modelData.key; Layout.fillWidth: true; text: modelData.label; onClicked: Qt.openUrlExternally(modelData.url) }
                            }
                        }
                    }
                    ColumnLayout {
                        objectName: "generalSettings"
                        visible: root.settingsPage === 0
                        Layout.fillWidth: true; spacing: 12
                        AvatarShelf { objectName: "avatarShelf" }
                        RowLayout {
                            Layout.fillWidth: true
                            EntryField { id: displayName; objectName: "userName"; Layout.fillWidth: true; maximumLength: 128; Accessible.name: qsTr("Your name"); text: root.voice.userName; onAccepted: root.voice.setUserName(text); onEditingFinished: root.voice.setUserName(text) }
                            ActionButton { objectName: "saveDisplayName"; text: qsTr("Save name"); glyph: "check"; iconOnly: true; enabled: displayName.text.trim() !== root.voice.userName; onClicked: root.voice.setUserName(displayName.text) }
                        }
                        Label { visible: root.voice.error.length > 0; text: root.voice.error; color: Theme.danger; wrapMode: Text.Wrap; Layout.fillWidth: true }
                        ChoiceBox {
                            objectName: "languageChoice"
                            Layout.fillWidth: true
                            model: root.voice.languages
                            textRole: "label"; valueRole: "code"
                            currentIndex: root.voice.languages.findIndex(function(item) { return item.code === root.voice.language })
                            Accessible.name: qsTr("Language")
                            onActivated: { root.voice.setLanguage(currentValue); currentIndex = Qt.binding(function() { return root.voice.languages.findIndex(function(item) { return item.code === root.voice.language }) }) }
                        }
                        Label { text: qsTr("Appearance"); font.weight: Font.DemiBold }
                        OptionCheck {
                            objectName: "showInDock"
                            visible: Qt.platform.os === "osx"
                            text: qsTr("Show in Dock")
                            checked: root.voice.showInDock
                            onClicked: { root.voice.setShowInDock(checked); checked = Qt.binding(function() { return root.voice.showInDock }) }
                        }
                        ChoiceBox {
                            objectName: "themeChoice"
                            Layout.fillWidth: true
                            model: [qsTr("System (automatic)"), qsTr("Light"), qsTr("Dark")]
                            currentIndex: ["system", "light", "dark"].indexOf(root.voice.theme)
                            Accessible.name: qsTr("Theme")
                            onActivated: { root.voice.setTheme(["system", "light", "dark"][currentIndex]); currentIndex = Qt.binding(function() { return ["system", "light", "dark"].indexOf(root.voice.theme) }) }
                        }
                        ChoiceBox {
                            objectName: "paletteChoice"
                            Layout.fillWidth: true
                            model: [qsTr("Plum"), qsTr("Ocean"), qsTr("Forest"), qsTr("Graphite")]
                            currentIndex: ["plum", "ocean", "forest", "graphite"].indexOf(root.voice.palette)
                            Accessible.name: qsTr("Colour palette")
                            onActivated: { root.voice.setPalette(["plum", "ocean", "forest", "graphite"][currentIndex]); currentIndex = Qt.binding(function() { return ["plum", "ocean", "forest", "graphite"].indexOf(root.voice.palette) }) }
                        }
                        OptionCheck { text: qsTr("Notification sounds"); checked: root.voice.eventSounds; onClicked: { root.voice.setEventSounds(checked); checked = Qt.binding(function() { return root.voice.eventSounds }) } }
                        OptionCheck { objectName: "animatedAvatars"; text: qsTr("Animate portraits"); checked: root.voice.animatedAvatars; onClicked: { root.voice.setAnimatedAvatars(checked); checked = Qt.binding(function() { return root.voice.animatedAvatars }) } }
                    }
                }
            }
        }
    }
    PanelDialog {
        id: remoteTargets
        objectName: "remoteTargetsDialog"
        property string error: ""
        readonly property var members: {
            if (!visible) return []
            const members = {}
            for (const entry of root.network.controllers) members[entry.id] = entry
            for (const channel of root.network.ownedChannels) {
                const owner = root.network.ownChannel(channel.id)
                if (owner) for (const entry of owner.hostClients) {
                    if (entry.id !== root.network.ownId) members[entry.id] = entry
                }
            }
            return Object.values(members).sort(function(a, b) { return a.name.localeCompare(b.name) || a.id.localeCompare(b.id) })
        }
        onOpened: error = ""
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(440, parent.width - 24)
        height: Math.min(520, parent.height - 24, remoteSetupContent.implicitHeight + topPadding + bottomPadding + header.implicitHeight + footer.implicitHeight)
        modal: true; title: qsTr("Remote control"); buttons: Dialog.Close
        contentItem: ScrollView {
            id: remoteSetupScroll
            contentWidth: availableWidth; clip: true
            ColumnLayout {
                id: remoteSetupContent
                width: remoteSetupScroll.availableWidth; spacing: 12
                Label {
                    text: qsTr("PTT, chat and channel controls. Uses the controlled device's microphone.")
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; font.pixelSize: 12
                }
                Label { text: qsTr("Allow remote control"); font.bold: true }
                Label { text: qsTr("This device") + ": " + root.voice.userName; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted }
                Label {
                    objectName: "remoteClientsEmpty"
                    visible: remoteTargets.members.length === 0
                    text: qsTr("Connect the other device to one of your channels, then allow it here.")
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted
                }
                Repeater {
                    model: remoteTargets.members
                    delegate: OptionCheck {
                        required property var modelData
                        objectName: "remotePermission_" + modelData.id
                        text: modelData.name
                        Layout.fillWidth: true
                        implicitHeight: Math.max(32, contentItem.implicitHeight)
                        checked: root.network.controllers.some(function(p) { return p.id === modelData.id && p.remote })
                        onClicked: {
                            const allowed = root.network.controllers.some(function(p) { return p.id === modelData.id && p.remote })
                            remoteTargets.error = root.network.setRemotePermission(modelData.id, !allowed) ? "" : root.network.controlStatus
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; implicitHeight: 1; color: Theme.border }
                Label { text: qsTr("Control a device"); font.bold: true }
                Label {
                    objectName: "remoteTargetsEmpty"
                    visible: root.network.remoteOffers.length === 0
                    text: qsTr("On the other device, open Settings > Input > Remote control and allow this device.")
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted
                }
                Repeater {
                    model: root.network.remoteOffers
                    delegate: ActionButton {
                        required property var modelData
                        objectName: "remoteTarget_" + modelData.id
                        text: modelData.ownerName || modelData.name; glyph: "remote"; Layout.fillWidth: true; horizontalAlignment: Text.AlignLeft
                        onClicked: root.controlDevice(modelData.id)
                    }
                }
                Label { visible: remoteTargets.error.length > 0; text: remoteTargets.error; textFormat: Text.PlainText; color: Theme.danger; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
        }
    }
    PanelDialog {
        id: channelInfo
        objectName: "channelInfoDialog"
        property var addresses: []
        property bool focusBlocked: false
        readonly property var entry: root.channelList.find(function(h) { return h.id === root.selectedHost }) || ({})
        readonly property bool manageable: !!root.selectedOwner
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(440, parent.width - 24)
        height: Math.min(manageable ? 620 : 300, parent.height - 24)
        modal: true; focus: true; title: qsTr("Channel info")
        buttons: Dialog.Close
        function positionContent() {
            const flick = channelInfoScroll.contentItem
            flick.contentY = focusBlocked ? Math.max(0, Math.min(flick.contentHeight - flick.height,
                blockedDeviceList.mapToItem(flick.contentItem, 0, 0).y)) : 0
        }
        onOpened: {
            addresses = root.network.channelAddresses(root.selectedHost)
            internetHelp.visible = false
            Qt.callLater(positionContent)
        }
        onClosed: focusBlocked = false
        contentItem: ScrollView {
            id: channelInfoScroll
            objectName: "channelInfoScroll"
            contentWidth: availableWidth; clip: true
            ColumnLayout {
                width: channelInfoScroll.availableWidth
                spacing: 12
                onHeightChanged: if (channelInfo.opened && channelInfo.focusBlocked) Qt.callLater(channelInfo.positionContent)
                DetailText { objectName: "channelDisplayName"; visible: !channelInfo.manageable; text: channelInfo.entry.name || ""; font.weight: Font.DemiBold }
                Label { text: qsTr("Address"); visible: channelInfo.addresses.length > 0; color: Theme.muted }
                Repeater {
                    model: channelInfo.addresses
                    delegate: RowLayout {
                        required property string modelData
                        Layout.fillWidth: true
                        DetailText { id: addressText; objectName: "channelAddress"; text: modelData; color: Theme.muted; Accessible.name: qsTr("Address") }
                        ActionButton { objectName: "copyChannelAddress"; text: qsTr("Copy address"); glyph: "copy"; iconOnly: true; flat: true; onClicked: { addressText.selectAll(); addressText.copy(); addressText.deselect() } }
                    }
                }
                DetailText { objectName: "channelIdentity"; text: channelInfo.entry.id || ""; color: Theme.muted; font.pixelSize: 11; Accessible.name: qsTr("Channel ID") }
                ActionButton { objectName: "internetHelpToggle"; text: qsTr("Internet access"); flat: true; onClicked: internetHelp.visible = !internetHelp.visible }
                ColumnLayout {
                    id: internetHelp
                    objectName: "internetHelp"
                    visible: false; Layout.fillWidth: true; spacing: 8
                    DetailText { text: qsTr("Share a DNS name such as my-channel.duckdns.org:%1. Your router or DDNS provider keeps its IP address updated; SquadSpeak resolves the name again when reconnecting.").arg(channelInfo.entry.port || 48763); color: Theme.muted; font.pixelSize: 12 }
                    DetailText { objectName: "internetPortHelp"; text: qsTr("On the host's router, forward %2 port %1 to the host and allow it through the firewall. Set a channel password.").arg(channelInfo.entry.port || 48763).arg("TCP/UDP"); color: Theme.muted; font.pixelSize: 12 }
                    DetailText { text: qsTr("DDNS cannot bypass CGNAT. If your provider shares the public IP, use a private VPN on all participating devices or a publicly reachable server."); color: Theme.muted; font.pixelSize: 12 }
                    RowLayout {
                        ActionButton { objectName: "ddnsHelp"; text: "DuckDNS"; flat: true; onClicked: Qt.openUrlExternally("https://www.duckdns.org/why.jsp") }
                        ActionButton { objectName: "vpnHelp"; text: "Tailscale"; flat: true; onClicked: Qt.openUrlExternally("https://tailscale.com/kb/1017/install") }
                    }
                }
                Label { visible: !channelInfo.manageable; text: channelInfo.entry.online ? qsTr("Access approved") : root.accessLabel(channelInfo.entry.access) || qsTr("Saved channel"); color: Theme.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
                OptionCheck {
                    objectName: "channelInfoAutoJoin"
                    visible: !root.remote; enabled: !root.remote
                    text: qsTr("Auto-join")
                    checked: !!channelInfo.entry.autoJoin
                    onClicked: { root.network.setAutoJoin(root.selectedHost, checked); checked = Qt.binding(function() { return !!channelInfo.entry.autoJoin }) }
                }
                Label { visible: root.remote; text: channelInfo.entry.autoJoin ? qsTr("Auto-join enabled") : qsTr("Auto-join disabled"); color: Theme.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
                Label { visible: !channelInfo.manageable && (channelInfo.entry.lifetimeDays || 0) > 0; text: qsTr("New messages: %1 days").arg(channelInfo.entry.lifetimeDays || 1); color: Theme.muted; Layout.fillWidth: true; wrapMode: Text.Wrap }
                ColumnLayout {
                    objectName: "hostPage"
                    visible: channelInfo.manageable
                    enabled: channelInfo.manageable
                    Layout.fillWidth: true; spacing: 12
                    EntryField { objectName: "channelName"; Layout.fillWidth: true; text: root.managedHost.channelName; Accessible.name: qsTr("Channel name"); onEditingFinished: { root.managedHost.setChannelName(text); text = Qt.binding(function() { return root.managedHost.channelName }) } }
                    Label { text: root.managedHost.hosting ? qsTr("Hosting") : qsTr("Channel unavailable"); color: Theme.muted; wrapMode: Text.Wrap; Layout.fillWidth: true }
                    Label { objectName: "channelConfiguration"; text: root.managedHost.passwordProtected ? qsTr("Password required") : qsTr("No password"); color: Theme.muted }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: qsTr("Port"); Layout.fillWidth: true }
                        SpinBox { background: Rectangle { radius: Theme.controlRadius; color: Theme.input; border.color: parent.activeFocus ? Theme.accent : Theme.border }
                            up.indicator: Rectangle { x: parent.width - width; height: parent.height; width: 24; radius: Theme.controlRadius; color: Theme.raised; Glyph { anchors.centerIn: parent; symbol: "plus"; width: 12; height: 12 } }
                            down.indicator: Rectangle { height: parent.height; width: 24; radius: Theme.controlRadius; color: Theme.raised; Rectangle { anchors.centerIn: parent; width: 8; height: 1; color: Theme.text } }
                            id: hostPort; objectName: "hostPort"; from: 1; to: 65535; editable: true; value: root.managedHost.configuredPort; textFromValue: function(value) { return value.toString() }; valueFromText: function(text) { return /^\d+$/.test(text) ? Number(text) : value }; Accessible.name: qsTr("Host port") }
                        ActionButton { objectName: "saveHostPort"; text: qsTr("Apply port"); glyph: "check"; iconOnly: true; enabled: hostPort.value !== root.managedHost.configuredPort; onClicked: root.managedHost.setConfiguredPort(hostPort.value) }
                    }
                    Label { text: qsTr("Port changes apply after restart."); color: Theme.muted; font.pixelSize: 12 }
                    RowLayout {
                        Layout.fillWidth: true
                        Label { objectName: "messageLifetimeLabel"; text: qsTr("Keep new messages"); Layout.fillWidth: true; Layout.minimumWidth: 0; wrapMode: Text.WordWrap }
                        ChoiceBox {
                            objectName: "messageLifetime"
                            Layout.preferredWidth: 128
                            property var durations: session.supporterEnabled ? [1, 7, 30, 90, 180, 360] : [1, 7, 30]
                            model: session.supporterEnabled
                                ? [qsTr("24 hours"), qsTr("7 days"), qsTr("30 days"), qsTr("90 days"), qsTr("180 days"), qsTr("360 days")]
                                : [qsTr("24 hours"), qsTr("7 days"), qsTr("30 days")]
                            currentIndex: durations.indexOf(root.managedHost.messageLifetimeDays)
                            Accessible.name: qsTr("New message lifetime")
                            onActivated: { root.managedHost.setMessageLifetimeDays(durations[currentIndex]); currentIndex = Qt.binding(function() { return durations.indexOf(root.managedHost.messageLifetimeDays) }) }
                        }
                    }
                    RowLayout {
                        ActionButton { objectName: "editHostPassword"; text: qsTr("Password"); enabled: !root.managedHost.passwordBusy; onClicked: hostPassword.open() }
                    }
                    Label { text: qsTr("System bot name"); font.weight: Font.DemiBold; Layout.fillWidth: true; wrapMode: Text.Wrap }
                    RowLayout {
                        Layout.fillWidth: true
                        EntryField { id: systemBotName; objectName: "systemBotName"; Layout.fillWidth: true; text: root.managedHost.botName; placeholderText: qsTr("System bot name"); Accessible.name: placeholderText; maximumLength: 128; onAccepted: root.managedHost.setBotName(text) }
                        ActionButton { objectName: "saveSystemBotName"; text: qsTr("Save"); onClicked: root.managedHost.setBotName(systemBotName.text) }
                    }
                    Label { visible: (root.hostRadio && root.hostRadio.active); Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted; text: root.radioStateText; textFormat: Text.PlainText }
                    OptionCheck { text: qsTr("Allow new join requests"); checked: root.managedHost.requestsAllowed; onClicked: { root.managedHost.setRequestsAllowed(checked); checked = Qt.binding(function() { return root.managedHost.requestsAllowed }) } }
                    Repeater {
                        model: root.managedHost.requests.filter(function(r) { return r.hostId === root.managedHost.channelId })
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label { text: modelData.name; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight }
                            ActionButton { text: qsTr("Allow"); glyph: "check"; iconOnly: true; primary: true; onClicked: root.managedHost.decide(modelData.id, true) }
                            ActionButton { text: qsTr("Deny"); destructive: true; glyph: "close"; iconOnly: true; onClicked: root.managedHost.decide(modelData.id, false) }
                        }
                    }
                    Repeater {
                        model: root.managedHost.hostClients.concat(root.managedHost.controllers.filter(function(c) { return !root.managedHost.hostClients.some(function(p) { return p.id === c.id }) }))
                        delegate: RowLayout {
                            required property var modelData
                            visible: modelData.id !== root.managedHost.ownId && !modelData.music
                            Layout.fillWidth: true
                            Label { text: modelData.name; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight }
                            ActionButton { objectName: "manageMember_" + modelData.id; text: qsTr("Manage member"); glyph: "more"; iconOnly: true; onClicked: root.memberOptions(modelData, root.managedHost.channelId, true) }
                        }
                    }
                    ColumnLayout {
                        id: blockedDeviceList
                        Layout.fillWidth: true; spacing: 8
                        visible: root.managedHost.blockedClients.length > 0
                    Label { visible: root.managedHost.blockedClients.length > 0; text: qsTr("Blocked devices"); color: Theme.muted }
                    Repeater {
                        model: root.managedHost.blockedClients
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label { text: modelData.name; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight }
                            ActionButton { objectName: "unblock_" + modelData.id; text: qsTr("Unblock"); onClicked: root.managedHost.setBlocked(modelData.id, false) }
                        }
                    }
                    }
                    Label { text: root.managedHost.status; textFormat: Text.PlainText; wrapMode: Text.Wrap; Layout.fillWidth: true; color: Theme.muted }
                }
            }
        }
    }

    PanelDialog {
        id: requestInfo
        objectName: "requestInfoDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(440, parent.width - 24)
        height: Math.min(requestDetails.implicitHeight + topPadding + bottomPadding + implicitHeaderHeight + implicitFooterHeight + spacing * 2, parent.height - 24)
        modal: true; focus: true; title: root.inspectedRequestId ? qsTr("Join request") : qsTr("Inspect")
        readonly property var entry: root.inspectedRequestId ? root.network.requests.find(function(r) { return r.id === root.inspectedRequestId && (!root.inspectedRequestHost || r.hostId === root.inspectedRequestHost) }) || ({}) : root.selectedMember
        readonly property bool personalControls: !root.inspectedRequestId && !root.remote && !!entry.id && !entry.music && entry.id !== root.network.ownId
        onOpened: {
            requestScroll.contentItem.contentY = 0
        }
        onClosed: memberMenu.close()
        footer: RowLayout {
            spacing: 8
            ActionButton { objectName: "denyInspectedRequest"; text: qsTr("Deny"); destructive: true; visible: !!root.inspectedRequestId; Layout.leftMargin: 12; Layout.bottomMargin: 12; enabled: !!requestInfo.entry.id; onClicked: root.decideRequest(requestInfo.entry.id, requestInfo.entry.hostId, false) }
            Item { Layout.fillWidth: true }
            ActionButton { text: qsTr("Close"); Layout.bottomMargin: 12; Layout.rightMargin: root.inspectedRequestId ? 0 : 12; onClicked: requestInfo.close() }
            ActionButton { objectName: "approveInspectedRequest"; text: qsTr("Approve"); primary: true; visible: !!root.inspectedRequestId; Layout.rightMargin: 12; Layout.bottomMargin: 12; enabled: !!requestInfo.entry.id; onClicked: root.decideRequest(requestInfo.entry.id, requestInfo.entry.hostId, true) }
        }
        contentItem: ScrollView {
            id: requestScroll
            objectName: "requestDetailsScroll"
            contentWidth: availableWidth; clip: true
            ColumnLayout {
                id: requestDetails
                width: requestScroll.availableWidth; spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    VoiceAvatar { objectName: "requestAvatar"; width: 56; height: 56; circular: true; animated: false; avatar: requestInfo.entry.avatarId || "mossling"; name: requestInfo.entry.name || ""; teamSpeak: !!requestInfo.entry.teamSpeak }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 2
                        DetailText { objectName: "requestName"; text: requestInfo.entry.name || ""; font.weight: Font.DemiBold }
                        DetailText { objectName: "requestClient"; visible: !!requestInfo.entry.id && !requestInfo.entry.music; text: requestInfo.entry.teamSpeak ? "TeamSpeak" : "SquadSpeak"; color: Theme.muted; font.pixelSize: 12 }
                    }
                    ActionButton {
                        objectName: "messageInspectedMember"
                        text: qsTr("Message"); glyph: "chat"; iconOnly: true; flat: true
                        visible: requestInfo.personalControls && root.network.canPrivateChat(root.selectedMemberHost, requestInfo.entry.id)
                        Accessible.name: qsTr("Message")
                        onClicked: root.openPrivateForMember()
                    }
                    ActionButton {
                        objectName: "memberActions"; text: qsTr("Manage member"); glyph: "more"; iconOnly: true; flat: true
                        visible: !root.inspectedRequestId && (root.canGrantMemberControl || root.canModerateMember || root.canStopMemberRadio)
                        onClicked: memberMenu.popup()
                    }
                }
                ActionButton {
                    objectName: "audioQuality"
                    readonly property int bitrate: root.current.receiveAudioBitrate || 0
                    visible: !root.inspectedRequestId && requestInfo.entry.id === root.current.ownId && bitrate > 0
                    text: (bitrate >= 32 ? qsTr("High") : bitrate === 20 ? qsTr("Balanced") : bitrate === 12 ? qsTr("Low") : qsTr("Minimum")) + " (" + bitrate + " kb/s)"
                    glyph: "signal"; flat: true; font.pixelSize: 12
                    Layout.fillWidth: true
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.text: qsTr("Audio received")
                    onClicked: ToolTip.show(ToolTip.text, 5000)
                }
                SettingSlider {
                    id: memberGain; objectName: "memberVolume"
                    Layout.fillWidth: true; visible: requestInfo.personalControls
                    caption: qsTr("Volume for me")
                    from: 0; to: 2; stepSize: 0.05
                    property int revision: 0
                    value: { revision; return requestInfo.entry.id ? audio.participantVolume(requestInfo.entry.id) : 1 }
                    valueText: Math.round(value * 100) + " %"
                    autoAvailable: true
                    automatic: { revision; return audio.participantAutomatic(requestInfo.entry.id || "") }
                    applyAutomatic: function(enabled) { return requestInfo.personalControls && audio.setParticipantAutomatic(requestInfo.entry.id, enabled) }
                    applyValue: function(value) { return requestInfo.personalControls && audio.setParticipantVolume(requestInfo.entry.id, value) }
                    Connections { target: audio; function onProfileChanged() { memberGain.revision++ } }
                }
                Label { text: qsTr("Channel"); visible: !!requestInfo.entry.channel; color: Theme.muted }
                DetailText { objectName: "requestChannel"; visible: text.length > 0; text: requestInfo.entry.channel || "" }
                Label { text: qsTr("Address"); visible: !!requestInfo.entry.address; color: Theme.muted }
                DetailText { objectName: "requestAddress"; visible: text.length > 0; text: root.endpoint(requestInfo.entry) }
                Label { text: qsTr("Identity ID"); color: Theme.muted }
                DetailText { objectName: "requestIdentity"; text: requestInfo.entry.id || ""; font.pixelSize: 11 }
            }
        }
    }
    PanelDialog {
        id: newOwnChannel
        objectName: "newOwnChannelDialog"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(380, parent.width - 24)
        modal: true; focus: true; title: qsTr("Host a channel")
        buttons: Dialog.Cancel
        contentItem: ColumnLayout {
            EntryField { id: ownChannelName; objectName: "newOwnChannelName"; Layout.fillWidth: true; maximumLength: 128; placeholderText: qsTr("Channel name"); Accessible.name: placeholderText }
            ActionButton {
                objectName: "saveOwnChannel"; text: qsTr("Create"); primary: true; enabled: ownChannelName.text.trim().length > 0
                onClicked: {
                    const id = root.network.addOwnedChannel(ownChannelName.text.trim())
                    if (id.length) { newOwnChannel.close(); discover.close(); ownChannelName.clear(); root.openHostChat(id) }
                    else root.showNotice(root.network.status)
                }
            }
        }
    }
    PanelDialog {
        id: removeOwnChannel
        objectName: "removeOwnChannelDialog"
        property string channelId: ""
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(380, parent.width - 24)
        modal: true; focus: true; title: qsTr("Remove own channel?")
        buttons: Dialog.Cancel
        onOpened: channelId = root.selectedHost
        contentItem: ColumnLayout {
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; text: qsTr("This deletes this channel's history, images, permissions and radio settings.") }
            ActionButton { objectName: "confirmRemoveOwnChannel"; text: qsTr("Remove"); destructive: true; onClicked: { if (root.network.removeOwnedChannel(removeOwnChannel.channelId)) { removeOwnChannel.close(); channelInfo.close() } else root.showNotice(root.network.status) } }
        }
    }
    PanelDialog {
        id: screenPicker
        objectName: "screenPicker"
        property int selectedSource: -1
        onOpened: selectedSource = -1
        onClosed: if (root.screen) { root.screen.detach(sourcePreview.videoSink); root.screen.stopPreview() }
        title: qsTr("Share screen")
        modal: true; anchors.centerIn: parent
        width: Math.min(root.width - 24, 480); height: Math.min(root.height - 32, 440)
        buttons: Dialog.Cancel
        contentItem: ColumnLayout {
            Label {
                objectName: "screenCaptureError"
                Layout.fillWidth: true
                visible: root.screen && root.screen.error.length > 0
                text: root.screen ? root.screen.error : ""
                textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.danger
            }
            ActionButton {
                objectName: "screenPermissionSettings"
                visible: root.screen && root.screen.permissionRequired === true
                text: qsTranslate("AudioSettings", "Open system settings")
                onClicked: root.screen.openPermissionSettings()
            }
            ScrollView {
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true
                ListView {
                    model: root.screen ? root.screen.sources : []
                    spacing: 4
                    delegate: ActionButton {
                        required property var modelData
                        objectName: "screenSource_" + modelData.index
                        width: ListView.view.width
                        flat: true; horizontalAlignment: Text.AlignLeft
                        text: modelData.name
                        checked: screenPicker.selectedSource === modelData.index
                        onClicked: {
                            screenPicker.selectedSource = modelData.index
                            root.screen.previewSource(modelData.index, sourcePreview.videoSink)
                        }
                    }
                }
            }
            VideoOutput {
                id: sourcePreview; objectName: "screenSourcePreview"
                visible: screenPicker.selectedSource >= 0
                Layout.fillWidth: true; Layout.preferredHeight: Math.min(140, screenPicker.height * 0.35)
                fillMode: VideoOutput.PreserveAspectFit
            }
            ActionButton {
                objectName: "startScreenShare"; text: qsTr("Share screen"); primary: true
                Layout.fillWidth: true; enabled: screenPicker.selectedSource >= 0 && root.screen && !root.screen.error.length
                onClicked: if (root.screen.start(screenPicker.selectedSource, root.selectedHost)) screenPicker.close()
            }
        }
    }
    Menu {
        popupType: Popup.Item
        background: Rectangle { implicitWidth: 240; implicitHeight: 40; radius: Theme.controlRadius; color: Theme.surface; border.color: Theme.border }
        id: channelMenu
        objectName: "channelMenu"
        width: Math.min(root.width - 24, root.language.supported && !root.remote ? 336 : 240)
        MenuAction { text: qsTr("Channel info"); onTriggered: channelInfo.open() }
        MenuAction {
            objectName: "blockedDevices"
            text: qsTr("Blocked devices") + " (" + root.managedHost.blockedClients.length + ")"
            visible: !!root.selectedOwner && root.managedHost.blockedClients.length > 0
            onTriggered: { channelInfo.focusBlocked = true; channelInfo.open() }
        }
        MenuAction {
            objectName: "managePrivateMessages"
            text: qsTr("Messages")
            visible: root.privateChatsForHost(root.selectedHost).length > 0
            onTriggered: { privateMessages.hostId = root.selectedHost; privateMessages.hostDeviceId = root.privateDeviceForHost(root.selectedHost); privateMessages.open() }
        }
        MenuAction {
            objectName: "shareScreen"
            visible: !!root.screen && !root.remote && !!root.selectedOwner
            readonly property bool sharingHere: root.screen && root.screen.active && root.screen.hostId === root.selectedHost
            text: sharingHere ? qsTr("Stop sharing") : qsTr("Share screen")
            destructive: sharingHere
            enabled: sharingHere || (root.selectedOwner && root.selectedOwner.hosting && root.screen && !root.screen.active && !root.screen.watching)
            onTriggered: {
                if (root.screen.active) root.screen.stop()
                else { root.screen.refreshSources(); screenPicker.open() }
            }
        }
        MenuAction {
            objectName: "screenAudioWhileSharing"
            text: root.screen && root.screen.computerAudio ? qsTr("Computer audio") : qsTr("App audio"); checkable: true
            visible: !root.remote && !!root.selectedOwner && root.screen && root.screen.active && root.screen.audioAvailable && root.screen.hostId === root.selectedHost
            checked: root.screen && root.screen.audioEnabled
            onTriggered: root.screen.setAudioEnabled(checked)
        }
        MenuAction { objectName: "browseStations"; text: qsTr("Browse stations"); enabled: !!root.hostRadio; visible: !root.remote && !!root.selectedOwner; onTriggered: radioBrowser.open() }
        MenuAction { objectName: "joinChannel"; text: qsTr("Join"); enabled: root.network.ready && (!root.activeHost || root.selectedHost !== root.activeHost); onTriggered: root.joinHost(root.selectedHost) }
        Item {
            id: channelMusic
            implicitWidth: 240; implicitHeight: visible ? 64 : 0
            readonly property string musicId: {
                const entry = root.channelList.find(function(h) { return h.id === root.selectedHost })
                return entry && entry.musicId ? entry.musicId : ""
            }
            property int revision: 0
            visible: !root.remote && musicId.length > 0
            Connections { target: audio; function onProfileChanged() { ++channelMusic.revision } }
            RowLayout {
                anchors.fill: parent; anchors.margins: 12
                enabled: audio.outputAvailable
                SettingSlider {
                    objectName: "channelMusicVolume"
                    Layout.fillWidth: true
                    caption: qsTranslate("AudioSettings", "Music volume")
                    from: 0; to: 1; stepSize: 0.01
                    value: { const revision = channelMusic.revision; return audio.channelMusicVolume(channelMusic.musicId) }
                    valueText: qsTranslate("AudioSettings", "%1 %").arg(Math.round(value * 100))
                    applyValue: function(value) { return audio.setChannelMusicVolume(channelMusic.musicId, value) }
                }
                ActionButton {
                    objectName: "resetChannelMusicVolume"
                    iconOnly: true; flat: true; destructive: true; glyph: "reset"
                    text: qsTr("Use default music volume")
                    onClicked: audio.resetChannelMusicVolume(channelMusic.musicId)
                }
            }
        }
        Item {
            id: channelTranslation
            objectName: "channelTranslationControls"
            implicitHeight: visible ? translationRows.implicitHeight + 20 : 0
            visible: root.language.supported && !root.remote
            readonly property var preferences: { root.language.revision; return root.language.channelPreferences(root.selectedHost) }
            function save(key, value) {
                const next = Object.assign({}, preferences)
                next[key] = value
                return root.language.configureChannel(root.selectedHost, !!next.messages, next.outgoing || "", !!next.voice, next.voiceTarget || "")
            }
            ColumnLayout {
                id: translationRows
                anchors.fill: parent; anchors.margins: 10; spacing: 6
                Repeater {
                    model: [
                        { key: "messages", target: "outgoing", label: qsTranslate("LocalLanguage", "Translate messages"), choice: qsTranslate("LocalLanguage", "Original text") },
                        { key: "voice", target: "voiceTarget", label: qsTranslate("LocalLanguage", "Translated voice"), choice: qsTr("App language") }
                    ]
                    RowLayout {
                        id: translationRow
                        required property var modelData
                        Layout.fillWidth: true; spacing: 8
                        OptionCheck {
                            objectName: translationRow.modelData.key === "messages" ? "channelTranslateMessages" : "channelTranslateVoice"
                            Layout.fillWidth: true
                            text: translationRow.modelData.label
                            checked: !!channelTranslation.preferences[translationRow.modelData.key]
                            enabled: root.language.available && (translationRow.modelData.key === "messages" || root.language.speechAvailable)
                            onClicked: { channelTranslation.save(translationRow.modelData.key, checked); checked = Qt.binding(function() { return !!channelTranslation.preferences[translationRow.modelData.key] }) }
                            ToolTip.visible: hovered
                            ToolTip.text: enabled ? qsTranslate("LocalLanguage", "Translation (experimental)") : qsTranslate("LocalLanguage", "Local language processing is unavailable.")
                            ToolTip.delay: 600
                        }
                        ChoiceBox {
                            objectName: translationRow.modelData.key === "messages" ? "channelOutgoingLanguage" : "channelVoiceLanguage"
                            Layout.preferredWidth: 150
                            model: [{id: "", name: translationRow.modelData.choice}].concat(root.language.languages)
                            textRole: "name"; valueRole: "id"
                            currentIndex: Math.max(0, model.findIndex(function(item) { return item.id === (channelTranslation.preferences[translationRow.modelData.target] || "").split("-")[0] }))
                            enabled: root.language.available && !!channelTranslation.preferences[translationRow.modelData.key]
                            Accessible.name: translationRow.modelData.key === "messages" ? qsTr("Send in") : qsTr("Hear in")
                            ToolTip.visible: hovered; ToolTip.text: Accessible.name; ToolTip.delay: 600
                            onActivated: channelTranslation.save(translationRow.modelData.target, currentValue)
                        }
                    }
                }
            }
        }
        MenuAction {
            objectName: "autoJoinMenuItem"
            text: qsTr("Auto-join"); checkable: true; visible: !root.remote
            checked: root.channelList.some(function(h) { return h.id === root.selectedHost && h.autoJoin })
            onTriggered: {
                const previous = root.channelList.some(function(h) { return h.id === root.selectedHost && h.autoJoin })
                root.network.setAutoJoin(root.selectedHost, !previous)
                checked = Qt.binding(function() { return root.channelList.some(function(h) { return h.id === root.selectedHost && h.autoJoin }) })
            }
        }
        MenuAction { objectName: "leaveMenuItem"; destructive: true; text: qsTr("Leave"); enabled: root.activeHost === root.selectedHost; onTriggered: root.leaveHost() }
        MenuSeparator { visible: !root.remote && root.selectedHost !== root.network.ownId; height: visible ? implicitHeight : 0 }
        MenuAction { objectName: "removeChannelMenuItem"; destructive: true; text: root.channelList.some(function(c) { return c.id === root.selectedHost && c.protocol === "teamspeak" }) ? qsTr("Remove server") : qsTr("Remove channel"); visible: !root.remote && root.selectedHost !== root.network.ownId; onTriggered: root.selectedOwner ? removeOwnChannel.open() : root.network.removeChannel(root.selectedHost) }
    }
    Menu {
        popupType: Popup.Item
        background: Rectangle { implicitWidth: 240; implicitHeight: 40; radius: Theme.controlRadius; color: Theme.surface; border.color: Theme.border }
        id: memberMenu
        MenuAction { objectName: "inspectMember"; text: qsTr("Inspect"); visible: !requestInfo.visible; onTriggered: requestInfo.open() }
        MenuAction {
            objectName: "allowRemoteControl"
            text: qsTr("Allow remote control")
            checkable: true
            visible: root.canGrantMemberControl
            checked: root.network.controllers.some(function(p) { return p.id === root.selectedMember.id && p.remote })
            onTriggered: {
                const previous = root.network.controllers.some(function(p) { return p.id === root.selectedMember.id && p.remote })
                if (!root.network.setRemotePermission(root.selectedMember.id, !previous)) root.showNotice(root.network.controlStatus)
                checked = Qt.binding(function() { return root.network.controllers.some(function(p) { return p.id === root.selectedMember.id && p.remote }) })
            }
        }
        MenuAction { text: qsTr("Stop radio"); visible: root.canStopMemberRadio; onTriggered: { const player = radio.forChannel(root.selectedMemberHost); if (player) player.stop() } }
        MenuAction { destructive: true; text: qsTr("Kick"); visible: root.canModerateMember; onTriggered: root.memberHost.kick(root.selectedMember.id) }
        MenuAction { destructive: true; text: qsTr("Block"); visible: root.canModerateMember; onTriggered: root.memberHost.setBlocked(root.selectedMember.id, true) }
    }
    PanelDialog {
        id: originalTranscript
        parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(380, parent.width - 24)
        title: root.selectedMember.name || ""
        buttons: Dialog.Close
        contentItem: TextEdit {
            text: { root.language.revision; const info = root.language.member(root.selectedMember.id); return [info.status, info.transcript].filter(Boolean).join("\n\n") }
            textFormat: TextEdit.PlainText; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
            color: Theme.text; font.pixelSize: 12
        }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: root.height < 460 ? 8 : 12; spacing: root.height < 460 ? 4 : 8
        RowLayout {
            visible: root.searchOpen
            Layout.fillWidth: true; spacing: 4
            EntryField {
                id: channelSearch; objectName: "channelSearch"
                Layout.fillWidth: true
                placeholderText: qsTr("Search channels")
                Accessible.name: placeholderText
                selectByMouse: true
                onTextChanged: root.updateChannelRows()
                Keys.onEscapePressed: root.searchOpen = false
            }
            ActionButton {
                text: qsTr("Close"); glyph: "close"; iconOnly: true; flat: true
                onClicked: root.searchOpen = false
            }
        }
        Rectangle {
            objectName: "remoteContext"
            visible: root.remote
            Layout.fillWidth: true
            implicitHeight: remoteIdentity.implicitHeight + 16
            radius: Theme.controlRadius
            color: Theme.raised
            border.color: Theme.accent
            RowLayout {
                id: remoteIdentity
                anchors.fill: parent; anchors.margins: 8; spacing: 8
                VoiceAvatar {
                    objectName: "remoteContextAvatar"
                    Layout.preferredWidth: 36; Layout.preferredHeight: 36
                    name: root.current.name || root.network.controlTargetName
                    avatar: root.current.avatarId || root.current.avatar || "mossling"
                    online: root.network.controlConnected
                    animated: false; circular: true
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 1
                    Label {
                        objectName: "remoteContextTitle"
                        text: qsTr("Remote control"); color: Theme.accent; font.pixelSize: 11
                    }
                    Label {
                        objectName: "remoteTargetName"
                        Layout.fillWidth: true; Layout.minimumWidth: 0
                        text: root.current.name || root.network.controlTargetName
                        textFormat: Text.PlainText; elide: Text.ElideRight; font.bold: true
                    }
                }
            }
        }
        ScrollView {
            id: scroll
            objectName: "channelsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: scroll.availableWidth; spacing: 2
                Repeater {
                    id: channelRepeater
                    model: channelRows
                    delegate: ColumnLayout {
                        id: channelEntry
                        required property var channelData
                        readonly property var modelData: channelData
                        required property string groupLabel
                        required property string groupId
                        required property bool groupUnread
                        required property string groupProtocol
                        required property bool groupExpanded
                        readonly property real headingHeight: hostHeading.visible ? hostHeading.implicitHeight + hostHeading.Layout.topMargin + hostHeading.Layout.bottomMargin : 0
                        Layout.fillWidth: true; spacing: 2
                        RowLayout {
                            id: hostHeading
                            visible: channelEntry.groupLabel.length > 0
                            Layout.fillWidth: true; Layout.topMargin: 4; Layout.bottomMargin: 2
                            ActionButton {
                                objectName: "toggleHost_" + channelEntry.groupId
                                text: channelEntry.groupLabel; iconOnly: true; flat: true
                                enabled: channelSearch.text.trim().length === 0
                                Accessible.role: Accessible.Button
                                Accessible.checkable: true
                                Accessible.checked: channelEntry.groupExpanded
                                contentItem: Glyph { symbol: "down"; rotation: channelEntry.groupExpanded ? 0 : -90; color: Theme.muted }
                                onClicked: root.toggleHost(channelEntry.groupId)
                                Keys.onLeftPressed: if (channelEntry.groupExpanded) root.toggleHost(channelEntry.groupId)
                                Keys.onRightPressed: if (!channelEntry.groupExpanded) root.toggleHost(channelEntry.groupId)
                            }
                            ProviderIcon { objectName: "hostProvider_" + channelEntry.groupId; provider: channelEntry.groupProtocol }
                            DetailText {
                                objectName: "hostGroup_" + channelEntry.groupId
                                text: channelEntry.groupLabel; color: Theme.muted; font.pixelSize: 11
                                selectByMouse: true; wrapMode: TextEdit.WrapAnywhere
                            }
                            Glyph { objectName: "activeHost_" + channelEntry.groupId; symbol: "headphones"; color: Theme.accent; visible: !!root.current.joined && root.channelList.some(function(entry) { return entry.id === root.activeHost && (entry.deviceId || entry.id) === channelEntry.groupId }); description: qsTr("Connected to voice") }
                            UnreadDot { objectName: "unreadHost_" + channelEntry.groupId; visible: channelEntry.groupUnread }
                        }
                        Rectangle {
                            id: channelRow
                            visible: channelEntry.groupExpanded
                            readonly property var modelData: channelEntry.channelData
                            readonly property bool privateFolder: !!modelData.privateFolder
                            readonly property bool privateConversation: !!modelData.private
                            readonly property bool active: root.activeHost === modelData.id && !!root.current.joined
                            readonly property bool owned: !privateFolder && !privateConversation && (!!modelData.owned || modelData.id === (root.current.ownId || root.network.ownId))
                            readonly property bool selected: visible && root.inspectedHost === modelData.id && root.chatExpanded
                            readonly property var members: modelData.members || []
                            Layout.fillWidth: true; implicitHeight: (privateFolder ? 34 : 42) + (selected && !privateFolder ? chatSlot.height + 8 : 0)
                            radius: Theme.controlRadius; color: active ? Theme.surface : channelHover.containsMouse ? Theme.raised : "transparent"
                            border.width: channelRow.active || activeFocus ? 1 : 0; border.color: Theme.accent
                            MouseArea {
                                id: channelHover
                                objectName: "channelRow_" + modelData.id
                                anchors.left: parent.left; anchors.right: parent.right; height: channelRow.privateFolder ? 34 : 42
                                hoverEnabled: true; acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onDoubleClicked: function(mouse) {
                                    if (mouse.button !== Qt.LeftButton) return
                                    if (channelRow.privateFolder) root.togglePrivateFolder(modelData.hostId)
                                    else { root.chatExpanded = true; root.joinHost(modelData.id) }
                                }
                                onClicked: function(mouse) {
                                    if (channelRow.privateFolder) {
                                        if (mouse.button === Qt.LeftButton) root.togglePrivateFolder(modelData.hostId)
                                        return
                                    }
                                    if (mouse.button === Qt.RightButton && channelRow.privateConversation) { privateMessages.hostId = root.privateHostId(modelData); privateMessages.hostDeviceId = modelData.deviceId; privateMessages.open() }
                                    else if (mouse.button === Qt.RightButton) { root.selectedHost = modelData.id; channelMenu.popup() }
                                    else if (mouse.button === Qt.LeftButton) root.showHostChat(modelData.id)
                                }
                            }
                            RowLayout {
                                anchors.left: parent.left; anchors.right: parent.right; height: channelRow.privateFolder ? 34 : 42
                                anchors.leftMargin: channelRow.privateConversation ? 18 : 8 + (modelData.depth || 0) * 12; anchors.rightMargin: 2; spacing: 6
                                Glyph { symbol: channelRow.privateFolder ? "down" : channelRow.privateConversation ? "chat" : "down"; rotation: channelRow.privateFolder ? (root.privateFolderIsExpanded(modelData.hostId) ? 0 : -90) : channelRow.selected ? 0 : -90; color: channelRow.selected || channelRow.privateFolder ? Theme.accent : Theme.muted }
                                Glyph { objectName: "ownedChannel_" + modelData.id; visible: channelRow.owned; symbol: "home"; description: qsTr("You host"); color: Theme.accent }
                                Label { objectName: "channelName_" + modelData.id; text: modelData.name; textFormat: Text.PlainText; font.weight: channelRow.selected || channelRow.owned || channelRow.active || channelRow.privateFolder ? Font.DemiBold : Font.Normal; Layout.fillWidth: true; elide: Text.ElideRight }
                                UnreadDot { objectName: "unreadChannel_" + modelData.id; visible: !!modelData.unread }
                                Label { visible: channelRow.privateConversation; text: modelData.peerOnline ? qsTr("Online") : qsTr("Offline"); color: Theme.muted; font.pixelSize: 11 }
                                Glyph { objectName: "joinedChannel_" + modelData.id; visible: channelRow.active; symbol: "headphones"; description: qsTr("Connected to voice"); color: Theme.accent }
                                Glyph { objectName: "channelMusic_" + modelData.id; visible: !channelRow.privateConversation && !!modelData.music; symbol: "music"; description: qsTr("Radio"); color: Theme.accent }
                                Label {
                                    visible: !channelRow.privateConversation && !channelRow.privateFolder && !modelData.online
                                    text: root.accessLabel(modelData.access)
                                    font.pixelSize: 11; color: Theme.muted
                                    Layout.maximumWidth: 75; elide: Text.ElideRight
                                }
                                Row {
                                    objectName: "memberSummary_" + modelData.id
                                    visible: !channelRow.privateFolder && !channelRow.privateConversation && !!modelData.online && channelRow.members.length > 0
                                    spacing: -5
                                    activeFocusOnTab: true
                                    Accessible.role: Accessible.Button
                                    Accessible.name: qsTr("Show all %1 members").arg(channelRow.members.length)
                                    Accessible.onPressAction: root.toggleMembers(channelRow.modelData.id)
                                    Keys.onReturnPressed: root.toggleMembers(channelRow.modelData.id)
                                    Keys.onSpacePressed: root.toggleMembers(channelRow.modelData.id)
                                    Repeater {
                                        model: channelRow.members.slice(0, 3)
                                        VoiceAvatar {
                                            id: memberAvatar
                                            required property var modelData
                                            width: 36; height: 36; circular: true
                                            avatar: modelData.avatarId || modelData.avatar || "mossling"; name: modelData.name; music: !!modelData.music; teamSpeak: !!modelData.teamSpeak
                                            available: modelData.available; muted: modelData.muted; deafened: !!modelData.deafened; sleeping: !!modelData.sleeping
                                            identity: modelData.id; animationTime: root.avatarTime
                                            objectName: "channelAvatar_" + channelRow.modelData.id + "_" + modelData.id
                                            activeFocusOnTab: true
                                            Accessible.role: Accessible.Button
                                            Accessible.onPressAction: root.memberOptions(modelData, channelRow.modelData.id)
                                            Keys.onReturnPressed: root.memberOptions(modelData, channelRow.modelData.id)
                                            Keys.onSpacePressed: root.memberOptions(modelData, channelRow.modelData.id)
                                            MouseArea { anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: mouse => root.memberOptions(modelData, channelRow.modelData.id, mouse.button === Qt.RightButton) }
                                            animated: root.voice.animatedAvatars
                                            level: channelRow.active ? root.memberLevel(modelData.id) : 0
                                            TextToolTip { objectName: "memberTooltip"; parent: memberAvatar; text: memberAvatar.name; visible: avatarHover.hovered }
                                            HoverHandler { id: avatarHover }
                                        }
                                    }
                                }
                                ActionButton {
                                    objectName: "showMembers_" + modelData.id
                                    visible: !channelRow.privateFolder && !channelRow.privateConversation && !!modelData.online && channelRow.members.length > 3
                                    text: "+" + (channelRow.members.length - 3)
                                    flat: true
                                    Accessible.name: qsTr("Show all %1 members").arg(channelRow.members.length)
                                    onClicked: root.toggleMembers(modelData.id)
                                }
                                ActionButton {
                                    text: qsTr("Channel options"); glyph: "more"; iconOnly: true; flat: true
                                    visible: !channelRow.privateFolder && !channelRow.privateConversation
                                    onClicked: { root.selectedHost = modelData.id; channelMenu.popup() }
                                }
                            }
                            Item {
                                id: chatSlot
                                objectName: "conversationSlot_" + channelRow.modelData.id
                                anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.topMargin: 42
                                height: {
                                    const count = Math.min(3, channelRepeater.count)
                                    let headings = 0
                                    for (let i = 0; i < count; ++i) {
                                        const entry = channelRepeater.itemAt(i)
                                        if (entry) headings += entry.headingHeight
                                    }
                                    return Math.max(root.compactInput ? 0 : 128, conversation.implicitHeight, scroll.height - count * 44 - headings - 12)
                                }
                                visible: channelRow.selected && !channelRow.privateFolder
                                function selectSlot() {
                                    if (channelRow.selected) {
                                        root.conversationSlot = chatSlot
                                        Qt.callLater(function() {
                                            if (!channelRow || !channelRow.selected || !scroll) return
                                            const flick = scroll.contentItem
                                            const top = channelRow.mapToItem(flick.contentItem, 0, 0).y
                                            const bottom = top + channelRow.height
                                            if (top < flick.contentY) flick.contentY = top
                                            else if (bottom > flick.contentY + flick.height)
                                                flick.contentY = Math.max(0, bottom - flick.height)
                                        })
                                    }
                                    else if (root.conversationSlot === chatSlot) root.conversationSlot = null
                                }
                                Component.onCompleted: selectSlot()
                                Component.onDestruction: if (root.conversationSlot === chatSlot) root.conversationSlot = null
                                Connections { target: channelRow; function onSelectedChanged() { chatSlot.selectSlot() } }
                            }
                            activeFocusOnTab: true
                            Accessible.role: Accessible.Button
                            Accessible.name: modelData.name + (channelRow.owned ? ", " + qsTr("You host") : "") + (channelRow.active ? ", " + qsTr("Connected to voice") : "")
                            Accessible.onPressAction: root.showHostChat(modelData.id)
                            Keys.onReturnPressed: root.showHostChat(modelData.id)
                            Keys.onSpacePressed: root.showHostChat(modelData.id)
                        }
                    }
                }
                Label { visible: root.channelList.length === 0; text: root.remote ? root.network.controlStatus : qsTr("No channels yet"); Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap }
                Label { objectName: "emptyChannelSearch"; visible: channelSearch.text.trim().length > 0 && channelRows.count === 0; text: qsTr("No matches"); Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap }
            }
        }
    }
    ColumnLayout {
        id: conversation
        parent: root.conversationSlot || root.contentItem
        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8
        visible: root.showChat && root.conversationSlot !== null
        spacing: 4
        RowLayout {
            visible: !root.compactInput
            Layout.fillWidth: true; spacing: 6
            Item { Layout.fillWidth: true }
            Label {
                visible: !!root.current.chatReady && root.current.chatLifetimeDays > 0
                text: root.current.chatLifetimeDays === 1 ? qsTr("24 h") : qsTr("%1 d").arg(root.current.chatLifetimeDays || 1)
                color: Theme.muted; font.pixelSize: 11
                ToolTip.text: qsTr("New message lifetime")
                ToolTip.visible: expiryHover.hovered
                HoverHandler { id: expiryHover }
            }
        }
        Loader {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? (root.height < 460 ? 48 : 112) : 0
            active: !!root.screen
            visible: !root.compactInput && !root.remote && (!!root.network.screenView.available || (root.screen && root.screen.active && root.screen.hostId === root.inspectedHost))
            sourceComponent: ScreenPanel { share: root.screen; network: root.network; hostId: root.inspectedHost }
        }
        Label {
            visible: root.screen && root.screen.error.length > 0
            text: root.screen ? root.screen.error : ""; textFormat: Text.PlainText; color: Theme.danger
            Layout.fillWidth: true; wrapMode: Text.Wrap
        }
        ChatPanel {
            id: chat
            objectName: "chatPanel"
            network: root.network; animatedAvatars: root.voice.animatedAvatars
            animationTime: root.avatarTime
            memberLevel: function(id) { return root.inspectedHost === root.activeHost ? root.memberLevel(id) : 0 }
            onMemberRequested: (member, contextual) => root.memberOptions(member, root.inspectedHost, contextual)
            active: root.visible && root.showChat
            readable: active && root.active && !settings.visible
            readViewport: scroll
            Layout.fillWidth: true; Layout.fillHeight: true
        }
    }
    footer: Rectangle {
        visible: !root.mobileKeyboard
        implicitHeight: visible ? controls.implicitHeight + 16 : 0
        color: Theme.surface
        ColumnLayout {
            id: controls
            anchors.fill: parent; anchors.margins: 8; spacing: 4
            RowLayout {
                objectName: "pendingRequest"
                visible: !root.remote && !!root.incomingRequest.id
                Layout.fillWidth: true; spacing: 6
                Label { text: root.network.ownedChannels.length > 1 ? qsTr("%1 wants to join %2").arg(root.incomingRequest.name || "").arg(root.incomingRequest.hostName || "") : qsTr("%1 wants to join").arg(root.incomingRequest.name || ""); textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.pixelSize: 12 }
                ActionButton { objectName: "inspectRequest"; text: qsTr("Inspect"); font.pixelSize: 12; onClicked: { root.inspectedRequestId = root.incomingRequest.id; root.inspectedRequestHost = root.incomingRequest.hostId; requestInfo.open() } }
                ActionButton { objectName: "approveRequest"; text: qsTr("Approve"); primary: true; font.pixelSize: 12; property var pressedRequest: ({}); onPressed: pressedRequest = root.incomingRequest; onCanceled: pressedRequest = ({}); onClicked: { const request = pressedRequest.id ? pressedRequest : root.incomingRequest; if (root.network.requests.some(function(r) { return r.id === request.id && r.hostId === request.hostId })) root.decideRequest(request.id, request.hostId, true); pressedRequest = ({}) } }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 4
                ActionButton { objectName: "addChannel"; visible: !root.remote; text: qsTr("Add channel"); glyph: "plus"; iconOnly: true; flat: true; onClicked: discover.open() }
                ActionButton { objectName: "openSettings"; visible: !root.remote; text: qsTr("Settings"); glyph: "settings"; iconOnly: true; flat: true; onClicked: root.openSettings(0) }
                ActionButton { objectName: "searchChannels"; text: qsTr("Search channels"); glyph: "search"; iconOnly: true; flat: true; checked: root.searchOpen; onClicked: root.searchOpen = !root.searchOpen }
                Item { Layout.fillWidth: true }
                ActionButton { objectName: "mute"; text: root.micMuted ? qsTr("Unmute microphone") : qsTr("Mute microphone"); glyph: root.micMuted || (!root.remote && !root.voice.available) ? "mute" : "mic"; iconOnly: true; flat: true; checked: root.micMuted; enabled: !root.remote || root.network.remoteAllowed; onClicked: root.remote ? root.network.remoteAction("mute", {value: !root.micMuted}) : root.voice.setMuted(!root.micMuted) }
                ActionButton { objectName: "deafen"; text: root.outputMuted ? qsTr("Enable speakers") : qsTr("Mute speakers"); glyph: root.outputMuted ? "deafen" : "speaker"; iconOnly: true; flat: true; checked: root.outputMuted; enabled: !root.remote || root.network.remoteAllowed; onClicked: root.remote ? root.network.remoteAction("deafen", {value: !root.outputMuted}) : root.voice.setDeafened(!root.outputMuted) }
                ActionButton {
                    objectName: "remoteToggle"
                    visible: root.remote || root.network.remoteOffers.length > 0
                    text: root.remote ? qsTr("Return to this device") + "\n" + (root.current.name || root.network.controlTargetName)
                        : qsTr("Remote control") + (root.network.remoteOffers.length === 1 ? "\n" + root.network.remoteOffers[0].name : "")
                    glyph: "remote"; iconOnly: true; flat: true; checked: root.remote
                    onClicked: {
                        if (root.remote) root.network.setRemoteMode(false)
                        else if (root.network.remoteOffers.length === 1) root.controlDevice(root.network.remoteOffers[0].id)
                        else remoteTargets.open()
                    }
                }
            }
            ActionButton {
                objectName: "pttButton"
                Layout.fillWidth: true
                visible: root.touchControls && (root.voice.pushToTalk || root.remote)
                text: root.voice.pttButtonHeld ? qsTr("Transmitting key held") : qsTr("Hold to talk")
                glyph: "mic"; checked: root.voice.pttButtonHeld
                onPressed: root.voice.setPttButtonHeld(true)
                onReleased: root.voice.setPttButtonHeld(false)
                onCanceled: root.voice.setPttButtonHeld(false)
            }
            RowLayout {
                Layout.fillWidth: true
                visible: statusMessage.text.length > 0 || retryIdentity.visible
                Label {
                    id: statusMessage
                    objectName: "channelStatus"
                    Layout.fillWidth: true
                    text: (!root.network.ready ? root.network.status : "") || root.voice.error || root.notice || (root.remote && !root.network.controlConnected ? root.network.controlStatus : "")
                    textFormat: Text.PlainText; font.pixelSize: 11; color: root.voice.error ? Theme.danger : Theme.muted
                    maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight
                    TextToolTip { parent: statusMessage; text: statusMessage.text; visible: statusHover.hovered }
                    HoverHandler { id: statusHover }
                }
                ActionButton {
                    id: retryIdentity
                    objectName: "retryIdentity"
                    text: qsTr("Retry")
                    visible: !root.network.ready
                    enabled: !root.network.initializing
                    onClicked: root.network.initialize()
                }
            }
        }
    }
}
