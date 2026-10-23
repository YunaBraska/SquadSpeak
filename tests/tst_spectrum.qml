import QtQuick
import QtTest
import "../ui"

Rectangle {
    id: fixture
    width: 700
    height: 250
    color: "#faf8f5"
    property var signal: referenceTone
    Component {
        id: spectrumComponent
        SpectrumView {
            x: 24
            y: 16
            width: parent.width - 48
            audio: fixture.signal
        }
    }
    TestCase {
        name: "FrequencyDisplay"
        when: windowShown
        property var spectrum
        function init() {
            fixture.width = 700
            fixture.signal = referenceTone
            // Release the canvas while its render window still exists.
            spectrum = createTemporaryObject(spectrumComponent, fixture)
            verify(spectrum !== null)
        }
        function test_detectedFundamentalIsSeparateFromSpectralPeak() {
            compare(findChild(spectrum, "frequencyPeak").text, "Peak: 440 Hz")
            compare(findChild(spectrum, "fundamentalEstimate").text, "Pitch estimate: 220 Hz")
            verify(waitForRendering(spectrum))
            const chart = findChild(spectrum, "frequencyChart")
            tryVerify(function() { return fixtures.renderedColor(chart, 70, 20).toString() !== fixture.color.toString() })
            if (imageDirectory.length > 0)
                verify(fixtures.saveWindow(fixture.Window.window, imageDirectory + "/spectrum-active.png"))
        }
        function test_liveLevelsUseMeasuredPowerAndClearWhenCaptureStops() {
            fixture.signal = Object.assign({}, referenceTone, {inputDb: -41.2, outputDb: -47.3})
            const input = findChild(spectrum, "inputLevel"), output = findChild(spectrum, "processedLevel")
            verify(input !== null && output !== null)
            compare(input.text, "Input -41.2 dBFS")
            compare(output.text, "Processed -47.3 dBFS")
            fixture.signal = Object.assign({}, fixture.signal, {inputDb: -22.4, outputDb: -20.1})
            compare(input.text, "Input -22.4 dBFS")
            compare(output.text, "Processed -20.1 dBFS")
            fixture.signal = Object.assign({}, fixture.signal, {running: false})
            compare(input.text, "Input")
            compare(output.text, "Processed")
            fixture.signal = Object.assign({}, fixture.signal, {running: true, spectrumReady: false})
            compare(input.text, "Input")
            compare(output.text, "Processed")
        }
        function test_differenceShadingDoesNotAlterTheSignalScale() {
            fixture.signal = Object.assign({}, referenceTone, {
                inputSpectrum: [-20, -20], outputSpectrum: [-20, -20], filterResponse: []
            })
            const chart = findChild(spectrum, "frequencyChart")
            mouseMove(fixture, 0, 0)
            tryCompare(findChild(chart, "frequencyTooltip"), "visible", false)
            verify(waitForRendering(spectrum))
            const x = 153, y = 20 + (chart.height - 48) * 40 / 90
            const clear = fixtures.renderedColor(chart, x, y).toString()
            fixture.signal = Object.assign({}, fixture.signal, {outputSpectrum: [-50, -50]})
            tryVerify(function() { return fixtures.renderedColor(chart, x, y).toString() !== clear })
            fixture.signal = Object.assign({}, fixture.signal, {outputSpectrum: [-20, -20]})
            tryVerify(function() { return fixtures.renderedColor(chart, x, y).toString() === clear })
        }
        function test_inactiveCaptureNeverDisplaysOldDetection() {
            fixture.signal = Object.assign({}, referenceTone, {running: false})
            compare(findChild(spectrum, "frequencyPeak").text, "Peak: No signal")
            compare(findChild(spectrum, "fundamentalEstimate").text, "Pitch estimate: No stable value")
            compare(findChild(spectrum, "frequencyChart").active, false)
            compare(findChild(spectrum, "frequencyChart").response.length, 256)
        }
        function test_closedActivationShadesOnlyTheWithheldSpectrum() {
            spectrum.activationEnabled = true
            fixture.signal = Object.assign({}, referenceTone, {
                inputDb: -20, outputDb: -40, inputSpectrum: [-20, -20], outputSpectrum: [-40, -40],
                filterResponse: [], activationOpen: true
            })
            const chart = findChild(spectrum, "frequencyChart")
            verify(waitForRendering(spectrum))
            mouseMove(spectrum, 0, 0)
            tryCompare(findChild(chart, "frequencyTooltip"), "visible", false)
            const x = 153, y = 20 + (chart.height - 48) * 70 / 90
            const open = fixtures.renderedColor(chart, x, y).toString()
            fixture.signal = Object.assign({}, fixture.signal, {activationOpen: false})
            compare(chart.gated, true)
            verify(waitForRendering(spectrum))
            tryVerify(function() { return fixtures.renderedColor(chart, x, y).toString() !== open })
            if (imageDirectory.length > 0)
                verify(fixtures.saveWindow(fixture.Window.window, imageDirectory + "/spectrum-gated.png"))
            const tip = findChild(chart, "frequencyTooltip")
            mouseMove(chart, x, y)
            tryCompare(tip, "visible", true)
            verify(tip.text.indexOf("Would stay silent") >= 0)
            spectrum.activationEnabled = false
            mouseMove(spectrum, 0, 0)
            tryCompare(tip, "visible", false)
            verify(waitForRendering(spectrum))
            tryCompare(chart, "gated", false)
            tryVerify(function() { return fixtures.renderedColor(chart, x, y).toString() === open })
            spectrum.activationEnabled = true
            fixture.signal = Object.assign({}, fixture.signal, {running: false})
            compare(chart.gated, false)
        }
        function test_noStablePeriodHasNoVoiceClaim() {
            fixture.signal = Object.assign({}, referenceTone, {fundamentalHz: 0})
            compare(findChild(spectrum, "fundamentalEstimate").text, "Pitch estimate: No stable value")
        }
        function test_narrowWindowRetainsReadableAnalysis() {
            fixture.width = 380
            verify(waitForRendering(spectrum))
            const chart = findChild(spectrum, "frequencyChart")
            tryVerify(function() { return fixtures.renderedColor(chart, 70, 20).toString() !== fixture.color.toString() })
            if (imageDirectory.length > 0)
                verify(fixtures.saveWindow(fixture.Window.window, imageDirectory + "/spectrum-narrow.png"))
            verify(spectrum.height <= fixture.height)
        }
        function test_combinedPlotKeepsFilterUnitsInHover() {
            const chart = findChild(spectrum, "frequencyChart")
            verify(findChild(spectrum, "filterChart") === null, "All curves belong in one plot")
            verify(waitForRendering(spectrum))
            const tip = findChild(chart, "frequencyTooltip")
            fixture.signal = Object.assign({}, referenceTone, {running: false, filterResponse: [0, 0]})
            mouseMove(chart, chart.width / 2, 20 + (chart.height - 48) / 3)
            tryCompare(tip, "visible", true)
            verify(tip.text.indexOf("Filter curve") >= 0)
            verify(tip.text.indexOf("0.0 dB") >= 0)
            verify(tip.text.indexOf("dBFS") < 0)
        }
        function test_processedCurveDescribesTheCompleteFilterPath() {
            const chart = findChild(spectrum, "frequencyChart")
            const tip = findChild(chart, "frequencyTooltip")
            fixture.signal = Object.assign({}, referenceTone, {
                inputSpectrum: [-20, -20], outputSpectrum: [-60, -60], filterResponse: [0, 0]
            })
            verify(waitForRendering(spectrum))
            mouseMove(chart, 200, 20 + (chart.height - 48) * 2 / 3)
            tryCompare(tip, "visible", true)
            verify(tip.text.indexOf("-60.0 dBFS") >= 0)
            verify(tip.text.indexOf("After all filters, before transmission.") >= 0)
        }
        function test_activationThresholdIsNotDrawnAsAFrequencyCut() {
            fixture.signal = Object.assign({}, referenceTone, {running: false, filterResponse: [], activationThresholdDb: -45})
            const chart = findChild(spectrum, "frequencyChart"), tip = findChild(spectrum, "frequencyTooltip")
            verify(waitForRendering(spectrum))
            mouseMove(chart, 200, 20 + (chart.height - 48) / 2)
            compare(tip.visible, false)
        }
        function test_overlappingCurvesShowBothUnits() {
            const chart = findChild(spectrum, "frequencyChart")
            const tip = findChild(chart, "frequencyTooltip")
            fixture.signal = Object.assign({}, referenceTone, {
                inputSpectrum: [-30, -30], outputSpectrum: [-60, -60], filterResponse: [0, 0]
            })
            verify(waitForRendering(spectrum))
            mouseMove(chart, chart.width / 2, 20 + (chart.height - 48) / 3)
            tryCompare(tip, "visible", true)
            verify(tip.text.indexOf("-30.0 dBFS") >= 0)
            verify(tip.text.indexOf("0.0 dB") >= 0)
            verify(tip.text.indexOf("Filter curve") >= 0)
        }
        function test_hoverExplanationStaysWhileTheSignalMoves() {
            const chart = findChild(spectrum, "frequencyChart")
            const tip = findChild(spectrum, "frequencyTooltip")
            verify(tip !== null)
            fixture.signal = Object.assign({}, referenceTone, {
                inputSpectrum: [-30, -30], outputSpectrum: [-60, -60], filterResponse: [12, 12]
            })
            mouseMove(chart, 200, 20 + (chart.height - 48) / 3)
            tryCompare(tip, "visible", true)
            const explanation = tip.text
            verify(explanation.indexOf("Input") >= 0)
            verify(explanation.indexOf("-30.0 dBFS") >= 0)
            fixture.signal = Object.assign({}, fixture.signal, {inputSpectrum: [-90, -90]})
            wait(600)
            compare(tip.visible, true)
            compare(tip.text, explanation)
            mouseMove(chart, 200, 20 + (chart.height - 48) * 2 / 3)
            tryVerify(function() { return tip.text.indexOf("After all filters") >= 0 })
            mouseMove(fixture, 1, 1)
            tryCompare(tip, "visible", false)
        }
    }
}
