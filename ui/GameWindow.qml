pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window

PanelWindow {
    id: root
    objectName: "gameWindow"
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
    property bool closingApproved: false
    property var pendingDeparture: null
    property var cancelledDeparture: null
    property bool departureHasOpponent: false
    property var gameMembers: ({})
    signal feedbackRequested(string kind)
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
    readonly property var roster: (current.members || []).concat(current.chatMembers || [])
    readonly property string statusText: !game.id ? qsTr("Game unavailable")
        : game.phase === "finished" ? qsTranslate("GameCard", "%1 wins!").arg(name(game.winner))
        : game.phase === "cancelled" ? qsTranslate("GameCard", "Game ended")
        : settingUp ? (trap < 0 ? qsTr("Trap") : qsTr("Core"))
        : game.phase === "duel" ? (!choosing || game.choice ? qsTr("Waiting for the other player")
            : (game.lastBattle || {}).effect === "tie" ? qsTr("Same element. Choose again.") : qsTr("Choose an element"))
        : myTurn ? qsTr("Your turn") : game.phase === "playing" ? qsTr("%1's turn").arg(name(game.turn)) : qsTr("Waiting for the other player")
    title: qsTranslate("GameCard", "Elements") + " · SquadSpeak"
    width: Math.min(720, Screen.desktopAvailableWidth || 720)
    height: Math.min(800, Screen.desktopAvailableHeight || 800)
    minimumWidth: 340; minimumHeight: 480
    visible: false
    background: Rectangle {
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.dark ? "#132d32" : "#f4ecd9" }
            GradientStop { position: 1; color: Theme.dark ? "#102220" : "#dbe5d7" }
        }
    }
    function name(id) { return member(id).name || (players.find(p => p.id === id) || {}).name || "" }
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
        actionError = accepted ? "" : qsTr("That game action is no longer available.")
        return accepted
    }
    function refresh() {
        if (!gameId) return
        const previous = game
        game = network.gameView(hostId, gameId, identity)
        const known = Object.assign({}, gameMembers)
        for (const player of players) { const profile = roster.find(p => p.id === player.id); if (profile) known[player.id] = profile }
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
        if (closingApproved && !visible && participant && !playing) act("dismiss")
    }
    function openGame(host, id) {
        if (gameId !== id || hostId !== host) { selected = -1; core = -1; trap = -1; actionError = ""; gameMembers = ({}) }
        closingApproved = false
        hostId = host; gameId = id
        identity = network.remoteMode ? network.remoteView.ownId : network.ownId
        refresh()
        if (Qt.platform.os === "ios" || Qt.platform.os === "android") showFullScreen(); else show()
        raise(); requestActivate()
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
        show(); raise(); requestActivate(); if (!departure.opened) departure.open()
    }
    onClosing: event => {
        if (playing && participant && !closingApproved) {
            event.accepted = false
            confirmDeparture(function() {
                if (root.playing && !root.act("leave")) return
                root.feedbackRequested("game-end"); root.closingApproved = true; root.close()
            })
        } else if (!playing && participant) act("dismiss")
    }
    Connections {
        target: root.network
        function onChatChanged() { root.refresh() }
        function onRemoteChanged() { root.refresh() }
    }
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 16; spacing: 8
        RowLayout {
            Layout.fillWidth: true
            ActionButton { objectName: "gameInfo"; text: qsTr("Information"); glyph: "info"; iconOnly: true; flat: true; onClicked: help.open() }
            Item { Layout.fillWidth: true }
            ElementsIcon { Layout.preferredWidth: 42; Layout.preferredHeight: 42 }
            Item { Layout.fillWidth: true }
            ActionButton { objectName: "gameLeave"; text: root.playing ? qsTranslate("GameCard", "Leave") : qsTranslate("Channels", "Close"); glyph: "leave"; iconOnly: true; flat: true; onClicked: root.close() }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 12
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
                        Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 78; Layout.preferredHeight: 78
                        Rectangle { anchors.fill: parent; radius: width / 2; color: playerHud.highlighted ? Qt.rgba(Theme.accent.r,Theme.accent.g,Theme.accent.b,0.24) : "transparent"; border.color: playerHud.highlighted ? Theme.accent : Theme.border; border.width: playerHud.highlighted ? 3 : 1 }
                        VoiceAvatar { objectName: "gameAvatar_" + playerHud.index; anchors.fill: parent; anchors.margins: 5; visible: !!playerHud.player.id; name: playerHud.player.name || ""; identity: playerHud.player.id || ""; avatar: root.avatar(identity); circular: true; animated: false }
                        Glyph { anchors.centerIn: parent; symbol: "person"; color: Theme.muted; visible: !playerHud.player.id }
                        Glyph { anchors.right: parent.right; anchors.bottom: parent.bottom; symbol: "check"; color: Theme.accent; visible: !!playerHud.player.ready && ["waiting","preparing"].includes(root.game.phase) }
                    }
                    Label { text: root.name(playerHud.player.id); textFormat: Text.PlainText; font.weight: Font.DemiBold; color: Theme.text; elide: Text.ElideRight; Layout.fillWidth: true; horizontalAlignment: Text.AlignHCenter }
                    Accessible.role: Accessible.Grouping
                    Accessible.name: player.name || qsTr("Waiting for the other player")
                    Accessible.description: highlighted ? root.statusText : ""
                }
            }
        }
        Item {
            Layout.fillWidth: true; Layout.fillHeight: true
            GameBoard {
                id: board
                objectName: "gameBoard"
                anchors.centerIn: parent
                width: Math.min(parent.width - 10, parent.height * 7 / 6 - 10); height: width * 6 / 7
                game: root.game; identity: root.identity; core: root.core; trap: root.trap
                settingUp: root.settingUp; selected: root.selected; myTurn: root.myTurn; choosing: root.choosing
                onCellPicked: cell => root.pick(cell)
                onElementPicked: kind => root.act("choose", {choice: kind})
                onPieceHeard: kind => root.feedbackRequested("piece-" + kind)
                Accessible.role: Accessible.Grouping; Accessible.name: root.statusText
            }
            Image {
                visible: root.settingUp && root.core < 0
                anchors.horizontalCenter: parent.horizontalCenter; anchors.top: parent.top
                width: 46; height: width
                source: board.art(root.trap < 0 ? "trap" : "core"); sourceSize: Qt.size(128,128); fillMode: Image.PreserveAspectFit
                Accessible.name: root.trap < 0 ? qsTr("Trap") : qsTr("Core")
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
                    Label { text: root.game.winner ? (root.game.winner === root.identity ? qsTr("Victory") : qsTr("Defeat")) : qsTranslate("GameCard", "Game ended"); color: Theme.text; font.pixelSize: Theme.fontSize(26); font.weight: Font.Bold; Layout.alignment: Qt.AlignHCenter }
                    Row {
                        id: resultsRow
                        Layout.fillWidth: true; spacing: 16
                        Repeater {
                            model: root.players
                            Column {
                                id: resultPlayer
                                required property var modelData
                                width: (resultsRow.width - resultsRow.spacing) / 2; spacing: 6
                                VoiceAvatar { anchors.horizontalCenter: parent.horizontalCenter; width: 90; height: 90; name: resultPlayer.modelData.name; identity: resultPlayer.modelData.id; avatar: root.avatar(identity); circular: true; animated: false; opacity: root.game.winner && root.game.winner !== identity ? 0.55 : 1 }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight; text: resultPlayer.modelData.name; textFormat: Text.PlainText; color: Theme.text; font.weight: resultPlayer.modelData.id === root.game.winner ? Font.Bold : Font.Normal }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: root.remaining(resultPlayer.modelData.id) + " / 14"; color: Theme.muted }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: qsTr("Remaining"); font.pixelSize: Theme.fontSize(11); color: Theme.muted }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: "✦ " + root.captured(resultPlayer.modelData.id); color: Theme.accent }
                                Label { width: parent.width; horizontalAlignment: Text.AlignHCenter; text: qsTr("Captured"); font.pixelSize: Theme.fontSize(11); color: Theme.muted }
                            }
                        }
                    }
                }
            }
        }
        Label { objectName: "boardStatus"; visible: false; text: root.statusText }
        Label { visible: !!root.actionError || !!root.game.error; text: root.actionError || root.game.error || ""; color: Theme.danger; textFormat: Text.PlainText; wrapMode: Text.Wrap; Layout.fillWidth: true }
    }
    Dialog {
        id: departure; objectName: "gameDepartureConfirmation"
        anchors.centerIn: parent; width: Math.min(360, root.width - 32); modal: true
        title: qsTr("Leave game?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        contentItem: Label { text: root.departureHasOpponent ? qsTr("Leaving gives the other player the win.") : qsTr("Cancel this invitation?"); color: Theme.text; wrapMode: Text.Wrap }
        onAccepted: { const action = root.pendingDeparture; root.pendingDeparture = null; root.cancelledDeparture = null; if (action) action() }
        onRejected: { const action = root.cancelledDeparture; root.pendingDeparture = null; root.cancelledDeparture = null; if (action) action() }
    }
    Dialog {
        id: help; objectName: "gameInformation"
        anchors.centerIn: parent; width: Math.min(430, root.width - 24); modal: true
        title: qsTr("Information")
        standardButtons: Dialog.Close
        contentItem: ColumnLayout {
            spacing: 8
            ElementsIcon { Layout.alignment: Qt.AlignHCenter; Layout.preferredWidth: 100; Layout.preferredHeight: 100 }
            Label { text: qsTr("Water beats Fire. Fire beats Ice. Ice beats Water."); color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
            GridLayout {
                columns: 3; Layout.fillWidth: true
                Repeater {
                    model: ["neutral","trap","core","water","fire","ice"]
                    Button {
                        id: helpPiece
                        required property string modelData
                        Layout.fillWidth: true; implicitHeight: 78; padding: 4; hoverEnabled: true
                        Accessible.name: board.label(modelData)
                        onClicked: { helpDetail.text = modelData === "neutral" ? qsTr("Choose its element in its first fight. Ties let you choose again.") : modelData === "trap" ? qsTr("Stays put. Defeats every attacker.") : modelData === "core" ? qsTr("Stays put. Protect it to stay in the game.") : modelData === "water" ? qsTr("Water extinguishes Fire") : modelData === "fire" ? qsTr("Fire melts Ice") : qsTr("Ice freezes Water"); root.feedbackRequested("piece-" + modelData) }
                        contentItem: ColumnLayout {
                            Image { source: board.art(helpPiece.modelData); sourceSize: Qt.size(128,128); fillMode: Image.PreserveAspectFit; Layout.preferredWidth: 48; Layout.preferredHeight: 48; Layout.alignment: Qt.AlignHCenter }
                            Label { text: board.label(helpPiece.modelData); color: Theme.text; Layout.alignment: Qt.AlignHCenter }
                        }
                    }
                }
            }
            Label { id: helpDetail; text: qsTr("Choose Trap, then Core. Move one square. Capture the other Core to win."); color: Theme.text; wrapMode: Text.Wrap; Layout.fillWidth: true }
        }
    }
}
