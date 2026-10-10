import QtQuick
import QtQuick.Controls.Basic

Canvas {
    id: root
    property string symbol: "audio"
    property string description: ""
    Accessible.role: Accessible.Graphic
    Accessible.name: description
    ToolTip.text: description
    ToolTip.visible: description.length > 0 && iconHover.hovered
    ToolTip.delay: 450
    HoverHandler { id: iconHover }
    property color color: Theme.text
    implicitWidth: 18
    implicitHeight: 18
    onSymbolChanged: requestPaint()
    onColorChanged: requestPaint()
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const c = getContext("2d")
        c.reset(); c.scale(width / 24, height / 24)
        c.strokeStyle = color; c.fillStyle = color; c.lineWidth = 1.6
        c.lineCap = "round"; c.lineJoin = "round"
        const line = function(points) {
            c.beginPath(); c.moveTo(points[0], points[1])
            for (let i = 2; i < points.length; i += 2) c.lineTo(points[i], points[i + 1])
            c.stroke()
        }
        if (symbol === "mic" || symbol === "mute") {
            c.beginPath(); c.roundedRect(9, 3, 6, 12, 3, 3); c.stroke()
            c.beginPath(); c.arc(12, 12, 7, 0, Math.PI); c.stroke()
            line([12, 19, 12, 22]); line([8, 22, 16, 22])
            if (symbol === "mute") line([3, 3, 21, 21])
        } else if (symbol === "speaker" || symbol === "deafen") {
            line([3, 9, 7, 9, 13, 4, 13, 20, 7, 15, 3, 15, 3, 9])
            if (symbol === "deafen") { line([17, 9, 23, 15]); line([23, 9, 17, 15]) }
            else { c.beginPath(); c.arc(13, 12, 6, -.8, .8); c.stroke(); c.beginPath(); c.arc(13, 12, 10, -.8, .8); c.stroke() }
        } else if (symbol === "home") {
            line([3, 10, 12, 3, 21, 10]); line([5, 9, 5, 21, 10, 21, 10, 14, 14, 14, 14, 21, 19, 21, 19, 9])
        } else if (symbol === "headphones") {
            c.beginPath(); c.arc(12, 11, 8, Math.PI, Math.PI * 2); c.stroke()
            c.beginPath(); c.roundedRect(4, 11, 4, 9, 2, 2); c.stroke()
            c.beginPath(); c.roundedRect(16, 11, 4, 9, 2, 2); c.stroke()
        } else if (symbol === "music") {
            line([9, 17, 9, 5, 20, 3, 20, 15])
            c.beginPath(); c.ellipse(3, 15, 6, 5); c.fill()
            c.beginPath(); c.ellipse(14, 13, 6, 5); c.fill()
        } else if (symbol === "play") {
            line([7, 4, 20, 12, 7, 20, 7, 4])
        } else if (symbol === "stop") {
            c.fillRect(6, 6, 12, 12)
        } else if (symbol === "record") {
            c.beginPath(); c.arc(12, 12, 9, 0, Math.PI * 2); c.stroke()
            c.beginPath(); c.arc(12, 12, 4, 0, Math.PI * 2); c.fill()
        } else if (symbol === "settings") {
            c.beginPath(); c.arc(12, 12, 6, 0, Math.PI * 2); c.stroke()
            c.beginPath(); c.arc(12, 12, 2, 0, Math.PI * 2); c.stroke()
            for (let i = 0; i < 8; ++i) { const a = i * Math.PI / 4; line([12 + 7 * Math.cos(a), 12 + 7 * Math.sin(a), 12 + 9 * Math.cos(a), 12 + 9 * Math.sin(a)]) }
        } else if (symbol === "person") {
            c.beginPath(); c.arc(12, 7, 4, 0, Math.PI * 2); c.stroke()
            c.beginPath(); c.arc(12, 21, 8, Math.PI, Math.PI * 2); c.stroke()
        } else if (symbol === "signal") {
            line([5, 18, 5, 21]); line([10, 13, 10, 21]); line([15, 8, 15, 21]); line([20, 3, 20, 21])
        } else if (symbol === "remote") {
            line([3, 4, 21, 4, 21, 16, 3, 16, 3, 4]); line([12, 16, 12, 21]); line([8, 21, 16, 21])
            line([8, 10, 16, 10, 13, 7]); line([16, 10, 13, 13])
        } else if (symbol === "image") {
            line([3, 3, 21, 3, 21, 21, 3, 21, 3, 3]); line([3, 17, 9, 11, 14, 16, 17, 13, 21, 17])
            c.beginPath(); c.arc(16, 8, 2, 0, 7); c.stroke()
        } else if (symbol === "send") {
            line([4, 4, 21, 12, 4, 20, 7, 12, 4, 4]); line([7, 12, 15, 12])
        } else if (symbol === "more") {
            for (let i = 5; i <= 19; i += 7) { c.beginPath(); c.arc(i, 12, 1.2, 0, 7); c.fill() }
        } else if (symbol === "down") {
            line([7, 10, 12, 15, 17, 10])
        } else if (symbol === "sleep") {
            line([7, 5, 18, 5, 7, 18, 18, 18])
        } else if (symbol === "check") {
            line([5, 12, 10, 17, 20, 6])
        } else if (symbol === "chat") {
            line([4, 4, 20, 4, 20, 16, 10, 16, 4, 21, 4, 4])
            line([8, 8, 16, 8]); line([8, 12, 13, 12])
        } else if (symbol === "channel") {
            line([8, 3, 5, 21]); line([18, 3, 15, 21])
            line([3, 9, 21, 9]); line([2, 16, 20, 16])
        } else if (symbol === "shield") {
            line([12, 3, 20, 6, 19, 15, 12, 22, 5, 15, 4, 6, 12, 3])
            line([8, 12, 11, 15, 16, 9])
        } else if (symbol === "lock") {
            c.beginPath(); c.roundedRect(5, 10, 14, 11, 2, 2); c.stroke()
            c.beginPath(); c.arc(12, 10, 4, Math.PI, Math.PI * 2); c.stroke()
            line([12, 14, 12, 17])
        } else if (symbol === "leave") {
            line([10, 4, 4, 4, 4, 20, 10, 20]); line([9, 12, 21, 12])
            line([16, 7, 21, 12, 16, 17])
        } else if (symbol === "link") {
            line([8, 14, 5, 17, 8, 20, 13, 15]); line([11, 9, 16, 4, 20, 8, 16, 12])
            line([9, 15, 15, 9])
        } else if (symbol === "close") {
            line([6, 6, 18, 18]); line([18, 6, 6, 18])
        } else if (symbol === "plus") {
            line([12, 4, 12, 20]); line([4, 12, 20, 12])
        } else {
            line([4, 8, 20, 8]); line([4, 16, 20, 16])
            c.fillRect(8, 5, 3, 6); c.fillRect(15, 13, 3, 6)
        }
    }
}
