pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Pane {
    id: root
    objectName: "gamePane"
    required property var network
    required property var voice
    property string hostId: ""
    property string gameId: ""
    property var game: ({})
    property string identity: ""
    property int selected: -1
    property int core: -1
    property int trap: -1
    property string actionError: ""
    property var pendingDeparture: null
    property var cancelledDeparture: null
    property bool departureHasOpponent: false
    property bool dismissOnFinish: false
    property var gameMembers: ({})
    property bool animatedAvatars: true
    property double animationTime: 0
    property var memberLevel: function(id) { return 0 }
    signal activated()
    signal closed()
    signal feedbackRequested(string kind)
    readonly property bool shortWide: width > height * 1.5 && height < 430
    readonly property bool canAddComputer: game.phase === "waiting" && players.length === 1 && game.owner === identity
    component PlayerAvatar: VoiceAvatar {
        readonly property var profile: root.member(identity)
        systemMessage: !!(root.players.find(p => p.id === identity) || {}).computer
        name: root.name(identity); avatar: root.avatar(identity); circular: true
        animated: root.animatedAvatars && root.visible
        animationTime: root.animationTime
        level: root.memberLevel(identity)
        available: profile.available === undefined || profile.available
        online: profile.online === undefined || profile.online
        muted: !!profile.muted; deafened: !!profile.deafened; sleeping: !!profile.sleeping
        music: !!profile.music; provider: profile.provider || ""
    }
    readonly property var players: game.players || []
    readonly property var me: players.find(p => p.id === identity) || ({})
    readonly property bool participant: !!me.id
    readonly property bool playing: ["waiting", "preparing", "playing", "duel"].includes(game.phase)
    readonly property bool settingUp: participant && !me.ready && ["waiting", "preparing"].includes(game.phase)
    readonly property bool myTurn: game.phase === "playing" && game.turn === identity
    readonly property var duel: game.duel || ({})
    readonly property var attacker: (game.board || [])[duel.from] || ({})
    readonly property var defender: (game.board || [])[duel.to] || ({})
    readonly property var fighter: attacker.owner === identity ? attacker : defender.owner === identity ? defender : ({})
    readonly property bool choosing: participant && needsChoice(identity)
    readonly property var current: network.remoteMode ? network.remoteView : network
    readonly property var roster: (current.participants || []).concat(current.chatMembers || [])
    readonly property string statusText: !game.id ? qsTranslate("GamePane", "Game unavailable")
        : game.phase === "finished" ? qsTranslate("GameCard", "%1 wins!").arg(name(game.winner))
        : game.phase === "cancelled" ? qsTranslate("GameCard", "Game ended")
        : settingUp ? (trap < 0 ? qsTranslate("GamePane", "Trap") : qsTranslate("GamePane", "Core"))
        : game.phase === "duel" ? (!choosing || game.choice ? qsTranslate("GamePane", "Waiting for the other player")
            : (game.lastBattle || {}).effect === "tie" ? qsTranslate("GamePane", "Same element. Choose again.") : qsTranslate("GamePane", "Choose an element"))
        : myTurn ? qsTranslate("GamePane", "Your turn") : game.phase === "playing" ? qsTranslate("GamePane", "%1's turn").arg(name(game.turn)) : qsTranslate("GamePane", "Waiting for the other player")
    implicitWidth: 320; implicitHeight: 0
    padding: 0
    background: Rectangle {
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.dark ? "#132d32" : "#f4ecd9" }
            GradientStop { position: 1; color: Theme.dark ? "#102220" : "#dbe5d7" }
        }
    }
    function name(id) { if ((players.find(p => p.id === id) || {}).computer) return qsTranslate("GamePane", "Computer"); return member(id).name || (players.find(p => p.id === id) || {}).name || "" }
    function member(id) { return roster.find(p => p.id === id) || gameMembers[id] || ({}) }
    function avatar(id) { const p = member(id); return p.avatarId || p.avatar || (id === network.ownId ? voice.avatar : "mossling") }
    function remaining(id) { return (game.board || []).filter(p => p.owner === id).length }
    function captured(id) { const opponent = players.find(p => p.id !== id); return opponent ? 14 - remaining(opponent.id) : 0 }
    function needsChoice(id) {
        const piece = attacker.owner === id ? attacker : defender.owner === id ? defender : ({})
        return game.phase === "duel" && (piece.kind === "neutral"
            || (["fire", "water", "ice"].includes(piece.kind) && attacker.kind === defender.kind))
    }
    function act(action, values) {
        const selection = Object.assign({id: gameId, round: game.round || 0}, values || {})
        const accepted = network.gameAction(action, selection, hostId, identity)
        actionError = accepted ? "" : qsTranslate("GamePane", "That game action is no longer available.")
        return accepted
    }
    function refresh() {
        if (!gameId) return
        const previous = game
        game = network.gameView(hostId, gameId, identity)
        const known = Object.assign({}, gameMembers)
        // Qt's list-backed maps can refer to another row after a participant leaves.
        for (const player of players) { const profile = roster.find(p => p.id === player.id); if (profile) known[player.id] = Object.assign({}, profile) }
        gameMembers = known
        if (game.setup) { core = game.core; trap = game.trap }
        else if (game.error) core = -1
        if (!myTurn) selected = -1
        const battle = game.lastBattle || ({})
        const outcome = JSON.stringify(battle), newBattle = battle.number && outcome !== JSON.stringify(previous.lastBattle || ({}))
        if (visible && participant && previous.id === game.id) {
            if (newBattle) {
                if (["tie", "choose"].includes(battle.effect)) { if (choosing) feedbackRequested("game-choose") }
                else feedbackRequested(battle.effect === "capture" ? "piece-core" : "game-" + battle.effect)
            }
            if (game.phase !== previous.phase && ["finished", "cancelled"].includes(game.phase))
                feedbackRequested(game.phase === "finished" && game.winner === identity ? "game-win" : "game-end")
            else if (!newBattle && game.phase === "playing" && (previous.phase !== "playing" || game.round !== previous.round))
                feedbackRequested("game-move")
        }
        if (dismissOnFinish && !playing) finishClose()
    }
    function clearView() {
        if (computerConfirmation.opened) computerConfirmation.reject()
        if (departure.opened) departure.reject()
        help.close()
        dismissOnFinish = false
        gameId = ""; game = ({}); gameMembers = ({})
        selected = -1; core = -1; trap = -1; actionError = ""
    }
    function openGame(host, id) {
        const actor = network.remoteMode ? network.remoteView.ownId : network.ownId
        if (gameId !== id || hostId !== host || identity !== actor) clearView()
        dismissOnFinish = false
        hostId = host; gameId = id
        identity = actor
        refresh()
        activated()
    }
    function pick(cell) {
        const piece = (game.board || [])[cell] || ({})
        if (settingUp) {
            if (piece.owner !== identity || core >= 0 || cell === trap) return
            if (trap < 0) { trap = cell; feedbackRequested("piece-trap") }
            else { core = cell; feedbackRequested("piece-core"); if (!act("setup", {core: core, trap: trap})) core = -1 }
        } else if (myTurn) {
            if (piece.owner === identity && ["neutral", "fire", "water", "ice"].includes(piece.kind)) {
                selected = cell; feedbackRequested("piece-" + piece.kind)
            } else if (board.legal(cell)) { act("move", {from: selected, to: cell}); selected = -1 }
        }
    }
    function confirmDeparture(action, cancelled, activity, actor) {
        const target = activity || game, player = actor || identity
        const participants = target.players || []
        if (!["waiting", "preparing", "playing", "duel"].includes(target.phase)
            || !participants.some(p => p.id === player)) { action(); return }
        if (cancelledDeparture) cancelledDeparture()
        departureHasOpponent = participants.length > 1
        pendingDeparture = action
        cancelledDeparture = cancelled || null
        activated(); if (!departure.opened) departure.open()
    }
    function finishClose() {
        if (playing) return
        dismissOnFinish = false
        if (participant) act("dismiss")
        clearView()
        closed()
    }
    function close() {
        confirmDeparture(function() {
            root.dismissOnFinish = true
            if (root.playing && !root.act("leave")) { root.dismissOnFinish = false; return }
            if (root.dismissOnFinish) root.finishClose()
        })
    }
    Connections {
        target: root.network
        function onChatChanged() { root.refresh() }
        function onRemoteChanged() {
            const actor = root.network.remoteMode ? root.network.remoteView.ownId : root.network.ownId
            if (root.identity && root.identity !== actor) {
                root.clearView()
                const game = root.current.currentGame || ({})
                if (game.id) root.openGame(game.hostId, game.id)
                else root.closed()
            } else root.refresh()
        }
    }
    Item {
        anchors.fill: parent; anchors.margins: 8
        RowLayout {
            id: gameHeader
            width: parent.width
            z: 2
            ActionButton { objectName: "gameInfo"; text: qsTranslate("GamePane", "Information"); glyph: "info"; iconOnly: true; flat: true; onClicked: help.open() }
            Item { Layout.fillWidth: true }
            ActionButton { objectName: "gameLeave"; text: root.playing ? qsTranslate("GameCard", "Leave") : qsTranslate("Channels", "Close"); glyph: "leave"; iconOnly: true; flat: true; onClicked: root.close() }
        }
        RowLayout {
            id: gamePlayers
            width: parent.width
            y: root.shortWide ? (parent.height - height) / 2 : gameHeader.height + 4
            spacing: root.shortWide ? board.width + 20 : 12
            z: 1
            visible: !["finished", "cancelled"].includes(root.game.phase) || board.effectRunning
            Repeater {
                model: 2
                ColumnLayout {
                    id: playerHud
                    required property int index
                    objectName: "gamePlayer_" + index
                    readonly property var player: root.players[index] || ({})
                    readonly property bool highlighted: !!player.id && (root.game.phase === "playing" ? root.game.turn === player.id
                        : root.game.phase === "duel" ? root.needsChoice(player.id) && !player.chosen
                        : ["waiting", "preparing"].includes(root.game.phase) && !!player.ready)
                    Layout.fillWidth: true; spacing: 4
                    Item {
                        Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: root.height < 430 ? 52 : 70; Layout.preferredHeight: Layout.preferredWidth
                        Rectangle {
                            anchors.fill: parent; radius: playerHud.player.id === root.identity ? width / 2 : 12
                            property color team: playerHud.player.id === root.identity ? board.selfColor : board.opponentColor
                            color: playerHud.highlighted ? Qt.rgba(team.r, team.g, team.b, 0.25) : "transparent"
                            border.color: playerHud.player.id ? team : Theme.border; border.width: playerHud.highlighted ? 3 : 1
                        }
                        PlayerAvatar { objectName: "gameAvatar_" + playerHud.index; anchors.fill: parent; anchors.margins: 5; visible: !!playerHud.player.id; identity: playerHud.player.id || "" }
                        ActionButton {
                            objectName: visible ? "gameAddComputer" : ""
                            anchors.fill: parent; visible: !playerHud.player.id && root.canAddComputer
                            text: qsTranslate("GamePane", "Computer"); glyph: "plus"; iconOnly: true; flat: true
                            onClicked: computerConfirmation.open()
                        }
                        Glyph { anchors.centerIn: parent; symbol: "person"; color: Theme.muted; visible: !playerHud.player.id && !root.canAddComputer }
                        Glyph { anchors.right: parent.right; anchors.bottom: parent.bottom; symbol: "check"; color: Theme.accent; visible: !!playerHud.player.ready && ["waiting","preparing"].includes(root.game.phase) }
                    }
                    Label { text: root.name(playerHud.player.id); textFormat: Text.PlainText; font.weight: Font.DemiBold; color: Theme.text; elide: Text.ElideRight; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter }
                    Accessible.role: Accessible.Grouping
                    Accessible.name: player.name || qsTranslate("GamePane", "Waiting for the other player")
                    Accessible.description: highlighted ? root.statusText : ""
                }
            }
        }
        Item {
            id: boardArea
            width: parent.width
            y: root.shortWide ? 0 : gameHeader.height + gamePlayers.height + 8
            height: Math.max(0, parent.height - y - (gameError.visible ? gameError.implicitHeight + 4 : 0))
            GameBoard {
                id: board
                objectName: "gameBoard"
                anchors.centerIn: parent
                width: Math.max(0, Math.min(parent.width - (root.shortWide ? 190 : 10), parent.height * 7 / 6 - 10)); height: width * 6 / 7
                game: root.game; identity: root.identity; core: root.core; trap: root.trap
                settingUp: root.settingUp; selected: root.selected; myTurn: root.myTurn; choosing: root.choosing
                onCellPicked: cell => root.pick(cell)
                onElementPicked: kind => root.act("choose", {choice: kind})
                onPieceHeard: kind => root.feedbackRequested("piece-" + kind)
                Accessible.role: Accessible.Grouping; Accessible.name: root.statusText
            }
            Rectangle {
                id: result
                objectName: "gameResult"
                anchors.fill: parent; radius: 16
                visible: ["finished","cancelled"].includes(root.game.phase) && !board.effectRunning
                color: Theme.dark ? "#ef102624" : "#eff3edda"
                property real reveal: 1
                onVisibleChanged: if (visible) celebration.restart()
                NumberAnimation { id: celebration; target: result; property: "reveal"; from: 0; to: 1; duration: 800 }
                Canvas {
                    anchors.fill: parent
                    property real frame: result.reveal
                    onFrameChanged: requestPaint()
                    onPaint: {
                        const c = getContext("2d"); c.reset()
                        if (root.game.winner !== root.identity) return
                        c.globalAlpha = Math.sin(frame * Math.PI); c.lineWidth = 3
                        for (let i = 0; i < 24; ++i) {
                            const angle = i * Math.PI * 2 / 24, distance = Math.min(width,height) * (Theme.reducedMotion ? 0.35 : 0.15 + frame * 0.4)
                            const x = width / 2 + Math.cos(angle) * distance, y = height / 2 + Math.sin(angle) * distance
                            c.strokeStyle = ["#efad51", "#66bddb", "#a4d8bb"][i%3]
                            c.beginPath(); c.moveTo(x-3,y-3); c.lineTo(x+3,y+3); c.stroke()
                        }
                    }
                }
                ColumnLayout {
                    anchors.centerIn: parent; width: parent.width - 24; spacing: 12
                    Glyph { Layout.alignment: Qt.AlignHCenter; implicitWidth: 42; implicitHeight: 42; symbol: root.game.winner ? "trophy" : "close"; color: Theme.warning }
                    Label { text: root.game.winner ? (root.game.winner === root.identity ? qsTranslate("GamePane", "Victory") : qsTranslate("GamePane", "Defeat")) : qsTranslate("GameCard", "Game ended"); color: Theme.text; font.pixelSize: Theme.fontSize(26); font.weight: Font.Bold; Layout.alignment: Qt.AlignHCenter }
                    Item {
                        Layout.fillWidth: true; implicitHeight: resultsRow.implicitHeight
                    Row {
                        id: resultsRow
                        anchors.left: parent.left; anchors.right: parent.right; spacing: 16
                        Repeater {
                            model: root.players
                            Column {
                                id: resultPlayer
                                required property var modelData
                                width: Math.max(0, (resultsRow.width - resultsRow.spacing) / 2); spacing: 6
                                PlayerAvatar { anchors.horizontalCenter: parent.horizontalCenter; width: Math.min(90, root.height / 5); height: width; identity: resultPlayer.modelData.id; opacity: root.game.winner && root.game.winner !== identity ? 0.55 : 1 }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight; text: root.name(resultPlayer.modelData.id); textFormat: Text.PlainText; color: Theme.text; font.weight: resultPlayer.modelData.id === root.game.winner ? Font.Bold : Font.Normal }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: root.remaining(resultPlayer.modelData.id) + " / 14"; color: Theme.muted }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: qsTranslate("GamePane", "Remaining"); font.pixelSize: Theme.fontSize(11); color: Theme.muted }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "✦ " + root.captured(resultPlayer.modelData.id); color: Theme.accent }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: qsTranslate("GamePane", "Captured"); font.pixelSize: Theme.fontSize(11); color: Theme.muted }
                            }
                        }
                    }
                    }
                }
            }
        }
        Label { objectName: "boardStatus"; visible: false; text: root.statusText }
        Label { id: gameError; anchors.bottom: parent.bottom; width: parent.width; visible: !!root.actionError || !!root.game.error; text: root.actionError || root.game.error || ""; color: Theme.danger; textFormat: Text.PlainText; wrapMode: Text.Wrap; Layout.fillWidth: true }
    }
    Dialog {
        id: computerConfirmation; objectName: "gameComputerConfirmation"
        anchors.centerIn: parent; width: Math.min(340, root.width - 24); modal: true
        title: qsTranslate("GamePane", "Play against the computer?")
        standardButtons: Dialog.Yes | Dialog.No
        onAccepted: if (root.canAddComputer) root.act("computer")
    }
    Dialog {
        id: departure; objectName: "gameDepartureConfirmation"
        anchors.centerIn: parent; width: Math.max(240, Math.min(360, root.width - 32)); modal: true
        contentWidth: width - leftPadding - rightPadding
        title: qsTranslate("GamePane", "Leave game?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label { text: root.departureHasOpponent ? qsTranslate("GamePane", "Leaving gives the other player the win.") : qsTranslate("GamePane", "Cancel this invitation?"); color: Theme.text; wrapMode: Text.Wrap }
        onAccepted: { const action = root.pendingDeparture; root.pendingDeparture = null; root.cancelledDeparture = null; if (action) action() }
        onRejected: { const action = root.cancelledDeparture; root.pendingDeparture = null; root.cancelledDeparture = null; if (action) action() }
    }
    Dialog {
        id: help; objectName: "gameInformation"
        parent: Overlay.overlay; anchors.centerIn: parent
        width: Math.min(430, parent.width - 24)
        height: Math.min(implicitHeight, parent.height - 24)
        modal: true
        title: qsTranslate("GamePane", "Information")
        standardButtons: Dialog.Close
        contentItem: ScrollView {
            id: helpScroll; objectName: "gameHelpScroll"
            contentWidth: availableWidth; clip: true
            implicitHeight: helpContent.implicitHeight
            ColumnLayout {
                id: helpContent
                width: helpScroll.availableWidth
                spacing: 8
                ElementsIcon { objectName: "gameHelpElements"; Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 100; Layout.preferredHeight: 100 }
                Label { text: qsTranslate("GamePane", "Water beats Fire. Fire beats Ice. Ice beats Water."); color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
                GridLayout {
                    columns: 3; Layout.fillWidth: true
                    Repeater {
                        model: ["neutral","trap","core","water","fire","ice"]
                        Button {
                            id: helpPiece
                            required property string modelData
                            Layout.fillWidth: true; implicitHeight: 78; padding: 4; hoverEnabled: true
                            Accessible.name: board.label(modelData)
                            onClicked: { helpDetail.text = modelData === "neutral" ? qsTranslate("GamePane", "Choose its element in its first fight. Ties let you choose again.") : modelData === "trap" ? qsTranslate("GamePane", "Stays put. Defeats every attacker.") : modelData === "core" ? qsTranslate("GamePane", "Stays put. Protect it to stay in the game.") : modelData === "water" ? qsTranslate("GamePane", "Water extinguishes Fire") : modelData === "fire" ? qsTranslate("GamePane", "Fire melts Ice") : qsTranslate("GamePane", "Ice freezes Water"); root.feedbackRequested("piece-" + modelData) }
                            contentItem: ColumnLayout {
                                Image { source: board.art(helpPiece.modelData); sourceSize: Qt.size(128,128); fillMode: Image.PreserveAspectFit; Layout.preferredWidth: 48; Layout.preferredHeight: 48; Layout.alignment: Qt.AlignHCenter }
                                Label { text: board.label(helpPiece.modelData); color: Theme.text; Layout.alignment: Qt.AlignHCenter }
                            }
                        }
                    }
                }
                Label { id: helpDetail; text: qsTranslate("GamePane", "Choose Trap, then Core. Move one square. Capture the other Core to win."); color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
            }
        }
    }
}
