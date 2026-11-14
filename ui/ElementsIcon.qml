import QtQuick

Item {
    id: root
    implicitWidth: 48; implicitHeight: 48
    Accessible.role: Accessible.Graphic
    Accessible.name: qsTranslate("GameCard", "Elements")
    Canvas {
        anchors.fill: parent
        property color ink: Theme.muted
        onInkChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const c = getContext("2d")
            c.reset(); c.scale(width / 100, height / 100)
            c.strokeStyle = ink; c.fillStyle = ink; c.lineWidth = 3
            for (let i = 0; i < 3; ++i) {
                const start = -Math.PI / 2 + i * Math.PI * 2 / 3 + 0.55
                const end = start + Math.PI * 2 / 3 - 1.1
                c.beginPath(); c.arc(50, 50, 32, start, end); c.stroke()
                c.save(); c.translate(50 + Math.cos(end) * 32, 50 + Math.sin(end) * 32); c.rotate(end + Math.PI / 2)
                c.beginPath(); c.moveTo(5, 0); c.lineTo(-3, -4); c.lineTo(-3, 4); c.closePath(); c.fill(); c.restore()
            }
            // Water, fire, ice follow the same clockwise order as the rules.
            c.fillStyle = "#259bc9"; c.beginPath(); c.moveTo(50, 2)
            c.bezierCurveTo(44, 12, 38, 17, 38, 23); c.bezierCurveTo(38, 39, 62, 39, 62, 23)
            c.bezierCurveTo(62, 17, 56, 12, 50, 2); c.fill()
            c.fillStyle = "#f08332"; c.beginPath(); c.moveTo(81, 47)
            c.bezierCurveTo(84, 59, 96, 63, 94, 74); c.bezierCurveTo(92, 90, 66, 89, 65, 74)
            c.bezierCurveTo(64, 68, 68, 61, 72, 58); c.lineTo(74, 68)
            c.bezierCurveTo(79, 63, 82, 55, 81, 47); c.fill()
            c.strokeStyle = Theme.dark ? "#a9e4f1" : "#4d93af"; c.lineWidth = 3; c.lineCap = "round"
            for (let i = 0; i < 6; ++i) {
                c.save(); c.translate(22, 74); c.rotate(i * Math.PI / 3)
                c.beginPath(); c.moveTo(0,0); c.lineTo(0,-17); c.moveTo(-5,-13); c.lineTo(0,-9); c.lineTo(5,-13); c.stroke(); c.restore()
            }
        }
    }
}
