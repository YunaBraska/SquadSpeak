import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: root
    LayoutMirroring.enabled: false
    LayoutMirroring.childrenInherit: true
    required property var audio
    property real chartHeight: 166
    property bool showReadouts: true
    property bool activationEnabled: false
    spacing: 4

    Flow {
        Layout.fillWidth: true
        spacing: 12
        Label {
            objectName: "inputLevel"
            text: qsTr("Input") + (chart.active ? " " + audio.inputDb.toFixed(1) + " " + qsTr("dBFS") : "")
            color: Theme.muted; font.pixelSize: 12
        }
        Label {
            objectName: "processedLevel"
            text: qsTr("Processed") + (chart.active ? " " + audio.outputDb.toFixed(1) + " " + qsTr("dBFS") : "")
            color: chart.gated ? Theme.danger : Theme.accent; font.pixelSize: 12
        }
        Label { text: qsTr("Filter curve"); color: Theme.warning; font.pixelSize: 12 }
    }
    Canvas {
        id: chart
        objectName: "frequencyChart"
        Layout.fillWidth: true
        implicitHeight: root.chartHeight
        property var raw: audio.inputSpectrum
        property var processed: audio.outputSpectrum
        property var response: audio.filterResponse || []
        property real maximum: audio.maximumFrequency
        property bool active: audio.running && audio.spectrumReady
        readonly property bool gated: active && root.activationEnabled && audio.activationOpen === false
        onRawChanged: requestPaint()
        onResponseChanged: requestPaint()
        onProcessedChanged: requestPaint()
        onMaximumChanged: requestPaint()
        onActiveChanged: requestPaint()
        onGatedChanged: requestPaint()
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        Connections { target: Theme; function onTonesChanged() { chart.requestPaint() } }
        Accessible.name: qsTr("Frequency spectrum in hertz and dBFS")
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const left = 42
            const top = 20
            const plotWidth = width - left - 36
            const plotHeight = height - top - 28
            const yFor = function(value, minimum, maximum) {
                return top + plotHeight * (1 - Math.max(0, Math.min(1, (value - minimum) / (maximum - minimum))))
            }
            const xFor = function(hz) {
                return left + plotWidth * Math.log(hz / 20) / Math.log(maximum / 20)
            }
            ctx.font = "11px sans-serif"
            ctx.fillStyle = Theme.muted
            ctx.fillText(qsTr("dBFS"), 0, 12)
            for (const db of [0, -30, -60, -90]) {
                const y = top - db / 90 * plotHeight
                ctx.strokeStyle = Theme.border
                ctx.beginPath(); ctx.moveTo(left, y); ctx.lineTo(left + plotWidth, y); ctx.stroke()
                ctx.fillText(String(db), 6, y + 4)
            }
            ctx.textAlign = "left"
            const ticks = width < 450 ? [20, 100, 1000, 10000] : [20, 50, 100, 200, 500, 1000, 2000, 5000, 10000]
            for (const hz of ticks) {
                if (hz >= maximum * 0.85) continue
                const x = xFor(hz)
                if (x > left + plotWidth - 56) continue
                ctx.strokeStyle = Theme.raised
                ctx.beginPath(); ctx.moveTo(x, top); ctx.lineTo(x, top + plotHeight); ctx.stroke()
                ctx.fillText(hz >= 1000 ? qsTr("%1k").arg(String(hz / 1000)) : String(hz), x - 7, height - 7)
            }
            ctx.textAlign = "right"
            ctx.fillText(qsTr("%1 Hz").arg(maximum >= 1000 ? qsTr("%1k").arg((maximum / 1000).toFixed(1)) : String(Math.round(maximum))), width - 2, height - 7)
            ctx.textAlign = "left"
            const draw = function(values, color, minimum, maximum) {
                ctx.strokeStyle = color
                ctx.lineWidth = 1.5
                ctx.beginPath()
                for (let i = 0; i < values.length; ++i) {
                    const x = left + i * plotWidth / (values.length - 1)
                    const y = yFor(values[i], minimum, maximum)
                    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
            if (active) {
                if (gated && processed.length > 1) {
                    ctx.fillStyle = Theme.danger
                    ctx.globalAlpha = 0.12
                    ctx.beginPath()
                    ctx.moveTo(left, top + plotHeight)
                    for (let i = 0; i < processed.length; ++i)
                        ctx.lineTo(left + i * plotWidth / (processed.length - 1), yFor(processed[i], -90, 0))
                    ctx.lineTo(left + plotWidth, top + plotHeight)
                    ctx.closePath(); ctx.fill()
                    ctx.globalAlpha = 1
                }
                if (raw.length === processed.length && raw.length > 1) {
                    ctx.fillStyle = Theme.accent
                    ctx.globalAlpha = 0.12
                    ctx.beginPath()
                    for (let i = 0; i < raw.length; ++i) {
                        const x = left + i * plotWidth / (raw.length - 1), y = yFor(raw[i], -90, 0)
                        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                    }
                    for (let i = processed.length - 1; i >= 0; --i)
                        ctx.lineTo(left + i * plotWidth / (processed.length - 1), yFor(processed[i], -90, 0))
                    ctx.closePath(); ctx.fill()
                    ctx.globalAlpha = 1
                }
                draw(raw, Theme.muted, -90, 0)
                draw(processed, gated ? Theme.danger : Theme.accent, -90, 0)
            }
            ctx.fillStyle = Theme.warning
            ctx.textAlign = "right"
            ctx.fillText(qsTr("dB"), width, 12)
            for (const db of [24, 0, -24, -48]) {
                const y = top + (24 - db) / 72 * plotHeight
                ctx.fillText(db > 0 ? "+" + db : String(db), width, y + 4)
            }
            ctx.setLineDash([4, 4])
            draw(response, Theme.warning, -48, 24)
        }
        Label {
            anchors.centerIn: parent
            width: parent.width - 100
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            visible: !chart.active
            text: audio.running ? qsTr("Collecting signal...") : qsTr("Microphone preview is not running")
            color: Theme.muted
        }
        MouseArea {
            id: probe
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            property real anchorX: -1000
            property real anchorY: -1000
            property bool showing: false
            onExited: showing = false
            onPositionChanged: function(mouse) {
                if (showing && Math.hypot(mouse.x - anchorX, mouse.y - anchorY) < 8) return
                anchorX = mouse.x; anchorY = mouse.y
                const w = chart.width - 78, h = chart.height - 48
                const t = (mouse.x - 42) / w
                if (t < 0 || t > 1 || mouse.y < 20 || mouse.y > 20 + h) { showing = false; return }
                const candidates = [
                    {values: chart.response, min: -48, max: 24, unit: qsTr("dB"),
                        text: qsTr("Filter curve: the configured change in level at each frequency, read on the right dB scale. It excludes noise reduction and voice activation.")},
                    {values: chart.active ? chart.raw : [], min: -90, max: 0, unit: qsTr("dBFS"),
                        text: qsTr("Input: sound picked up by the microphone before gain and frequency cuts. Left is bass, right is treble; higher is louder.")},
                    {values: chart.active ? chart.processed : [], min: -90, max: 0, unit: qsTr("dBFS"),
                        text: qsTr("After all filters, before transmission.")}
                ]
                const matches = []
                for (const line of candidates) {
                    if (line.values.length < 2) continue
                    const at = t * (line.values.length - 1), lo = Math.floor(at), hi = Math.min(lo + 1, line.values.length - 1)
                    const value = line.values[lo] + (line.values[hi] - line.values[lo]) * (at - lo)
                    const y = 20 + h * (1 - Math.max(0, Math.min(1, (value - line.min) / (line.max - line.min))))
                    const distance = Math.abs(mouse.y - y)
                    if (distance < 13) matches.push({distance: distance, text: value.toFixed(1) + " " + line.unit + "\n" + line.text})
                }
                matches.sort(function(a, b) { return a.distance - b.distance })
                frequencyTip.text = qsTr("%1 Hz").arg(Math.round(20 * Math.pow(chart.maximum / 20, t)))
                    + "\n" + matches.map(function(line) { return line.text }).join("\n\n")
                showing = matches.length > 0
                if (chart.gated && chart.processed.length > 1) {
                    const at = t * (chart.processed.length - 1), lo = Math.floor(at), hi = Math.min(lo + 1, chart.processed.length - 1)
                    const value = chart.processed[lo] + (chart.processed[hi] - chart.processed[lo]) * (at - lo)
                    if (mouse.y >= 20 + h * (1 - Math.max(0, Math.min(1, (value + 90) / 90)))) {
                        frequencyTip.text += "\n" + qsTranslate("AudioSettings", "Would stay silent")
                        showing = true
                    }
                }
            }
            ToolTip {
                id: frequencyTip
                objectName: "frequencyTooltip"
                visible: probe.showing
                timeout: -1
                width: Math.min(chart.width - 16, 320)
                x: Math.max(8, Math.min(probe.anchorX - width / 2, chart.width - width - 8))
                y: probe.anchorY > chart.height / 2 ? probe.anchorY - height - 12 : probe.anchorY + 12
                contentItem: Label { text: frequencyTip.text; wrapMode: Text.Wrap; color: Theme.text }
                background: Rectangle { radius: Theme.controlRadius; color: Theme.surface; border.color: Theme.border }
            }
        }
    }
    Flow {
        visible: root.showReadouts
        Layout.fillWidth: true
        spacing: 14
        Label {
            objectName: "frequencyPeak"
            text: qsTr("Peak: %1").arg(!audio.running || audio.strongestHz <= 0 ? qsTr("No signal") : qsTr("%1 Hz").arg(Math.round(audio.strongestHz)))
            color: Theme.muted; font.pixelSize: 12
        }
        Label {
            objectName: "fundamentalEstimate"
            text: qsTr("Pitch estimate: %1").arg(!audio.running || audio.fundamentalHz <= 0 ? qsTr("No stable value") : qsTr("%1 Hz").arg(Math.round(audio.fundamentalHz)))
            color: Theme.muted; font.pixelSize: 12
            ToolTip.visible: pitchHover.hovered
            ToolTip.text: qsTr("A periodic pitch can also come from music or hum. It does not identify a person.")
            HoverHandler { id: pitchHover }
        }
    }
}
