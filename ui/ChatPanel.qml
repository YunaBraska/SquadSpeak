import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs

Item {
    id: root
    implicitHeight: chatLayout.implicitHeight
    required property var network
    property bool animatedAvatars: true
    property double animationTime: 0
    property var memberLevel: function(id) { return 0 }
    property bool active: visible
    readonly property bool remote: network.remoteMode
    readonly property var current: remote ? network.remoteView : network
    readonly property string hostId: current.chatHostId || ""
    readonly property var records: current.messages || []
    property var drafts: ({})
    property string draftKey: ""
    property string sentDraft: ""
    property bool awaitingRemoteReceipt: false
    property string sentHost: ""
    property string attachmentHost: ""
    property string sendError: ""
    property var sentDrafts: ({})
    property bool updating: false
    property bool initialPage: true
    property real anchorSequence: 0
    property real anchorOffset: 0
    property bool followEnd: true

    function saveAnchor() {
        if (updating) return
        followEnd = history.atYEnd && !current.hasNewerMessages
        const index = history.indexAt(1, history.contentY + 1)
        const item = index >= 0 ? history.itemAtIndex(index) : null
        anchorSequence = item ? item.message.sequence : 0
        anchorOffset = item ? history.contentY - item.y : 0
    }
    function updateHistory() {
        if (records.length === visibleMessages.count && records.every(function(record, i) {
            return JSON.stringify(record) === JSON.stringify(visibleMessages.get(i).message)
        })) return
        saveAnchor()
        updating = true
        const incoming = records
        if (incoming.length === 0) visibleMessages.clear()
        else {
            const first = incoming[0].sequence, last = incoming[incoming.length - 1].sequence
            while (visibleMessages.count && visibleMessages.get(0).message.sequence < first) visibleMessages.remove(0)
            while (visibleMessages.count && visibleMessages.get(visibleMessages.count - 1).message.sequence > last) visibleMessages.remove(visibleMessages.count - 1)
            for (let i = 0; i < incoming.length; ++i) {
                if (i >= visibleMessages.count || visibleMessages.get(i).message.sequence !== incoming[i].sequence)
                    visibleMessages.insert(i, {message: incoming[i]})
                else if (JSON.stringify(visibleMessages.get(i).message) !== JSON.stringify(incoming[i]))
                    visibleMessages.setProperty(i, "message", incoming[i])
            }
        }
        restorePosition.atEnd = initialPage || followEnd
        restorePosition.sequence = anchorSequence; restorePosition.offset = anchorOffset
        restorePosition.restart()
    }
    Timer {
        id: restorePosition
        interval: 0
        property bool atEnd: false
        property real sequence: 0
        property real offset: 0
        onTriggered: {
            history.forceLayout()
            if (atEnd) history.positionViewAtEnd()
            else if (sequence > 0) {
                for (let i = 0; i < visibleMessages.count; ++i) if (visibleMessages.get(i).message.sequence === sequence) {
                    history.positionViewAtIndex(i, ListView.Beginning)
                    history.contentY += offset
                    break
                }
            }
            if (visibleMessages.count > 0) root.initialPage = false
            root.updating = false
        }
    }

    function page(older) {
        if (current.historyLoading || updating) return
        saveAnchor()
        followEnd = false
        if (older) network.loadOlderMessages()
        else network.loadNewerMessages()
    }
    function switchDraft() {
        if (draftKey.length > 0) drafts[draftKey] = draft.text
        draftKey = (remote ? "remote:" : "local:") + hostId
        draft.text = drafts[draftKey] || ""
        sendError = ""
        initialPage = true; anchorSequence = 0; visibleMessages.clear()
        updateHistory()
    }
    onHostIdChanged: switchDraft()
    onRemoteChanged: switchDraft()
    onRecordsChanged: updateHistory()
    Component.onCompleted: switchDraft()

    ListModel { id: visibleMessages; dynamicRoles: true }
    FileDialog {
        id: chooseImage
        title: qsTr("Choose a chat image")
        nameFilters: [qsTr("Images (*.png *.jpg *.jpeg *.webp *.gif *.bmp)")]
        onAccepted: { root.attachmentHost = root.hostId; chatContent.prepareFile(selectedFile) }
    }
    ColumnLayout {
        id: chatLayout
        anchors.fill: parent
        spacing: 6
        ListView {
            id: history
            objectName: "chatHistory"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 16
            clip: true
            spacing: 14
            model: visibleMessages
            cacheBuffer: 0
            ScrollBar.vertical: ScrollBar {}
            WheelHandler {
                target: null
                onWheel: function(event) {
                    const delta = event.angleDelta.y || event.pixelDelta.y
                    if (delta > 0 && history.atYBeginning && root.current.hasOlderMessages) root.page(true)
                    else if (delta < 0 && history.atYEnd && root.current.hasNewerMessages) root.page(false)
                    event.accepted = false
                }
            }
            onContentYChanged: {
                if (!moving || root.updating || root.current.historyLoading) return
                if (atYBeginning && root.current.hasOlderMessages) root.page(true)
                else if (atYEnd && root.current.hasNewerMessages) root.page(false)
            }
            header: Item {
                width: history.width; height: root.current.hasOlderMessages || root.current.historyLoading ? 32 : 8
                ActionButton {
                    anchors.centerIn: parent
                    objectName: "loadOlderMessages"
                    text: root.current.historyLoading ? qsTr("Loading...") : qsTr("Earlier messages")
                    flat: true; font.pixelSize: 11
                    visible: !!root.current.hasOlderMessages || !!root.current.historyLoading
                    enabled: !root.current.historyLoading
                    onClicked: root.page(true)
                }
            }
            delegate: Item {
                id: messageRow
                LayoutMirroring.enabled: false
                LayoutMirroring.childrenInherit: true
                required property var message
                readonly property bool systemMessage: !!message.event
                readonly property bool own: !systemMessage && message.sender === root.current.ownId
                objectName: "messageRow_" + message.sequence
                width: ListView.view.width - 8
                height: messageBody.implicitHeight + 12
                Rectangle {
                    anchors.fill: messageBody; anchors.margins: -6
                    visible: messageRow.own; color: Theme.raised; radius: Theme.panelRadius
                }
                RowLayout {
                id: messageBody
                objectName: "messageBody_" + messageRow.message.sequence
                x: messageRow.own ? messageRow.width - width - 6 : 6
                y: 6
                width: messageRow.width * 0.88 - 12
                layoutDirection: messageRow.own ? Qt.RightToLeft : Qt.LeftToRight
                spacing: 8
                VoiceAvatar {
                    Layout.preferredWidth: 44; Layout.preferredHeight: 44; Layout.alignment: Qt.AlignTop
                    objectName: "messageAvatar_" + messageRow.message.sender
                    systemMessage: messageRow.systemMessage
                    online: systemMessage || !root.current.chatPresenceKnown || (root.current.chatOnlineIds || []).includes(messageRow.message.sender)
                    avatar: messageRow.message.avatarId || messageRow.message.avatar || "mossling"; name: messageRow.message.name
                    readonly property var member: systemMessage ? (root.current.chatBot || {}) : (root.current.chatMembers || []).find(function(m) { return m.id === messageRow.message.sender }) || ({})
                    identity: messageRow.message.sender; animationTime: root.animationTime
                    available: member.available !== false; muted: !!member.muted; deafened: !!member.deafened; sleeping: !!member.sleeping
                    level: root.memberLevel(identity)
                    circular: true; animated: root.animatedAvatars
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    RowLayout {
                        Layout.fillWidth: true
                        Label { text: messageRow.message.name; textFormat: Text.PlainText; elide: Text.ElideRight; font.weight: Font.DemiBold; Layout.fillWidth: true; horizontalAlignment: messageRow.own ? Text.AlignRight : Text.AlignLeft }
                        Label { text: Qt.formatDateTime(new Date(messageRow.message.created), "hh:mm"); color: Theme.muted; font.pixelSize: 11 }
                    }
                    TextEdit {
                        id: messageText
                        objectName: "messageText_" + messageRow.message.sequence
                        Layout.fillWidth: true
                        text: {
                            if (messageRow.systemMessage) {
                                const event = messageRow.message.event
                                switch (event.kind) {
                                case "joined": return qsTr("%1 joined.").arg(event.name)
                                case "left": return qsTr("%1 left.").arg(event.name)
                                case "kicked": return qsTr("%1 was kicked.").arg(event.name)
                                case "banned": return qsTr("%1 was banned from the channel.").arg(event.name)
                                }
                                if (event.kind !== "announcement") return messageRow.message.text
                            }
                            root.network.imageRevision
                            const image = messageRow.message.image
                            return chatContent.format(messageRow.message.text, image ? {
                                "hash": image.hash, "source": root.network.imageSource(image.hash),
                                "width": image.width, "height": image.height, "displayWidth": width
                            } : {}, Theme.accent)
                        }
                        textFormat: messageRow.systemMessage && messageRow.message.event.kind !== "announcement" ? TextEdit.PlainText : TextEdit.RichText
                        readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                        color: messageRow.systemMessage ? Theme.muted : Theme.text; font.pixelSize: messageRow.systemMessage ? 12 : 14
                        function requestAttachment() {
                            if (root.active && messageRow.message.image && messageRow.y + messageRow.height >= history.contentY
                                    && messageRow.y <= history.contentY + history.height)
                                root.network.requestImage(messageRow.message.image.hash)
                        }
                        Timer { interval: 0; running: true; onTriggered: messageText.requestAttachment() }
                        Connections { target: root; function onActiveChanged() { messageText.requestAttachment() } }
                        Connections { target: history; function onContentYChanged() { messageText.requestAttachment() } }
                        onLinkActivated: function(link) { chatContent.openLink(link) }
                    }
                }
                }
            }
            Label {
                anchors.centerIn: parent
                width: parent.width - 24; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                visible: history.count === 0
                text: root.current.historyLoading ? qsTr("Loading...") : root.current.chatReady ? qsTr("Start the conversation.") : qsTr("Chat opens after access is approved.")
                color: Theme.muted
            }
        }
        ActionButton {
            objectName: "latestMessages"
            Layout.alignment: Qt.AlignHCenter
            visible: !!root.current.hasNewerMessages
            text: qsTr("Latest messages"); glyph: "down"; flat: true
            enabled: !root.current.historyLoading
            onClicked: { root.initialPage = true; root.network.refreshChat() }
        }
        RowLayout {
            visible: !root.remote && (chatContent.busy || chatContent.hasImage) && root.attachmentHost === root.hostId
            Layout.fillWidth: true
            Glyph { symbol: "image"; color: Theme.muted }
            Label { text: chatContent.busy ? qsTr("Loading image...") : qsTr("Image attached"); elide: Text.ElideRight; Layout.fillWidth: true; color: Theme.muted }
            ActionButton { text: qsTr("Remove image"); destructive: true; glyph: "close"; iconOnly: true; flat: true; enabled: !chatContent.busy && !root.current.chatPending; onClicked: chatContent.clearImage() }
        }
        Label {
            Layout.fillWidth: true
            visible: (!root.remote && root.network.chatError.length > 0) || chatContent.error.length > 0 || root.sendError.length > 0
            text: root.sendError || chatContent.error || (root.remote ? root.network.controlStatus : root.network.chatError)
            textFormat: Text.PlainText; wrapMode: Text.Wrap; color: Theme.danger; maximumLineCount: 2
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: composer.implicitHeight + 8
            color: Theme.input; radius: Theme.panelRadius; border.color: draft.activeFocus ? Theme.accent : Theme.border
            RowLayout {
                id: composer
                anchors.fill: parent; anchors.margins: 4; spacing: 2
                ActionButton {
                    text: qsTr("Attach image"); glyph: "plus"; iconOnly: true; flat: true; visible: !root.remote
                    enabled: !chatContent.busy && !root.current.chatPending && !!root.current.chatReady
                    Layout.alignment: Qt.AlignBottom
                    onClicked: attachmentMenu.popup()
                    Menu {
        popupType: Popup.Item
                        background: Rectangle { implicitWidth: 240; implicitHeight: 40; radius: Theme.controlRadius; color: Theme.surface; border.color: Theme.border }
                        id: attachmentMenu
                        MenuItem { text: qsTr("Choose image..."); onTriggered: chooseImage.open() }
                        MenuItem { text: qsTr("Paste image"); onTriggered: { root.attachmentHost = root.hostId; chatContent.pasteImage() } }
                    }
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(88, Math.max(36, draft.contentHeight + 12))
                    TextArea {
                        id: draft
                        objectName: "chatDraft"
                        placeholderText: qsTr("Message...")
                        Accessible.name: qsTr("Chat message. Markdown supported. Shift+Enter for a new line.")
                        wrapMode: TextEdit.Wrap; selectByMouse: true
                        color: Theme.text; placeholderTextColor: Theme.muted
                        background: Item {}
                        enabled: !root.current.chatPending && !chatContent.busy && !root.awaitingRemoteReceipt
                        Keys.onPressed: function(event) {
                            if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && !(event.modifiers & Qt.ShiftModifier)) {
                                event.accepted = true
                                if (send.enabled) send.clicked()
                            }
                        }
                    }
                }
                ActionButton {
                    id: send
                    objectName: "sendChat"
                    text: root.current.chatPending ? qsTr("Waiting for delivery") : qsTr("Send")
                    glyph: "send"; iconOnly: true; primary: true
                    Layout.alignment: Qt.AlignBottom
                    enabled: !!root.current.chatReady && !root.current.chatPending && !chatContent.busy && !root.awaitingRemoteReceipt
                        && (draft.text.trim().length > 0 || (chatContent.hasImage && root.attachmentHost === root.hostId))
                        && (!chatContent.hasImage || root.attachmentHost === root.hostId || root.remote)
                    onClicked: {
                        root.sentDraft = draft.text; root.sentHost = root.hostId
                        root.sentDrafts[root.draftKey] = draft.text
                        root.sendError = ""
                        if (root.remote) {
                            root.awaitingRemoteReceipt = root.network.remoteAction("chat", {text: draft.text})
                            if (!root.awaitingRemoteReceipt) root.sendError = qsTr("Message could not be sent. Check the connection and text length.")
                        } else chatContent.prepareMessage(draft.text)
                    }
                }
            }
        }
    }
    Connections {
        target: root.network
        function onRemoteChatFailed(error) { root.awaitingRemoteReceipt = false; root.sendError = error }
        function onRemoteChatSent(text) {
            root.awaitingRemoteReceipt = false
            const key = "remote:" + root.sentHost
            if (root.draftKey === key) root.initialPage = true
            if (root.draftKey === key && draft.text === root.sentDrafts[key]) draft.clear()
            else if (root.drafts[key] === root.sentDrafts[key]) root.drafts[key] = ""
        }
        function onRemoteChanged() {
            if (root.awaitingRemoteReceipt && !root.network.remoteAllowed) {
                root.awaitingRemoteReceipt = false
                root.sendError = qsTr("Remote permission changed. Check delivery on the target device.")
            }
        }
        function onControlChanged() {
            if (root.awaitingRemoteReceipt && !root.network.controlConnected) {
                root.awaitingRemoteReceipt = false
                root.sendError = qsTr("Connection lost. Check the chat before sending this draft again.")
            }
        }
        function onChatSent(text, hostId) {
            if (root.remote) return
            const key = "local:" + hostId
            if (root.draftKey === key) root.initialPage = true
            if (root.draftKey === key && draft.text === root.sentDrafts[key]) draft.clear()
            else if (root.drafts[key] === root.sentDrafts[key]) root.drafts[key] = ""
            if (root.attachmentHost === hostId) chatContent.clearImage()
        }
    }
    Connections {
        target: chatContent
        function onMessagePrepared(markdown, image) {
            if (root.sentHost !== root.hostId || !root.current.chatReady) {
                root.sendError = qsTr("Channel changed. Check the draft before sending.")
                return
            }
            root.network.sendChat(markdown, image)
        }
    }
}
