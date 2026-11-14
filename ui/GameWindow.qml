pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Window

PanelWindow {
    id: root
    objectName: "gameWindow"
    required property var network
    property string hostId: ""
    property string gameId: ""
    property var game: ({})
    property string identity: ""
    property int selected: -1
    property int core: -1
    property int trap: -1
    property string placing: "core"
    property double readyDeadline: 0
    property double now: Date.now()
    property string shownBattle: ""
    property string actionError: ""
    signal feedbackRequested(string kind)
    readonly property var duel: game.duel || ({})
    readonly property var attacker: (game.board || [])[duel.from] || ({})
    readonly property var defender: (game.board || [])[duel.to] || ({})
    readonly property var fighter: attacker.owner === identity ? attacker : defender.owner === identity ? defender : ({})
    readonly property bool choosing: game.phase === "duel" && participant && (fighter.kind === "neutral"
        || (["fire", "water", "ice"].includes(fighter.kind) && attacker.kind === defender.kind))
    readonly property var players: game.players || []
    readonly property var me: players.find(p => p.id === identity) || ({})
    readonly property bool participant: !!me.id
    readonly property bool playing: ["waiting", "preparing", "playing", "duel"].includes(game.phase)
    readonly property bool settingUp: participant && !me.ready && (game.phase === "waiting" || game.phase === "preparing")
    readonly property bool myTurn: game.phase === "playing" && game.turn === identity
    readonly property bool flipped: players.length > 1 && players[1].id === identity
    readonly property int seconds: Math.max(0, Math.ceil((readyDeadline - now) / 1000))
    title: qsTranslate("GameCard", "Elements") + " · SquadSpeak"
    width: Math.min(700, Screen.desktopAvailableWidth || 700)
    height: Math.min(800, Screen.desktopAvailableHeight || 800)
    minimumWidth: 340; minimumHeight: 480
    visible: false
    function name(id) { return (players.find(p => p.id === id) || {}).name || "" }
    function label(kind) {
        return kind === "fire" ? qsTr("Fire") : kind === "water" ? qsTr("Water") : kind === "ice" ? qsTr("Ice")
            : kind === "core" ? qsTr("Core") : kind === "trap" ? qsTr("Trap") : kind === "neutral" ? qsTr("Neutral") : qsTr("Unknown")
    }
    function act(action, values) {
        const selection = Object.assign({id: gameId, round: game.round || 0}, values || {})
        actionError = network.gameAction(action, selection, hostId, identity) ? "" : qsTr("That game action is no longer available.")
    }
    function refresh() {
        if (!gameId) return
        const next = network.gameView(hostId, gameId, identity)
        now = Date.now()
        if (next.id !== game.id || next.readyMs !== game.readyMs || next.phase !== game.phase)
            readyDeadline = now + (next.readyMs || 0)
        const previous = game
        game = next
        if (game.setup) { core = game.core; trap = game.trap }
        if (!myTurn) selected = -1
        const battle = game.lastBattle || ({})
        const outcome = JSON.stringify(battle)
        const newBattle = battle.number && outcome !== shownBattle
        if (newBattle && visible && !Theme.reducedMotion && !["tie", "choose"].includes(battle.effect)) effect.restart()
        if (visible && participant && previous.id === game.id) {
            if (game.phase !== previous.phase && ["finished", "cancelled"].includes(game.phase))
                feedbackRequested(game.phase === "finished" ? "game-win" : "game-end")
            else if (newBattle) {
                if (["tie", "choose"].includes(battle.effect)) { if (choosing) feedbackRequested("game-choose") }
                else feedbackRequested("game-" + battle.effect)
            } else if (game.phase === "playing" && (previous.phase !== "playing" || game.round !== previous.round))
                feedbackRequested("game-move")
        }
        shownBattle = outcome
    }
    function openGame(host, id) {
        if (gameId !== id || hostId !== host) {
            selected = -1; core = -1; trap = -1; placing = "core"; shownBattle = ""
        }
        hostId = host; gameId = id
        identity = network.remoteMode ? network.remoteView.ownId : network.ownId
        refresh(); if (Qt.platform.os === "ios" || Qt.platform.os === "android") showFullScreen(); else show(); raise(); requestActivate()
    }
    function pick(cell) {
        const piece = (game.board || [])[cell] || ({})
        if (settingUp) {
            if (piece.owner !== identity) return
            if (placing === "core") { core = cell; if (trap === cell) trap = -1; placing = "trap" }
            else { trap = cell; if (core === cell) core = -1; placing = "core" }
            if (core >= 0 && trap >= 0) act("setup", {core: core, trap: trap})
        } else if (myTurn) {
            if (piece.owner === identity && ["neutral", "fire", "water", "ice"].includes(piece.kind)) selected = cell
            else if (legal(cell)) { act("move", {from: selected, to: cell}); selected = -1 }
        }
    }
    function focusCell(index) {
        const cell = boardCells.itemAt(index)
        if (cell && cell.enabled) cell.forceActiveFocus(Qt.TabFocusReason)
    }
    function legal(cell) {
        return selected >= 0 && myTurn && ((game.board || [])[cell] || {}).owner !== identity
            && Math.abs(cell % 7 - selected % 7) + Math.abs(Math.floor(cell / 7) - Math.floor(selected / 7)) === 1
    }
    onClosing: event => { if (playing && participant) { act("leave"); feedbackRequested("game-end") } }
    onVisibleChanged: if (!visible) effect.stop()
    Connections {
        target: root.network
        function onChatChanged() { root.refresh() }
        function onRemoteChanged() { root.refresh() }
    }
    Timer { interval: 200; repeat: true; running: root.visible && root.settingUp && !!root.game.setup; onTriggered: root.now = Date.now() }
    header: RowLayout {
        spacing: 8
        Label { text: root.players.map(p => p.name).join("  ·  "); textFormat: Text.PlainText; color: Theme.text; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true; Layout.margins: 12 }
    }
    ScrollView {
        id: gameScroll
        anchors.fill: parent
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width; spacing: 12
            ColumnLayout {
                id: introduction
                Layout.fillWidth: true; spacing: 12
                RowLayout {
                    Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16; spacing: 8
                    Label {
                        id: status; objectName: "boardStatus"
                        Layout.fillWidth: true
                        color: Theme.text; wrapMode: Text.Wrap; textFormat: Text.PlainText
                        text: !root.game.id ? qsTr("Game unavailable")
                            : root.game.phase === "cancelled" ? (root.game.reason === "disconnected" ? qsTranslate("GameCard", "Game ended. A player disconnected.") : qsTranslate("GameCard", "Game ended. A player left."))
                            : root.game.phase === "finished" ? qsTranslate("GameCard", "%1 wins!").arg(root.name(root.game.winner))
                            : root.settingUp ? (root.game.setup ? qsTr("Ready in %1 s").arg(root.seconds) : qsTr("Choose your Core and Trap."))
                            : root.game.phase === "duel" ? (!root.choosing || root.game.choice ? qsTr("Waiting for the other player")
                                : (root.game.lastBattle || {}).effect === "tie" ? qsTr("Same element. Choose again.") : qsTr("Choose an element"))
                            : root.myTurn ? qsTr("Your turn") : root.game.phase === "playing" ? qsTr("%1's turn").arg(root.name(root.game.turn)) : qsTr("Waiting for the other player")
                    }
                    Item {
                        id: cycle
                        objectName: "elementCycle"
                        Layout.preferredWidth: 132; Layout.preferredHeight: 116
                        LayoutMirroring.enabled: false; LayoutMirroring.childrenInherit: true
                        Accessible.role: Accessible.Grouping
                        Accessible.name: qsTr("Water beats Fire. Fire beats Ice. Ice beats Water.")
                        Canvas {
                            anchors.fill: parent
                            property color ink: Theme.muted
                            onInkChanged: requestPaint()
                            onPaint: {
                                const c = getContext("2d")
                                c.reset(); c.strokeStyle = ink; c.fillStyle = ink; c.lineWidth = 1.5
                                for (let i = 0; i < 3; ++i) {
                                    const start = -Math.PI / 2 + i * Math.PI * 2 / 3 + 0.65
                                    const end = start + Math.PI * 2 / 3 - 1.3
                                    c.beginPath(); c.arc(66, 58, 39, start, end); c.stroke()
                                    const x = 66 + Math.cos(end) * 39, y = 58 + Math.sin(end) * 39
                                    c.save(); c.translate(x, y); c.rotate(end + Math.PI / 2)
                                    c.beginPath(); c.moveTo(3, 0); c.lineTo(-4, -3); c.lineTo(-4, 3); c.closePath(); c.fill(); c.restore()
                                }
                            }
                        }
                        Repeater {
                            model: ["water", "fire", "ice"]
                            Button {
                                id: elementButton
                                required property int index
                                required property string modelData
                                objectName: "gameChoice_" + modelData
                                x: 42 + Math.cos(-Math.PI / 2 + index * Math.PI * 2 / 3) * 39
                                y: 34 + Math.sin(-Math.PI / 2 + index * Math.PI * 2 / 3) * 34
                                width: 48; height: 48; padding: 3; hoverEnabled: true
                                enabled: root.choosing && !!root.game.connected && !root.game.choice
                                opacity: root.game.choice && root.game.choice !== modelData ? 0.5 : 1
                                Accessible.name: root.label(modelData)
                                onClicked: root.act("choose", {choice: modelData})
                                background: Rectangle {
                                    radius: 24
                                    color: elementButton.hovered || elementButton.activeFocus || root.game.choice === elementButton.modelData ? Theme.raised : "transparent"
                                    border.width: root.choosing || elementButton.activeFocus ? 1 : 0
                                    border.color: root.game.choice === elementButton.modelData || elementButton.activeFocus ? Theme.accent : Theme.muted
                                }
                                contentItem: Image {
                                    source: Qt.resolvedUrl("games/elements/" + elementButton.modelData + ".png")
                                    sourceSize: Qt.size(128, 128); fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true
                                }
                                TextToolTip { parent: elementButton; text: root.label(elementButton.modelData); visible: elementButton.hovered || elementButton.activeFocus }
                            }
                        }
                    }
                }
                RowLayout {
                    visible: root.settingUp; Layout.alignment: Qt.AlignHCenter
                    ActionButton { objectName: "placeCore"; text: qsTr("Core"); checked: root.placing === "core"; onClicked: root.placing = "core" }
                    ActionButton { objectName: "placeTrap"; text: qsTr("Trap"); checked: root.placing === "trap"; onClicked: root.placing = "trap" }
                    ActionButton { objectName: "gameReady"; text: qsTranslate("GameCard", "Ready"); primary: true; enabled: !!root.game.setup && !!root.game.connected; onClicked: root.act("ready") }
                }
            }
            Item {
                id: boardArea
                objectName: "gameBoard"
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: Math.min(root.width - 24, 650,
                    Math.max(320, (gameScroll.availableHeight - introduction.implicitHeight - resultControls.implicitHeight - 24) * 7 / 6))
                Layout.preferredHeight: width * 6 / 7
                LayoutMirroring.enabled: false; LayoutMirroring.childrenInherit: true
                Grid {
                    anchors.fill: parent; columns: 7; spacing: 2
                    Repeater {
                        id: boardCells
                        model: 42
                        Button {
                            id: cellButton
                            required property int index
                            readonly property int cell: root.flipped ? 41 - index : index
                            readonly property var piece: (root.game.board || [])[cell] || ({})
                            readonly property bool own: piece.owner === root.identity
                            readonly property bool fighting: root.game.phase === "duel" && (cell === root.duel.from || cell === root.duel.to)
                            readonly property string kind: root.settingUp && own && cell === root.core ? "core"
                                : root.settingUp && own && cell === root.trap ? "trap" : piece.kind || ""
                            objectName: "gameCell_" + cell
                            width: (boardArea.width - 12) / 7; height: width
                            padding: 3; hoverEnabled: true
                            enabled: !!root.game.connected && (root.settingUp ? own : root.myTurn)
                            opacity: 1
                            Accessible.name: String.fromCharCode(65 + cell % 7) + (Math.floor(cell / 7) + 1)
                                + ": " + (piece.owner ? root.name(piece.owner) + ", " + root.label(kind) : qsTr("Empty"))
                            onClicked: root.pick(cell)
                            Keys.onLeftPressed: root.focusCell(index - 1)
                            Keys.onRightPressed: root.focusCell(index + 1)
                            Keys.onUpPressed: root.focusCell(index - 7)
                            Keys.onDownPressed: root.focusCell(index + 7)
                            background: Rectangle {
                                radius: Theme.controlRadius
                                color: (Math.floor(cellButton.cell / 7) + cellButton.cell % 7) % 2 ? Theme.surface : Theme.raised
                                border.color: root.selected === cellButton.cell || cellButton.activeFocus || cellButton.fighting ? Theme.accent
                                    : root.legal(cellButton.cell) ? Theme.accent : "transparent"
                                border.width: root.selected === cellButton.cell ? 3 : cellButton.fighting ? 2 : 1
                                Rectangle { anchors.centerIn: parent; width: 8; height: 8; radius: 4; visible: root.legal(cellButton.cell) && !cellButton.piece.owner; color: Theme.accent }
                            }
                            contentItem: Item {
                                Canvas {
                                    anchors.fill: parent; visible: !!cellButton.kind
                                    onWidthChanged: requestPaint()
                                    onHeightChanged: requestPaint()
                                    onPaint: {
                                        const c = getContext("2d")
                                        c.reset(); c.save()
                                        c.translate(width / 2, height * 0.88); c.scale(width * 0.36, height * 0.08)
                                        const shade = c.createRadialGradient(0, 0, 0, 0, 0, 1)
                                        shade.addColorStop(0, "rgba(0, 0, 0, 0.30)"); shade.addColorStop(1, "transparent")
                                        c.fillStyle = shade; c.fillRect(-1, -1, 2, 2); c.restore()
                                    }
                                }
                                Image {
                                    id: pieceArt
                                    objectName: "gamePiece_" + cellButton.cell
                                    anchors.fill: parent
                                    source: !cellButton.kind ? "" : Qt.resolvedUrl("games/elements/"
                                        + (["fire", "water", "ice", "core", "trap"].includes(cellButton.kind) ? cellButton.kind : "hidden") + ".png")
                                    sourceSize: Qt.size(192, 192)
                                    fillMode: Image.PreserveAspectFit
                                    asynchronous: true; smooth: true; mipmap: true
                                    scale: root.selected === cellButton.cell ? 1.06 : 1
                                    Behavior on scale { NumberAnimation { duration: Theme.reducedMotion ? 0 : 120 } }
                                }
                                Label { anchors.bottom: parent.bottom; anchors.right: parent.right; text: cellButton.own ? "●" : "◇"; color: Theme.text; font.pixelSize: 11; visible: !!cellButton.piece.owner }
                            }
                            TextToolTip { parent: cellButton; text: cellButton.Accessible.name; visible: cellButton.hovered || cellButton.activeFocus }
                        }
                    }
                }
                Image {
                    id: defeated
                    readonly property var battle: root.game.lastBattle || ({})
                    readonly property bool lostAttack: battle.winner !== battle.attacker
                    readonly property string kind: (lostAttack ? battle.attack : battle.defence) || "hidden"
                    readonly property int cell: lostAttack ? battle.from || 0 : battle.to || 0
                    readonly property int tile: root.flipped ? 41 - cell : cell
                    width: boardArea.width / 7; height: width
                    x: tile % 7 * width; y: Math.floor(tile / 7) * height
                    visible: effect.running
                    source: visible ? Qt.resolvedUrl("games/elements/" + (["fire", "water", "ice", "core", "trap"].includes(kind) ? kind : "hidden") + ".png") : ""
                    sourceSize: Qt.size(192, 192); fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true
                    opacity: 1 - burst.progress
                    transform: Scale {
                        origin.x: defeated.width / 2; origin.y: defeated.height
                        xScale: defeated.battle.effect === "melt" ? 1 + burst.progress * 0.15 : 1
                        yScale: defeated.battle.effect === "melt" ? 1 - burst.progress : 1
                    }
                }
                Canvas {
                    id: burst
                    objectName: "battleEffect"
                    anchors.fill: parent
                    property real progress: 1
                    visible: effect.running
                    onProgressChanged: requestPaint()
                    NumberAnimation { id: effect; target: burst; property: "progress"; from: 0; to: 1; duration: 600 }
                    onPaint: {
                        const c = getContext("2d"), b = root.game.lastBattle || ({})
                        c.reset()
                        const tile = defeated.tile
                        const x = (tile % 7 + 0.5) * width / 7, y = (Math.floor(tile / 7) + 0.5) * height / 6
                        c.globalAlpha = 1 - progress
                        c.strokeStyle = b.effect === "melt" ? "#ee9958" : b.effect === "freeze" ? "#b3e9eb"
                            : b.effect === "trap" ? "#96bf64" : b.effect === "capture" ? "#ffd37b" : "#66b6e6"
                        c.lineWidth = 3
                        if (b.effect === "freeze") {
                            c.fillStyle = "rgba(170, 225, 255, 0.35)"
                            const radius = width / 7 * (0.3 + progress * 0.1)
                            c.beginPath()
                            for (let i = 0; i < 6; ++i) {
                                const angle = i * Math.PI / 3
                                const px = x + Math.cos(angle) * radius, py = y + Math.sin(angle) * radius
                                if (i === 0) c.moveTo(px, py); else c.lineTo(px, py)
                            }
                            c.closePath(); c.fill()
                        }
                        for (let i = 0; i < 8; ++i) {
                            const angle = Math.PI * i / 4, distance = (8 + progress * 26) * width / 500
                            c.beginPath()
                            if (b.effect === "freeze") {
                                const px = x + Math.cos(angle) * distance, py = y + Math.sin(angle) * distance
                                c.moveTo(x, y); c.lineTo(px, py); c.lineTo(px + Math.cos(angle + 2) * 7, py + Math.sin(angle + 2) * 7)
                                c.moveTo(px, py); c.lineTo(px + Math.cos(angle - 2) * 7, py + Math.sin(angle - 2) * 7)
                            } else if (b.effect === "melt") {
                                const px = x + Math.cos(angle) * 18, py = y + Math.sin(angle) * 12 + progress * 32
                                c.moveTo(px, py - 5); c.quadraticCurveTo(px + 6, py + 4, px, py + 5); c.quadraticCurveTo(px - 6, py + 4, px, py - 5)
                            } else if (b.effect === "capture" || b.effect === "trap") {
                                const spread = b.effect === "trap" ? 30 * (1 - progress) : distance
                                const px = x + Math.cos(angle) * spread, py = y + Math.sin(angle) * spread
                                c.moveTo(px - 3, py); c.lineTo(px + 3, py); c.moveTo(px, py - 3); c.lineTo(px, py + 3)
                            } else {
                                const px = x + Math.cos(angle) * distance, py = y - progress * 30 + Math.sin(angle) * 12
                                c.moveTo(px, py); c.bezierCurveTo(px - 8, py - 5, px + 8, py - 10, px, py - 16)
                            }
                            c.stroke()
                        }
                    }
                }
            }
            ColumnLayout {
                id: resultControls
                Layout.fillWidth: true; spacing: 12
                Label {
                    Layout.fillWidth: true; Layout.leftMargin: 16; Layout.rightMargin: 16
                    wrapMode: Text.Wrap; color: Theme.muted
                    id: battleSummary
                    objectName: "battleSummary"
                    readonly property string outcome: (root.game.lastBattle || {}).effect || ""
                    visible: !!battleSummary.outcome && !["tie", "choose"].includes(battleSummary.outcome)
                    text: battleSummary.outcome === "freeze" ? qsTr("Ice freezes Water") : battleSummary.outcome === "melt" ? qsTr("Fire melts Ice")
                        : battleSummary.outcome === "extinguish" ? qsTr("Water extinguishes Fire") : battleSummary.outcome === "trap" ? qsTr("Trap triggered")
                        : battleSummary.outcome === "capture" ? qsTr("Core captured") : qsTr("Same element. Choose again.")
                }
                Label {
                    visible: !!root.game.error || !!root.actionError
                    text: root.actionError || root.game.error || ""; textFormat: Text.PlainText; color: Theme.danger
                    Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 12; wrapMode: Text.Wrap
                }
            }
        }
    }
    footer: RowLayout {
        spacing: 8
        Item { Layout.fillWidth: true }
        ActionButton {
            objectName: "gameLeave"; text: root.playing && root.participant ? qsTranslate("GameCard", "Leave") : qsTranslate("Channels", "Close")
            Layout.margins: 8
            onClicked: root.close()
        }
    }
}
