import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import "AvatarAtlas.js" as Atlas

PanelWindow {
    id: root
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
    property var voice: session
    property var supporter: typeof supporterLicense !== "undefined" ? supporterLicense : null
    property var updates: typeof appUpdates !== "undefined" ? appUpdates : null
    function selectAvatar(id) {
        if (voice.avatars.indexOf(id) < 10 || voice.supporterEnabled) voice.setAvatar(id)
        else if (supporter && supporter.directDistribution) settingsPage = 3
    }
    property var screen: typeof screenShare !== "undefined" ? screenShare : null
    property var network: channel
    readonly property bool remote: network.remoteMode
    readonly property var current: remote ? network.remoteView : network
    readonly property var channelList: remote ? (current.channels || []) : network.savedChannels
    readonly property var members: current.participants || []
    readonly property var chatMembers: current.chatMembers || []
    readonly property string inspectedHost: current.chatHostId || ""
    property bool chatExpanded: true
    property Item conversationSlot: null
    property double avatarTime: Date.now()
    Timer { interval: Atlas.frameInterval(); running: root.visible && root.voice.animatedAvatars; repeat: true; onTriggered: root.avatarTime = Date.now() }
    readonly property bool showChat: chatExpanded && inspectedHost.length > 0
    onInspectedHostChanged: chatExpanded = true
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
    readonly property var selectedOwner: { const revision = network.ownedChannels; return remote ? null : network.ownChannel(selectedHost) }
    readonly property var managedHost: selectedOwner || network
    readonly property var memberHost: { const revision = network.ownedChannels; return remote ? null : network.ownChannel(selectedMemberHost) }
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
    ListModel { id: channelRows; dynamicRoles: true }
    function updateChannelRows() {
        for (let i = channelRows.count - 1; i >= 0; --i)
            if (!channelList.some(function(entry) { return entry.id === channelRows.get(i).channelData.id })) channelRows.remove(i)
        for (let i = 0; i < channelList.length; ++i) {
            let found = -1
            for (let j = i; j < channelRows.count; ++j) if (channelRows.get(j).channelData.id === channelList[i].id) { found = j; break }
            if (found < 0) channelRows.insert(i, {channelData: channelList[i]})
            else {
                if (found !== i) channelRows.move(found, i, 1)
                channelRows.setProperty(i, "channelData", channelList[i])
            }
        }
    }
    onChannelListChanged: updateChannelRows()
    Component.onCompleted: updateChannelRows()
    Binding { target: Theme; property: "mode"; value: session.theme }
    Binding { target: Theme; property: "paletteName"; value: session.palette }
    property alias recordingMode: audioSettingsPanel.recordingMode
    signal previewRequested(bool enabled)
    readonly property bool audioSettingsOpen: visible && settings.visible && settingsPage >= 1 && settingsPage <= 2
    function syncAudioSettings() {
        const active = audioSettingsOpen
        const preview = active && settingsPage === 1 && !recordingMode
        if (!preview) audio.stop()
        voice.setAudioSettingsOpen(active)
        previewRequested(preview)
    }
    onAudioSettingsOpenChanged: syncAudioSettings()
    onRecordingModeChanged: syncAudioSettings()
    onSettingsPageChanged: {
        if (settings.visible) audio.stop()
        if (settingsPage !== 1) recordingMode = false
        if (settings.visible && settingsPage === 3) pickAboutSentence()
        syncAudioSettings()
    }
    onClosing: settings.close()
    width: 460
    height: 560
    minimumWidth: 360
    minimumHeight: 360
    visible: false
    title: "SquadSpeak"
    function joinHost(id) {
        const entry = channelList.find(function(host) { return host.id === id })
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
    function showNotice(text) {
        if (text === lastNotice) return
        lastNotice = text; notice = text; noticeTimer.restart()
    }
    Timer { id: noticeTimer; interval: 4000; onTriggered: root.notice = "" }
    Timer {
        id: channelClick
        property string hostId: ""
        interval: Qt.styleHints.mouseDoubleClickInterval
        onTriggered: root.showHostChat(hostId)
    }
    function openHostChat(id) { chatExpanded = true; return remote ? network.remoteAction("openChat", {hostId: id}) : network.openSavedChat(id) }
    function showHostChat(id) {
        if (id === inspectedHost && chatExpanded) chatExpanded = false
        else openHostChat(id)
    }
    function memberLevel(id) { return remote ? (network.remoteLevels[id] || 0) : id === network.ownId ? audio.transmitLevel : (audio.playbackLevels[id] || 0) }
    function toggleMembers(id) { return openHostChat(id) }
    function memberOptions(member, host) { selectedMember = member; selectedMemberHost = host; memberMenu.popup() }
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
        id: joinPassword
        objectName: "joinPasswordDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(360, parent.width - 24)
        modal: true
        title: qsTr("Channel password")
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
                Accessible.name: qsTr("Channel password")
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
            if (root.network.passwordRequired && !joinPassword.opened) joinPassword.open()
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
            Label {
                objectName: "discoveryStatus"
                visible: root.network.discoverySearching || !!root.network.discoveryError || discoveryList.count === 0
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
        onClosed: { audio.stop(); root.recordingMode = false }
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
                            spacing: 12
                            Image { objectName: "aboutLogo"; source: "icons/app.png"; Layout.preferredWidth: 52; Layout.preferredHeight: 52; fillMode: Image.PreserveAspectFit; sourceSize.width: 104; sourceSize.height: 104 }
                            ColumnLayout {
                                spacing: 2
                                Label { text: "SquadSpeak"; font.pixelSize: 20; font.weight: Font.DemiBold }
                                Label { objectName: "aboutVersion"; text: qsTr("Version %1").arg(Qt.application.version); color: Theme.muted }
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
                                    : qsTr("Free version")
                            }
                            RowLayout {
                                visible: root.supporter !== null && root.supporter.configured
                                Layout.fillWidth: true
                                EntryField { id: licenseKey; objectName: "licenseKey"; Layout.fillWidth: true; placeholderText: qsTr("License key"); Accessible.name: placeholderText; echoMode: TextInput.Password; maximumLength: 256; enabled: !root.supporter.busy && !root.supporter.pending }
                                ActionButton { objectName: "activateLicense"; text: qsTr("Activate"); enabled: licenseKey.enabled && licenseKey.text.trim().length > 0; onClicked: { root.supporter.activate(licenseKey.text); licenseKey.clear() } }
                            }
                            GridLayout {
                                visible: root.supporter !== null && root.supporter.configured
                                Layout.fillWidth: true; columns: 2
                                ActionButton { objectName: "buyLicense"; Layout.columnSpan: 2; Layout.fillWidth: true; text: qsTr("Buy annual pass"); visible: root.supporter !== null && root.supporter.purchaseUrl.toString().length > 0; onClicked: Qt.openUrlExternally(root.supporter.purchaseUrl) }
                                ActionButton { objectName: "checkLicense"; Layout.fillWidth: true; text: qsTr("Check now"); enabled: root.supporter !== null && !root.supporter.busy; onClicked: root.supporter.refresh() }
                                ActionButton { objectName: "deactivateLicense"; Layout.fillWidth: true; text: qsTr("Deactivate"); destructive: true; enabled: root.supporter !== null && !root.supporter.busy && root.supporter.supportReference.length > 0; onClicked: root.supporter.deactivate() }
                            }
                            DetailText { objectName: "licenseSupportReference"; text: root.supporter ? root.supporter.supportReference : ""; visible: text.length > 0; Accessible.name: qsTr("Support reference") }
                            Label { text: root.supporter ? root.supporter.status : ""; visible: text.length > 0; color: Theme.muted; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            ActionButton { text: qsTr("Resolve activation"); visible: root.supporter !== null && root.supporter.recoveryNeeded; enabled: !root.supporter || !root.supporter.busy; onClicked: resolveActivation.open() }
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
                        PathView {
                            id: avatarShelf
                            objectName: "avatarShelf"
                            Layout.fillWidth: true; Layout.preferredHeight: 120
                            clip: true; dragMargin: height
                            model: root.voice.avatars
                            currentIndex: model.indexOf(root.voice.avatar)
                            pathItemCount: Math.ceil(width / 100) + 1
                            cacheItemCount: 2
                            preferredHighlightBegin: 0.5; preferredHighlightEnd: 0.5
                            snapMode: PathView.SnapToItem; highlightMoveDuration: 120
                            activeFocusOnTab: true
                            Keys.onLeftPressed: decrementCurrentIndex()
                            Keys.onRightPressed: incrementCurrentIndex()
                            Keys.onSpacePressed: root.selectAvatar(model[currentIndex])
                            Keys.onReturnPressed: root.selectAvatar(model[currentIndex])
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
                                readonly property bool unlocked: index < 10 || root.voice.supporterEnabled
                                objectName: "avatarChoice_" + modelData
                                width: 96; height: 110
                                readonly property string label: modelData.split("-").map(function(word) { return word.charAt(0).toUpperCase() + word.slice(1) }).join(" ")
                                VoiceAvatar { anchors.top: parent.top; anchors.horizontalCenter: parent.horizontalCenter; width: 88; height: 88; avatar: modelData; muted: false; animated: false; online: parent.unlocked; Accessible.ignored: true }
                                Glyph { anchors.right: parent.right; anchors.top: parent.top; width: 22; height: 22; symbol: "lock"; color: Theme.muted; visible: !parent.unlocked; Accessible.ignored: true }
                                Rectangle { anchors.horizontalCenter: parent.horizontalCenter; width: 88; height: 88; color: "transparent"; radius: Theme.panelRadius; border.color: root.voice.avatar === modelData || (avatarShelf.activeFocus && index === avatarShelf.currentIndex) ? Theme.accent : "transparent"; border.width: 2 }
                                Label { anchors.bottom: parent.bottom; width: parent.width; text: parent.label; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight }
                                TapHandler { onTapped: root.selectAvatar(modelData) }
                                Accessible.role: unlocked ? Accessible.RadioButton : Accessible.Button
                                Accessible.name: label
                                Accessible.description: unlocked ? "" : qsTranslate("VoiceSession", "This avatar requires Supporter.")
                                Accessible.checked: root.voice.avatar === modelData
                                Accessible.onPressAction: root.selectAvatar(modelData)
                                ToolTip.visible: portraitHover.hovered
                                ToolTip.text: unlocked ? label : label + " - " + Accessible.description
                                HoverHandler { id: portraitHover }
                            }
                        }
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
        id: resolveActivation
        parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(360, parent.width - 32); modal: true
        title: qsTr("Resolve activation")
        contentItem: ColumnLayout {
            Label { text: qsTr("Reset only after support has released the device slot."); wrapMode: Text.Wrap; Layout.fillWidth: true }
            RowLayout {
                ActionButton { text: qsTr("Cancel"); onClicked: resolveActivation.close() }
                ActionButton { text: qsTr("Reset"); destructive: true; onClicked: { root.supporter.resetActivation(); resolveActivation.close() } }
            }
        }
    }
    PanelDialog {
        id: remoteTargets
        objectName: "remoteTargetsDialog"
        property string error: ""
        onOpened: error = ""
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(400, parent.width - 24); height: Math.min(360, parent.height - 24)
        modal: true; title: qsTr("Control a device"); buttons: Dialog.Cancel
        contentItem: ScrollView {
            contentWidth: availableWidth; clip: true
            ColumnLayout {
                width: parent.width
                Repeater {
                    model: root.network.remoteOffers
                    delegate: ActionButton {
                        required property var modelData
                        objectName: "remoteTarget_" + modelData.id
                        text: modelData.name; glyph: "remote"; Layout.fillWidth: true; horizontalAlignment: Text.AlignLeft
                        onClicked: {
                            if (root.network.chooseRemoteOffer(modelData.id)) remoteTargets.close()
                            else remoteTargets.error = root.network.controlStatus
                        }
                    }
                }
                Label { visible: remoteTargets.error.length > 0; text: remoteTargets.error; textFormat: Text.PlainText; color: Theme.danger; Layout.fillWidth: true; wrapMode: Text.Wrap }
            }
        }
    }
    PanelDialog {
        id: channelInfo
        objectName: "channelInfoDialog"
        readonly property var entry: root.channelList.find(function(h) { return h.id === root.selectedHost }) || ({})
        readonly property bool manageable: !!root.selectedOwner
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(440, parent.width - 24)
        height: Math.min(manageable ? 620 : 300, parent.height - 24)
        modal: true; focus: true; title: qsTr("Channel info")
        buttons: Dialog.Close
        onOpened: { channelInfoScroll.contentItem.contentY = 0; internetHelp.visible = false }
        contentItem: ScrollView {
            id: channelInfoScroll
            objectName: "channelInfoScroll"
            contentWidth: availableWidth; clip: true
            ColumnLayout {
                width: channelInfoScroll.availableWidth
                spacing: 12
                DetailText { objectName: "channelDisplayName"; visible: !channelInfo.manageable; text: channelInfo.entry.name || ""; font.weight: Font.DemiBold }
                DetailText { objectName: "channelAddress"; text: root.endpoint(channelInfo.entry); color: Theme.muted; Accessible.name: qsTr("Address") }
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
                    Label { visible: (root.hostRadio ? root.hostRadio.error : radio.error).length > 0 && !stationEditor.visible; text: root.hostRadio ? root.hostRadio.error : radio.error; textFormat: Text.PlainText; color: Theme.danger; wrapMode: Text.Wrap; Layout.fillWidth: true }
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
                            ActionButton { objectName: "manageMember_" + modelData.id; text: qsTr("Manage member"); glyph: "more"; iconOnly: true; onClicked: root.memberOptions(modelData, root.managedHost.channelId) }
                        }
                    }
                    Label { visible: root.managedHost.blockedClients.length > 0; text: qsTr("Blocked devices"); color: Theme.muted }
                    Repeater {
                        model: root.managedHost.blockedClients
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label { text: modelData.name; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight }
                            ActionButton { text: qsTr("Unblock"); onClicked: root.managedHost.setBlocked(modelData.id, false) }
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
        width: Math.min(440, parent.width - 24); height: Math.min(390, parent.height - 24)
        modal: true; focus: true; title: root.inspectedRequestId ? qsTr("Join request") : qsTr("Inspect")
        readonly property var entry: root.inspectedRequestId ? root.network.requests.find(function(r) { return r.id === root.inspectedRequestId && (!root.inspectedRequestHost || r.hostId === root.inspectedRequestHost) }) || ({}) : root.selectedMember
        footer: RowLayout {
            spacing: 8
            ActionButton { objectName: "denyInspectedRequest"; text: qsTr("Deny"); destructive: true; visible: !!root.inspectedRequestId; Layout.leftMargin: 12; Layout.bottomMargin: 12; enabled: !!requestInfo.entry.id; onClicked: root.decideRequest(requestInfo.entry.id, requestInfo.entry.hostId, false) }
            Item { Layout.fillWidth: true }
            ActionButton { text: qsTr("Close"); Layout.bottomMargin: 12; onClicked: requestInfo.close() }
            ActionButton { objectName: "approveInspectedRequest"; text: qsTr("Approve"); primary: true; visible: !!root.inspectedRequestId; Layout.rightMargin: 12; Layout.bottomMargin: 12; enabled: !!requestInfo.entry.id; onClicked: root.decideRequest(requestInfo.entry.id, requestInfo.entry.hostId, true) }
        }
        contentItem: ScrollView {
            id: requestScroll
            contentWidth: availableWidth; clip: true
            ColumnLayout {
                width: requestScroll.availableWidth; spacing: 12
                RowLayout {
                    Layout.fillWidth: true
                    VoiceAvatar { objectName: "requestAvatar"; width: 56; height: 56; circular: true; animated: false; avatar: requestInfo.entry.avatarId || "mossling"; name: requestInfo.entry.name || "" }
                    DetailText { objectName: "requestName"; text: requestInfo.entry.name || ""; font.weight: Font.DemiBold }
                }
                Label { text: qsTr("Channel"); visible: !!requestInfo.entry.channel; color: Theme.muted }
                DetailText { objectName: "requestChannel"; visible: text.length > 0; text: requestInfo.entry.channel || "" }
                Label { text: qsTr("Address"); visible: !!requestInfo.entry.address; color: Theme.muted }
                DetailText { objectName: "requestAddress"; visible: text.length > 0; text: root.endpoint(requestInfo.entry) }
                Label { text: qsTr("Device ID"); color: Theme.muted }
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
        title: qsTr("Share screen")
        modal: true; anchors.centerIn: parent
        width: Math.min(root.width - 24, 480); height: Math.min(root.height - 32, 440)
        buttons: Dialog.Cancel
        contentItem: ColumnLayout {
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
                        onClicked: { if (root.screen.start(modelData.index, root.selectedHost)) screenPicker.close() }
                    }
                }
            }
        }
    }
    Menu {
        popupType: Popup.Item
        background: Rectangle { implicitWidth: 240; implicitHeight: 40; radius: Theme.controlRadius; color: Theme.surface; border.color: Theme.border }
        id: channelMenu
        MenuAction { text: qsTr("Channel info"); onTriggered: channelInfo.open() }
        MenuAction {
            objectName: "shareScreen"
            visible: !!root.screen && !root.remote && !!root.selectedOwner
            readonly property bool sharingHere: root.screen && root.screen.active && root.screen.hostId === root.selectedHost
            text: sharingHere ? qsTr("Stop sharing") : qsTr("Share screen")
            destructive: sharingHere
            enabled: sharingHere || (root.voice.supporterEnabled && root.selectedOwner && root.selectedOwner.hosting && root.screen && !root.screen.active && !root.screen.watching)
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
        MenuAction { text: qsTr("Join voice"); onTriggered: root.joinHost(root.selectedHost) }
        Item {
            implicitWidth: 240; implicitHeight: visible ? 64 : 0
            visible: !root.remote && root.selectedHost === root.activeHost
            SettingSlider {
                objectName: "channelMusicVolume"
                anchors.fill: parent; anchors.margins: 12
                caption: qsTranslate("AudioSettings", "Music volume")
                enabled: audio.outputAvailable
                from: 0; to: 1; stepSize: 0.01
                value: audio.musicVolume
                valueText: qsTranslate("AudioSettings", "%1 %").arg(Math.round(audio.musicVolume * 100))
                applyValue: function(value) { return audio.setMusicVolume(value) }
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
        MenuAction { objectName: "removeChannelMenuItem"; destructive: true; text: qsTr("Remove channel"); visible: !root.remote && root.selectedHost !== root.network.ownId; onTriggered: root.selectedOwner ? removeOwnChannel.open() : root.network.removeChannel(root.selectedHost) }
    }
    Menu {
        popupType: Popup.Item
        background: Rectangle { implicitWidth: 240; implicitHeight: 40; radius: Theme.controlRadius; color: Theme.surface; border.color: Theme.border }
        id: memberMenu
        MenuAction { objectName: "inspectMember"; text: qsTr("Inspect"); onTriggered: { root.inspectedRequestId = ""; requestInfo.open() } }
        MenuAction {
            objectName: "allowRemoteControl"
            text: qsTr("Allow remote control")
            checkable: true
            visible: !root.remote && !root.selectedMember.music && root.selectedMember.id !== root.network.ownId
                && ((root.memberHost ? root.memberHost.hostClients : []).some(function(p) { return p.id === root.selectedMember.id })
                    || root.network.controllers.some(function(p) { return p.id === root.selectedMember.id }))
            checked: root.network.controllers.some(function(p) { return p.id === root.selectedMember.id && p.remote })
            onTriggered: {
                const previous = root.network.controllers.some(function(p) { return p.id === root.selectedMember.id && p.remote })
                if (!root.network.setRemotePermission(root.selectedMember.id, !previous)) root.showNotice(root.network.controlStatus)
                checked = Qt.binding(function() { return root.network.controllers.some(function(p) { return p.id === root.selectedMember.id && p.remote }) })
            }
        }
        MenuAction { text: qsTr("Stop radio"); visible: !root.remote && !!root.selectedMember.music && !!root.memberHost; onTriggered: { const player = radio.forChannel(root.selectedMemberHost); if (player) player.stop() } }
        MenuAction { text: qsTr("Volume for me"); enabled: !root.remote && root.selectedMember.id !== root.network.ownId; onTriggered: memberVolume.open() }
        MenuAction { destructive: true; text: qsTr("Kick"); visible: !root.remote && !root.selectedMember.music && !!root.memberHost && (root.memberHost ? root.memberHost.hostClients : []).some(function(p) { return p.id === root.selectedMember.id && p.id !== root.network.ownId }); onTriggered: root.memberHost.kick(root.selectedMember.id) }
        MenuAction { destructive: true; text: qsTr("Block"); visible: !root.remote && !root.selectedMember.music && !!root.memberHost && (root.memberHost ? root.memberHost.hostClients : []).some(function(p) { return p.id === root.selectedMember.id && p.id !== root.network.ownId }); onTriggered: root.memberHost.setBlocked(root.selectedMember.id, true) }
    }
    PanelDialog {
        id: memberVolume
        parent: Overlay.overlay; anchors.centerIn: parent; width: Math.min(340, parent.width - 24)
        title: qsTr("Volume for me")
        buttons: Dialog.Close
        contentItem: SettingSlider {
            id: memberGain
            caption: root.selectedMember.name || ""
            from: 0; to: 2; stepSize: 0.05
            property int revision: 0
            value: { revision; return root.selectedMember.id ? audio.participantVolume(root.selectedMember.id) : 1 }
            valueText: Math.round(value * 100) + " %"
            applyValue: function(value) { return audio.setParticipantVolume(root.selectedMember.id, value) }
            Connections { target: audio; function onProfileChanged() { memberGain.revision++ } }
        }
    }
    header: Rectangle {
        implicitHeight: 52
        color: Theme.surface
        RowLayout {
            anchors.fill: parent; anchors.margins: 14; spacing: 8
            Label { text: root.remote ? root.network.controlTargetName : "SquadSpeak"; textFormat: Text.PlainText; elide: Text.ElideRight; font.pixelSize: 19; font.weight: Font.DemiBold; Layout.fillWidth: true }
            ActionButton { objectName: "addChannel"; text: qsTr("Add channel"); glyph: "plus"; iconOnly: true; flat: true; enabled: !root.remote; onClicked: discover.open() }
            ActionButton { objectName: "openSettings"; visible: !root.remote; text: qsTr("Settings"); glyph: "settings"; iconOnly: true; flat: true; onClicked: root.openSettings(0) }
        }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: root.height < 460 ? 8 : 12; spacing: root.height < 460 ? 4 : 8
        ScrollView {
            id: scroll
            objectName: "channelsScroll"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true; contentWidth: availableWidth
            ColumnLayout {
                width: scroll.availableWidth; spacing: 2
                Repeater {
                    model: channelRows
                    delegate: Rectangle {
                        id: channelRow
                        required property var channelData
                        readonly property var modelData: channelData
                        readonly property bool active: root.activeHost === modelData.id && !!root.current.joined
                        readonly property bool selected: root.inspectedHost === modelData.id && root.chatExpanded
                        readonly property var members: modelData.members || []
                        Layout.fillWidth: true; implicitHeight: 42 + (selected ? chatSlot.height + 8 : 0)
                        radius: Theme.controlRadius; color: selected ? Theme.surface : channelHover.containsMouse ? Theme.surface : "transparent"
                        border.width: activeFocus ? 1 : 0; border.color: Theme.accent
                        MouseArea {
                            id: channelHover
                            objectName: "channelRow_" + modelData.id
                            anchors.left: parent.left; anchors.right: parent.right; height: 42
                            hoverEnabled: true; acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onDoubleClicked: function(mouse) { if (mouse.button === Qt.LeftButton) { channelClick.stop(); root.chatExpanded = true; root.joinHost(modelData.id) } }
                            onClicked: function(mouse) {
                                if (mouse.button === Qt.RightButton) { root.selectedHost = modelData.id; channelMenu.popup() }
                                else { channelClick.hostId = modelData.id; channelClick.restart() }
                            }
                        }
                        RowLayout {
                            anchors.left: parent.left; anchors.right: parent.right; height: 42
                            anchors.leftMargin: 8; anchors.rightMargin: 2; spacing: 6
                            Glyph { symbol: "down"; rotation: channelRow.selected ? 0 : -90; color: channelRow.selected ? Theme.accent : Theme.muted }
                            Label { objectName: "channelName_" + modelData.id; text: modelData.name; textFormat: Text.PlainText; font.weight: channelRow.selected ? Font.DemiBold : Font.Normal; Layout.fillWidth: true; elide: Text.ElideRight }
                            Rectangle { visible: channelRow.active; implicitWidth: 6; implicitHeight: 6; radius: Theme.smallRadius; color: Theme.accent; Accessible.name: qsTr("Connected to voice") }
                            Label { visible: !!modelData.owned || modelData.id === (root.current.ownId || root.network.ownId); text: qsTr("You host"); font.pixelSize: 11; color: Theme.muted }
                            Label {
                                visible: !modelData.online
                                text: root.accessLabel(modelData.access)
                                font.pixelSize: 11; color: Theme.muted
                                Layout.maximumWidth: 75; elide: Text.ElideRight
                            }
                            Row {
                                visible: !!modelData.online && channelRow.members.length > 0
                                spacing: -5
                                activeFocusOnTab: true
                                Accessible.role: Accessible.Button
                                Accessible.name: qsTr("Show all %1 members").arg(channelRow.members.length)
                                Accessible.onPressAction: root.toggleMembers(channelRow.modelData.id)
                                Keys.onReturnPressed: root.toggleMembers(channelRow.modelData.id)
                                Keys.onSpacePressed: root.toggleMembers(channelRow.modelData.id)
                                TapHandler { onTapped: root.toggleMembers(channelRow.modelData.id) }
                                Repeater {
                                    model: channelRow.members.slice(0, 3)
                                    VoiceAvatar {
                                        id: memberAvatar
                                        required property var modelData
                                        width: 36; height: 36; circular: true
                                        avatar: modelData.avatarId || modelData.avatar || "mossling"; name: modelData.name; music: !!modelData.music
                                        available: modelData.available; muted: modelData.muted; deafened: !!modelData.deafened; sleeping: !!modelData.sleeping
                                        identity: modelData.id; animationTime: root.avatarTime
                                        objectName: "channelAvatar_" + channelRow.modelData.id + "_" + modelData.id
                                        animated: root.voice.animatedAvatars
                                        level: channelRow.active ? root.memberLevel(modelData.id) : 0
                                        TextToolTip { objectName: "memberTooltip"; parent: memberAvatar; text: memberAvatar.name; visible: avatarHover.hovered }
                                        HoverHandler { id: avatarHover }
                                    }
                                }
                            }
                            ActionButton {
                                visible: !!modelData.online && channelRow.members.length > 3
                                text: "+" + (channelRow.members.length - 3)
                                flat: true
                                Accessible.name: qsTr("Show all %1 members").arg(channelRow.members.length)
                                onClicked: root.toggleMembers(modelData.id)
                            }
                            ActionButton {
                                text: qsTr("Channel options"); glyph: "more"; iconOnly: true; flat: true
                                onClicked: { root.selectedHost = modelData.id; channelMenu.popup() }
                            }
                        }
                        Item {
                            id: chatSlot
                            objectName: "conversationSlot_" + channelRow.modelData.id
                            anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top; anchors.topMargin: 42
                            height: Math.max(128, conversation.implicitHeight, scroll.height - Math.min(3, channelRows.count) * 44 - 12)
                            visible: channelRow.selected
                            function selectSlot() {
                                if (channelRow.selected) {
                                    root.conversationSlot = chatSlot
                                    Qt.callLater(function() {
                                        if (!channelRow.selected) return
                                        const flick = scroll.contentItem
                                        const bottom = channelRow.y + channelRow.height
                                        if (channelRow.y < flick.contentY) flick.contentY = channelRow.y
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
                        Accessible.name: modelData.name + (channelRow.active ? ", " + qsTr("Connected to voice") : "")
                        Accessible.onPressAction: root.showHostChat(modelData.id)
                        Keys.onReturnPressed: root.showHostChat(modelData.id)
                        Keys.onSpacePressed: root.showHostChat(modelData.id)
                    }
                }
                Label { visible: root.channelList.length === 0; text: root.remote ? root.network.controlStatus : qsTr("No channels yet"); Layout.fillWidth: true; color: Theme.muted; wrapMode: Text.Wrap }
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
            Layout.fillWidth: true; spacing: 6
            Item { Layout.fillWidth: true }
            Label {
                visible: !!root.current.chatReady
                text: root.current.chatLifetimeDays === 1 ? qsTr("24 h") : qsTr("%1 d").arg(root.current.chatLifetimeDays || 1)
                color: Theme.muted; font.pixelSize: 11
                ToolTip.text: qsTr("Lifetime for new messages. Earlier messages keep their expiry.")
                ToolTip.visible: expiryHover.hovered
                HoverHandler { id: expiryHover }
            }
        }
        ScrollView {
            objectName: "channelMembers"
            visible: root.showChat && root.current.chatReady && root.chatMembers.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(memberFlow.implicitHeight, root.height < 460 ? 48 : 100)
            clip: true; contentWidth: availableWidth
            Flow {
                id: memberFlow
                width: parent.width
                spacing: 6
                Repeater {
                    model: root.chatMembers
                    delegate: Item {
                        required property var modelData
                        width: Math.min(memberFlow.width, 176); height: 48
                        activeFocusOnTab: true
                        Accessible.role: Accessible.Button
                        Accessible.name: modelData.name
                        Accessible.onPressAction: root.memberOptions(modelData, root.inspectedHost)
                        Keys.onReturnPressed: root.memberOptions(modelData, root.inspectedHost)
                        Keys.onSpacePressed: root.memberOptions(modelData, root.inspectedHost)
                        Rectangle { anchors.fill: parent; visible: parent.activeFocus; color: "transparent"; radius: Theme.controlRadius; border.color: Theme.accent }
                        RowLayout {
                            anchors.fill: parent; spacing: 5
                            VoiceAvatar {
                                objectName: "participantAvatar"
                                Layout.preferredWidth: 44; Layout.preferredHeight: 44; circular: true
                                avatar: modelData.avatarId || modelData.avatar || "mossling"; name: modelData.name; music: !!modelData.music
                                available: modelData.available; muted: modelData.muted; deafened: !!modelData.deafened; sleeping: !!modelData.sleeping
                                identity: modelData.id; animationTime: root.avatarTime
                                animated: root.voice.animatedAvatars
                                level: root.inspectedHost === root.activeHost ? root.memberLevel(modelData.id) : 0
                            }
                            Label { text: modelData.name; textFormat: Text.PlainText; Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 12 }
                        }
                        MouseArea { anchors.fill: parent; acceptedButtons: Qt.LeftButton | Qt.RightButton; onClicked: root.memberOptions(modelData, root.inspectedHost) }
                    }
                }
            }
        }
        Loader {
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? (root.height < 460 ? 48 : 112) : 0
            active: !!root.screen
            visible: !root.remote && !!root.network.screenView.available
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
            active: root.visible && root.showChat
            Layout.fillWidth: true; Layout.fillHeight: true
        }
    }
    footer: Rectangle {
        implicitHeight: controls.implicitHeight + 16
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
                ActionButton {
                    objectName: "audioQuality"
                    readonly property int bitrate: root.current.receiveAudioBitrate || 0
                    visible: bitrate > 0
                    text: bitrate === 32 ? qsTr("High") : bitrate === 20 ? qsTr("Balanced") : bitrate === 12 ? qsTr("Low") : qsTr("Minimum")
                    glyph: "signal"; flat: true; font.pixelSize: 11
                    ToolTip.visible: hovered || activeFocus
                    ToolTip.text: qsTr("Receive quality: %1 kb/s per voice. Adjusts automatically for this device.").arg(bitrate)
                    onClicked: ToolTip.show(ToolTip.text, 5000)
                }
                Item { Layout.fillWidth: true }
                ActionButton { objectName: "mute"; text: root.micMuted ? qsTr("Unmute microphone") : qsTr("Mute microphone"); glyph: root.micMuted || (!root.remote && !root.voice.available) ? "mute" : "mic"; iconOnly: true; flat: true; checked: root.micMuted; enabled: !root.remote || root.network.remoteAllowed; onClicked: root.remote ? root.network.remoteAction("mute", {value: !root.micMuted}) : root.voice.setMuted(!root.micMuted) }
                ActionButton { objectName: "deafen"; text: root.outputMuted ? qsTr("Enable speakers") : qsTr("Mute speakers"); glyph: root.outputMuted ? "deafen" : "speaker"; iconOnly: true; flat: true; checked: root.outputMuted; enabled: !root.remote || root.network.remoteAllowed; onClicked: root.remote ? root.network.remoteAction("deafen", {value: !root.outputMuted}) : root.voice.setDeafened(!root.outputMuted) }
                ActionButton {
                    objectName: "remoteToggle"
                    visible: root.remote || root.network.remoteOffers.length > 0
                    text: root.remote ? qsTr("Return to this device") : qsTr("Remote control")
                    glyph: "remote"; iconOnly: true; flat: true; checked: root.remote
                    onClicked: {
                        if (root.remote) root.network.setRemoteMode(false)
                        else if (root.network.remoteOffers.length === 1) {
                            if (!root.network.chooseRemoteOffer(root.network.remoteOffers[0].id)) root.showNotice(root.network.controlStatus)
                        }
                        else remoteTargets.open()
                    }
                }
                ActionButton { objectName: "leaveChannel"; text: qsTr("Leave channel"); destructive: true; glyph: "leave"; iconOnly: true; flat: true; visible: root.activeHost.length > 0; onClicked: root.leaveHost() }
            }
            ActionButton {
                objectName: "pttButton"
                Layout.fillWidth: true
                visible: root.voice.pushToTalk || root.remote
                text: root.voice.pttButtonHeld ? qsTr("Transmitting key held") : qsTr("Hold to talk")
                glyph: "mic"; checked: root.voice.pttButtonHeld
                onPressed: root.voice.setPttButtonHeld(true)
                onReleased: root.voice.setPttButtonHeld(false)
                onCanceled: root.voice.setPttButtonHeld(false)
            }
            Label {
                id: statusMessage
                Layout.fillWidth: true
                visible: text.length > 0
                text: root.voice.error || root.notice || (root.remote && !root.network.controlConnected ? root.network.controlStatus : "")
                textFormat: Text.PlainText; font.pixelSize: 11; color: root.voice.error ? Theme.danger : Theme.muted
                maximumLineCount: 2; wrapMode: Text.Wrap; elide: Text.ElideRight
                TextToolTip { parent: statusMessage; text: statusMessage.text; visible: statusHover.hovered }
                HoverHandler { id: statusHover }
            }
        }
    }
}
