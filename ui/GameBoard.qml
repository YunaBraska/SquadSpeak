pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root
    required property var game
    required property string identity
    readonly property color selfColor: "#348ddb"
    readonly property color opponentColor: "#df5c65"
    property int core: -1
    property int trap: -1
    property bool settingUp: false
    property int selected: -1
    property bool myTurn: false
    property bool choosing: false
    property var previous: ({})
    property var motion: ({})
    property real progress: 1
    readonly property bool effectRunning: travel.running
    readonly property bool flipped: (game.players || []).length > 1 && game.players[1].id === identity
    readonly property real tileSize: width / 7
    readonly property var duel: game.duel || ({})
    readonly property var fighter: ((game.board || [])[duel.from] || {}).owner === identity ? duel.from : duel.to
    signal cellPicked(int cell)
    signal elementPicked(string kind)
    signal pieceHeard(string kind)
    implicitWidth: 560; implicitHeight: width * 6 / 7
    LayoutMirroring.enabled: false; LayoutMirroring.childrenInherit: true
    function tile(cell) { return flipped ? 41 - cell : cell }
    function point(cell) { const t = tile(cell); return Qt.point(t % 7 * tileSize, Math.floor(t / 7) * tileSize) }
    function art(kind) { return Qt.resolvedUrl("games/elements/" + (["fire", "water", "ice", "core", "trap"].includes(kind) ? kind : "hidden") + ".png") }
    function label(kind) {
        return kind === "fire" ? qsTranslate("GamePane", "Fire") : kind === "water" ? qsTranslate("GamePane", "Water")
            : kind === "ice" ? qsTranslate("GamePane", "Ice") : kind === "core" ? qsTranslate("GamePane", "Core")
            : kind === "trap" ? qsTranslate("GamePane", "Trap") : kind === "neutral" ? qsTranslate("GamePane", "Neutral") : qsTranslate("GamePane", "Unknown")
    }
    function legal(cell) {
        return selected >= 0 && myTurn && ((game.board || [])[cell] || {}).owner !== identity
            && Math.abs(cell % 7 - selected % 7) + Math.abs(Math.floor(cell / 7) - Math.floor(selected / 7)) === 1
    }
    onGameChanged: {
        const before = previous
        previous = game
        if (!visible || before.id !== game.id || !before.board) { travel.stop(); motion = ({}); return }
        const battle = game.lastBattle || ({})
        let next = ({})
        if (battle.number && JSON.stringify(battle) !== JSON.stringify(before.lastBattle || ({}))) {
            next = Object.assign({battle: true, pending: ["choose", "tie"].includes(battle.effect)}, battle)
        } else if (game.round !== before.round && game.phase === "playing") {
            const from = before.board.findIndex((p, i) => p.owner && !(game.board[i] || {}).owner)
            const to = game.board.findIndex((p, i) => p.owner && !(before.board[i] || {}).owner)
            if (from >= 0 && to >= 0) next = {from: from, to: to, attack: game.board[to].kind, attacker: game.board[to].owner, winner: game.board[to].owner, battle: false}
        }
        if (next.from === undefined) return
        travel.stop(); motion = next; progress = 0; travel.start()
    }
    onVisibleChanged: if (!visible) { travel.stop(); motion = ({}) }
    NumberAnimation {
        id: travel
        target: root; property: "progress"; from: 0; to: 1
        duration: Theme.reducedMotion ? (root.motion.battle ? 700 : 120) : root.motion.battle ? (root.motion.pending ? 420 : 1050) : 240
        easing.type: Easing.Linear
    }
    Rectangle {
        anchors.fill: parent; anchors.margins: -5
        radius: 10; color: Theme.dark ? "#243b39" : "#ae9870"
        border.color: Theme.dark ? "#a39265" : "#8f7951"; border.width: 2
    }
    Grid {
        anchors.fill: parent; columns: 7
        Repeater {
            id: cells
            model: 42
            Button {
                id: cellButton
                required property int index
                readonly property int cell: root.tile(index)
                readonly property var piece: (root.game.board || [])[cell] || ({})
                readonly property bool own: piece.owner === root.identity
                readonly property bool candidate: root.settingUp && own && root.core < 0 && (root.trap < 0 || cell !== root.trap)
                readonly property string placement: root.trap < 0 ? "trap" : "core"
                readonly property string kind: root.settingUp && own ? (cell === root.trap ? "trap" : cell === root.core ? "core"
                    : candidate && (hovered || activeFocus) ? placement : "neutral") : piece.kind || ""
                readonly property bool fighting: root.game.phase === "duel" && (cell === root.duel.from || cell === root.duel.to)
                readonly property bool hiddenForMotion: root.effectRunning && (cell === root.motion.from || cell === root.motion.to)
                objectName: "gameCell_" + cell
                width: root.tileSize; height: width; padding: 2; hoverEnabled: true
                enabled: !!root.game.connected && !root.effectRunning && (root.settingUp ? candidate : root.myTurn)
                opacity: 1
                Accessible.name: String.fromCharCode(65 + cell % 7) + (Math.floor(cell / 7) + 1) + ": "
                    + (piece.owner ? ((root.game.players || []).find(p => p.id === piece.owner) || {}).name + ", " + root.label(kind) : qsTranslate("GamePane", "Empty"))
                onClicked: root.cellPicked(cell)
                Keys.onLeftPressed: { const next = cells.itemAt(index - 1); if (next && next.enabled) next.forceActiveFocus() }
                Keys.onRightPressed: { const next = cells.itemAt(index + 1); if (next && next.enabled) next.forceActiveFocus() }
                Keys.onUpPressed: { const next = cells.itemAt(index - 7); if (next && next.enabled) next.forceActiveFocus() }
                Keys.onDownPressed: { const next = cells.itemAt(index + 7); if (next && next.enabled) next.forceActiveFocus() }
                background: Rectangle {
                    color: (Math.floor(cellButton.cell / 7) + cellButton.cell % 7) % 2 ? (Theme.dark ? "#253e3e" : "#d8cfb4") : (Theme.dark ? "#34514b" : "#eee4c9")
                    border.color: root.selected === cellButton.cell || cellButton.fighting || cellButton.activeFocus || (cellButton.candidate && cellButton.hovered) ? Theme.accent : Qt.rgba(0, 0, 0, 0.13)
                    border.width: root.selected === cellButton.cell || cellButton.fighting || (cellButton.candidate && cellButton.hovered) ? 3 : 1
                    Rectangle { anchors.centerIn: parent; width: 9; height: 9; radius: 5; visible: root.legal(cellButton.cell) && !cellButton.piece.owner; color: Theme.accent }
                }
                contentItem: Item {
                    opacity: cellButton.hiddenForMotion ? 0 : 1
                    Rectangle {
                        objectName: "gameTeam_" + cellButton.cell
                        anchors.horizontalCenter: parent.horizontalCenter; y: parent.height * 0.80
                        width: parent.width * 0.76; height: parent.height * 0.17
                        radius: cellButton.own ? height / 2 : 2
                        color: cellButton.own ? root.selfColor : root.opponentColor
                        border.color: Qt.darker(color, 1.4); border.width: 1
                        visible: !!cellButton.kind
                    }
                    Image {
                        objectName: "gamePiece_" + cellButton.cell
                        anchors.fill: parent
                        source: cellButton.kind ? root.art(cellButton.kind) : ""
                        sourceSize: Qt.size(256, 256); fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true
                        scale: root.selected === cellButton.cell || (cellButton.candidate && cellButton.hovered) ? 1.05 : 1
                        Behavior on scale { NumberAnimation { duration: Theme.reducedMotion ? 0 : 100 } }
                    }
                }
            }
        }
    }
    Item {
        id: encounter
        objectName: "battleEffect"
        anchors.fill: parent; visible: root.effectRunning
        readonly property point start: root.point(root.motion.from || 0)
        readonly property point end: root.point(root.motion.to || 0)
        readonly property bool attackerWins: root.motion.winner === root.motion.attacker
        readonly property real approach: Theme.reducedMotion ? 0 : root.motion.battle ? (root.motion.pending ? Math.sin(root.progress * Math.PI) * 0.28 : Math.min(1, root.progress / 0.3) * 0.62) : root.progress * root.progress * (3 - 2 * root.progress)
        readonly property real impact: Math.max(0, (root.progress - 0.25) / 0.75)
        Image {
            width: root.tileSize; height: width
            x: Theme.reducedMotion && !root.motion.battle ? encounter.end.x : encounter.start.x + (encounter.end.x - encounter.start.x) * encounter.approach
                + (!Theme.reducedMotion && root.motion.battle && !root.motion.pending && encounter.attackerWins ? (encounter.end.x - encounter.start.x) * 0.38 * encounter.impact : 0)
            y: Theme.reducedMotion && !root.motion.battle ? encounter.end.y : encounter.start.y + (encounter.end.y - encounter.start.y) * encounter.approach
                + (!Theme.reducedMotion && root.motion.battle && !root.motion.pending && encounter.attackerWins ? (encounter.end.y - encounter.start.y) * 0.38 * encounter.impact : 0)
            source: visible ? root.art(root.motion.attack) : ""
            sourceSize: Qt.size(256, 256); fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true
            opacity: Theme.reducedMotion && !root.motion.battle ? root.progress : root.motion.battle && !root.motion.pending && !encounter.attackerWins ? 1 - encounter.impact : 1
            rotation: !Theme.reducedMotion && root.motion.battle ? Math.sin(root.progress * Math.PI * 4) * 5 * (1 - root.progress) : 0
        }
        Image {
            id: victim
            visible: !!root.motion.battle
            width: root.tileSize; height: width; x: encounter.end.x; y: encounter.end.y
            source: visible ? root.art(root.motion.defence) : ""
            sourceSize: Qt.size(256, 256); fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true
            opacity: !root.motion.pending && encounter.attackerWins ? 1 - encounter.impact : 1
            transform: Scale { origin.x: victim.width / 2; origin.y: victim.height; yScale: !Theme.reducedMotion && root.motion.effect === "melt" && encounter.attackerWins ? 1 - encounter.impact * 0.9 : 1 }
        }
        Canvas {
            anchors.fill: parent; visible: !!root.motion.battle
            property real frame: root.progress
            onFrameChanged: requestPaint()
            onPaint: {
                const c = getContext("2d"); c.reset()
                const m = root.motion, p = root.progress, s = root.tileSize
                const center = root.point(m.to || 0), x = center.x + s / 2, y = center.y + s / 2
                const ink = m.effect === "freeze" ? "#b8eeff" : m.effect === "melt" ? "#ff983a"
                    : m.effect === "extinguish" ? "#69ccff" : m.effect === "trap" ? "#b0dc5b" : m.effect === "capture" ? "#ffe298" : "#f4dfad"
                const strength = Math.sin(p * Math.PI)
                c.globalAlpha = strength; c.fillStyle = ink; c.strokeStyle = ink; c.lineWidth = 2
                const halo = c.createRadialGradient(x, y, 0, x, y, s * 0.95)
                halo.addColorStop(0, ink); halo.addColorStop(1, "transparent")
                c.globalAlpha = strength * 0.5; c.fillStyle = halo; c.fillRect(x-s, y-s, s*2, s*2)
                c.globalAlpha = strength; c.fillStyle = ink
                for (let i=0; i<14; ++i) {
                    const a = i * Math.PI * 2 / 14, distance = s * (Theme.reducedMotion ? 0.4 : 0.15 + p * 0.75)
                    const px = x + Math.cos(a) * distance, py = y + Math.sin(a) * distance
                    c.beginPath()
                    if (m.effect === "freeze") {
                        c.moveTo(px, py-s*0.08); c.lineTo(px+s*0.035, py); c.lineTo(px, py+s*0.08); c.lineTo(px-s*0.035, py); c.closePath(); c.fill()
                    } else if (m.effect === "extinguish" || m.pending) {
                        c.arc(px, py - p*s*0.12, s*(0.025+p*0.04), 0, Math.PI*2); c.fill()
                    } else {
                        c.moveTo(px-s*0.04, py); c.lineTo(px+s*0.04, py); c.moveTo(px, py-s*0.04); c.lineTo(px, py+s*0.04); c.stroke()
                    }
                }
            }
        }
    }
    Rectangle {
        id: choices
        objectName: "elementChoices"
        visible: root.choosing
        readonly property point anchor: root.point(root.fighter || 0)
        width: Math.min(root.width, 174); height: 62; radius: 24
        x: Math.max(0, Math.min(root.width - width, anchor.x + root.tileSize / 2 - width / 2))
        y: anchor.y >= height + 8 ? anchor.y - height - 6 : anchor.y + root.tileSize + 6
        color: Theme.surface; border.color: Theme.accent; border.width: 2
        Row {
            anchors.centerIn: parent; spacing: 4
            Repeater {
                model: ["water", "fire", "ice"]
                Button {
                    id: choiceButton
                    required property string modelData
                    objectName: "gameChoice_" + modelData
                    width: 50; height: 50; padding: 2; hoverEnabled: true
                    enabled: root.choosing && !!root.game.connected && !root.game.choice && !root.effectRunning
                    Accessible.name: root.label(modelData)
                    opacity: root.game.choice && root.game.choice !== modelData ? 0.35 : 1
                    onClicked: { root.pieceHeard(modelData); root.elementPicked(modelData) }
                    background: Rectangle { radius: 24; color: choiceButton.hovered || choiceButton.activeFocus || root.game.choice === choiceButton.modelData ? Theme.raised : "transparent"; border.color: choiceButton.activeFocus ? Theme.accent : "transparent" }
                    contentItem: Image { source: root.art(choiceButton.modelData); sourceSize: Qt.size(128,128); fillMode: Image.PreserveAspectFit; smooth: true; mipmap: true }
                    TextToolTip { parent: choiceButton; text: root.label(choiceButton.modelData); visible: choiceButton.hovered || choiceButton.activeFocus }
                }
            }
        }
    }
}
