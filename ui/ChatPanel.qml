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
    property real layoutWidth: 0
    property real layoutHeight: 0
    readonly property var language: audio.language
    readonly property string translationKey: "draft:" + hostId
    readonly property string translationTarget: { language.revision; return remote ? "" : language.channelTarget(hostId) }
    readonly property var translatedDraft: { language.revision; return language.result(translationKey) }
    readonly property bool translationReady: !draft.text.trim().length || !translationTarget.length || (translatedDraft.original === draft.text
        && translatedDraft.target === translationTarget && !!translatedDraft.text && !translatedDraft.pending)
    function translateDraft() {
        language.cancel(translationKey)
        draftTranslation.restart()
    }
    Timer {
        id: draftTranslation; interval: 400
        onTriggered: if (!root.remote && !root.language.busy && root.translationTarget.length && draft.text.trim().length)
            root.language.request(root.translationKey, draft.text, "", root.translationTarget)
    }
    onTranslationTargetChanged: translateDraft()
    Connections {
        target: root.language
        function onChanged() {
            if (!root.language.busy && root.translationTarget.length && draft.text.trim().length && !root.translatedDraft.original)
                draftTranslation.restart()
        }
    }

    function restoreLayout() {
        updating = true
        restorePosition.restart()
    }

    function saveAnchor() {
        if (updating) return
        followEnd = history.atYEnd && !current.hasNewerMessages
        const item = history.messageAt(history.contentY + 1)
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
        for (let i = 0; i < incoming.length; ++i) {
            while (i < visibleMessages.count && visibleMessages.get(i).message.sequence < incoming[i].sequence)
                visibleMessages.remove(i)
            if (i >= visibleMessages.count || visibleMessages.get(i).message.sequence !== incoming[i].sequence)
                visibleMessages.insert(i, {message: incoming[i]})
            else if (JSON.stringify(visibleMessages.get(i).message) !== JSON.stringify(incoming[i]))
                visibleMessages.setProperty(i, "message", incoming[i])
        }
        if (visibleMessages.count > incoming.length)
            visibleMessages.remove(incoming.length, visibleMessages.count - incoming.length)
        restoreLayout()
    }
    Timer {
        id: restorePosition
        interval: 0
        onTriggered: {
            messageColumn.forceLayout()
            if (root.initialPage || root.followEnd) history.scrollTo(history.contentHeight - history.height)
            else if (root.anchorSequence > 0) {
                for (let i = 0; i < visibleMessages.count; ++i) if (visibleMessages.get(i).message.sequence === root.anchorSequence) {
                    history.scrollTo(messageRows.itemAt(i).y + root.anchorOffset)
                    break
                }
            }
            if (visibleMessages.count > 0) root.initialPage = false
            root.layoutWidth = history.width; root.layoutHeight = history.height
            root.updating = false
            root.saveAnchor()
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
        if (draftKey.length > 0) language.cancel("draft:" + draftKey.substring(draftKey.indexOf(":") + 1))
        draftKey = (remote ? "remote:" : "local:") + hostId
        draft.text = drafts[draftKey] || ""
        sendError = ""
        initialPage = true; anchorSequence = 0; visibleMessages.clear()
        updateHistory()
    }
    onHostIdChanged: switchDraft()
    onRemoteChanged: switchDraft()
    onRecordsChanged: updateHistory()
    onActiveChanged: if (active) {
        initialPage = true
        restoreLayout()
    }
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
        Flickable {
            id: history
            objectName: "chatHistory"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 16
            clip: true
            contentWidth: width
            contentHeight: messageColumn.height
            flickableDirection: Flickable.VerticalFlick
            property alias rows: messageRows
            function scrollTo(y) {
                cancelFlick()
                contentY = Math.max(0, Math.min(y, contentHeight - height))
            }
            function messageAt(y) {
                for (let i = 0; i < messageRows.count; ++i) {
                    const row = messageRows.itemAt(i)
                    if (row.y + row.height > y) return row
                }
                return null
            }
            ScrollBar.vertical: ScrollBar {
                objectName: "chatHistoryScrollBar"
            }
            WheelHandler {
                target: null
                blocking: false
                onWheel: function(event) {
                    const delta = event.angleDelta.y || event.pixelDelta.y
                    if (delta > 0 && history.atYBeginning && root.current.hasOlderMessages) root.page(true)
                    else if (delta < 0 && history.atYEnd && root.current.hasNewerMessages) root.page(false)
                    event.accepted = false
                }
            }
            onWidthChanged: root.restoreLayout()
            onHeightChanged: root.restoreLayout()
            onContentHeightChanged: root.restoreLayout()
            onContentYChanged: {
                if (width === root.layoutWidth && height === root.layoutHeight) root.saveAnchor()
                if (!moving || root.updating || root.current.historyLoading) return
                if (atYBeginning && root.current.hasOlderMessages) root.page(true)
                else if (atYEnd && root.current.hasNewerMessages) root.page(false)
            }
            Column {
                id: messageColumn
                width: history.width
                spacing: 14
                // The protocol bounds this window to 160 messages / 512 KiB.
                // Lay it out exactly so scrolling never changes estimated height.
                Item {
                    width: parent.width; height: root.current.hasOlderMessages || root.current.historyLoading ? 32 : 8
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
                Repeater {
                    id: messageRows
                    model: visibleMessages
                    delegate: Item {
                        id: messageRow
                        LayoutMirroring.enabled: false
                        LayoutMirroring.childrenInherit: true
                        required property var message
                        readonly property bool systemMessage: !!message.event
                        readonly property var translationPreference: { root.language.revision; return root.language.member(message.sender) }
                        readonly property string translationKey: root.hostId + ":" + message.sequence
                        readonly property var translated: { root.language.revision; return root.language.result(translationKey) }
                        readonly property bool wantsTranslation: !root.remote && !systemMessage && !own && !!translationPreference.incoming
                        readonly property bool hasTranslation: wantsTranslation && translated.original === message.text && !!translated.text && !translated.pending
                        property bool showOriginal: false
                        Timer {
                            id: incomingTranslation; interval: 50
                            running: messageRow.inViewport && messageRow.wantsTranslation && !messageRow.translated.original && !root.language.busy
                            onTriggered: root.language.request(messageRow.translationKey, messageRow.message.text,
                                messageRow.translationPreference.source, messageRow.translationPreference.target)
                        }
                        readonly property bool own: !systemMessage && message.sender === root.current.ownId
                        objectName: "messageRow_" + message.sequence
                        width: messageColumn.width - 8
                        height: messageBody.implicitHeight + 12
                        readonly property bool inViewport: root.active && y + height >= history.contentY
                            && y <= history.contentY + history.height
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
                            Loader {
                                Layout.preferredWidth: 44; Layout.preferredHeight: 44; Layout.alignment: Qt.AlignTop
                                active: messageRow.inViewport
                                sourceComponent: VoiceAvatar {
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
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3
                                RowLayout {
                                    Layout.fillWidth: true
                                    Label { text: messageRow.message.name; textFormat: Text.PlainText; elide: Text.ElideRight; font.weight: Font.DemiBold; Layout.fillWidth: true; horizontalAlignment: messageRow.own ? Text.AlignRight : Text.AlignLeft }
                                    Label { text: Qt.formatDateTime(new Date(messageRow.message.created), "hh:mm"); color: Theme.muted; font.pixelSize: 11 }
                                }
                                ActionButton {
                                    Layout.alignment: Qt.AlignRight
                                    visible: messageRow.hasTranslation || (messageRow.wantsTranslation && !!messageRow.translated.error)
                                    text: messageRow.hasTranslation ? qsTranslate("LocalLanguage", "Original text") : messageRow.translated.error
                                    glyph: "translate"; iconOnly: true; flat: true; implicitHeight: 26
                                    onClicked: messageRow.showOriginal = !messageRow.showOriginal
                                }
                                TextEdit {
                                    id: messageText
                                    objectName: "messageText_" + messageRow.message.sequence
                                    Layout.fillWidth: true
                                    text: {
                                        Qt.uiLanguage // Reformat after the translation catalog changes.
                                        if (messageRow.systemMessage) {
                                            const event = messageRow.message.event
                                            if (event.kind !== "announcement") return chatContent.plainText(messageRow.message)
                                        }
                                        root.network.imageRevision
                                        const image = messageRow.message.image
                                        return chatContent.format(messageRow.hasTranslation ? messageRow.translated.text : messageRow.message.text, image ? {
                                            "hash": image.hash,
                                            // Keep image geometry without retaining offscreen decoded images.
                                            "source": (messageRow.inViewport && root.network.imageSource(image.hash))
                                                || "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAAC0lEQVR4nGNgAAIAAAUAAXpeqz8AAAAASUVORK5CYII=",
                                            "width": image.width, "height": image.height, "displayWidth": width
                                        } : {}, Theme.accent, Theme.codeBackground, font)
                                    }
                                    textFormat: messageRow.systemMessage && messageRow.message.event.kind !== "announcement" ? TextEdit.PlainText : TextEdit.RichText
                                    readOnly: true; selectByMouse: mouseSelection.active; selectByKeyboard: true; wrapMode: TextEdit.Wrap
                                    // A mouse drag selects text. Touch starts with no selection grab so the list can scroll.
                                    PointHandler { id: mouseSelection; acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad }
                                    color: messageRow.systemMessage ? Theme.muted : Theme.text; font.pixelSize: messageRow.systemMessage ? 12 : 14
                                    function requestAttachment() {
                                        if (messageRow.inViewport && messageRow.message.image)
                                            root.network.requestImage(messageRow.message.image.hash)
                                    }
                                    Timer { interval: 0; running: true; onTriggered: messageText.requestAttachment() }
                                    Connections { target: messageRow; function onInViewportChanged() { messageText.requestAttachment() } }
                                    onLinkActivated: function(link) { chatContent.openLink(link) }
                                }
                                TextEdit {
                                    Layout.fillWidth: true
                                    visible: messageRow.showOriginal && messageRow.hasTranslation
                                    text: chatContent.plainText(messageRow.message); textFormat: TextEdit.PlainText
                                    readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                                    color: Theme.muted; font.pixelSize: 12
                                }
                            }
                        }
                    }
                }
            }
            Label {
                anchors.centerIn: parent
                width: parent.width - 24; horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                visible: messageRows.count === 0
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
        ColumnLayout {
            Layout.fillWidth: true
            visible: root.translationTarget.length > 0 && draft.text.trim().length > 0
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 12; color: Theme.muted
                    text: root.translatedDraft.text ? chatContent.plainText({text: root.translatedDraft.text})
                        : root.translatedDraft.error || qsTranslate("ChatPanel", "Loading...")
                    textFormat: Text.PlainText; maximumLineCount: 3; elide: Text.ElideRight
                }
                ActionButton {
                    text: root.language.preparing ? qsTranslate("Channels", "Cancel") : qsTranslate("LocalLanguage", "Prepare languages"); glyph: root.language.preparing ? "close" : "down"; iconOnly: true; flat: true
                    visible: !!root.translatedDraft.error && !!root.translatedDraft.detectedSource
                    onClicked: root.language.preparing ? root.language.cancelPreparation() : root.language.prepareTranslation(root.translationKey)
                }
            }
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
                ActionButton {
                    objectName: "translateDraft"
                    text: qsTranslate("LocalLanguage", "Translation (experimental)"); glyph: "translate"; iconOnly: true; flat: true
                    visible: root.language.supported && !root.remote
                    checked: root.translationTarget.length > 0
                    Layout.alignment: Qt.AlignBottom
                    onClicked: outgoingLanguage.open()
                    Popup {
                        id: outgoingLanguage
                        popupType: Popup.Item
                        width: Math.min(320, root.width - 16); padding: 10
                        background: Rectangle { color: Theme.surface; radius: Theme.controlRadius; border.color: Theme.border }
                        contentItem: ColumnLayout {
                            Label { text: qsTranslate("LocalLanguage", "Translate to"); color: Theme.text }
                            ChoiceBox {
                                Layout.fillWidth: true
                                model: [{id: "", name: qsTranslate("AudioSettings", "Off")}].concat(root.language.languages)
                                textRole: "name"; valueRole: "id"
                                currentIndex: Math.max(0, model.findIndex(function(item) { return item.id === root.translationTarget }))
                                enabled: root.language.available
                                onActivated: { root.language.setChannelTarget(root.hostId, currentValue); outgoingLanguage.close() }
                            }
                            Label {
                                visible: !root.language.available; Layout.fillWidth: true; wrapMode: Text.Wrap; color: Theme.muted
                                text: qsTranslate("LocalLanguage", "Local language processing is unavailable.")
                            }
                        }
                    }
                }
                ScrollView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(88, Math.max(36, draft.contentHeight + 12))
                    TextArea {
                        id: draft
                        objectName: "chatDraft"
                        onTextChanged: root.translateDraft()
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
                        && (!chatContent.hasImage || root.attachmentHost === root.hostId || root.remote) && root.translationReady
                    onClicked: {
                        root.sentDraft = draft.text; root.sentHost = root.hostId
                        root.sentDrafts[root.draftKey] = draft.text
                        root.sendError = ""
                        if (root.remote) {
                            root.awaitingRemoteReceipt = root.network.remoteAction("chat", {text: draft.text})
                            if (!root.awaitingRemoteReceipt) root.sendError = qsTr("Message could not be sent. Check the connection and text length.")
                        } else chatContent.prepareMessage(root.translationTarget.length && draft.text.trim().length ? root.translatedDraft.text : draft.text)
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
