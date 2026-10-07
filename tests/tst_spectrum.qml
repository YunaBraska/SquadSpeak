import QtQuick
import QtTest
import "../ui"

Rectangle {
    id: fixture
    width: 700
    height: 440
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
            if (imageDirectory.length > 0)
                grabImage(fixture).save(imageDirectory + "/spectrum-active.png")
        }
        function test_inactiveCaptureNeverDisplaysOldDetection() {
            fixture.signal = Object.assign({}, referenceTone, {running: false})
            compare(findChild(spectrum, "frequencyPeak").text, "Peak: No signal")
            compare(findChild(spectrum, "fundamentalEstimate").text, "Pitch estimate: No stable value")
            compare(findChild(spectrum, "frequencyChart").active, false)
            compare(findChild(spectrum, "frequencyChart").response.length, 256)
        }
        function test_noStablePeriodHasNoVoiceClaim() {
            fixture.signal = Object.assign({}, referenceTone, {fundamentalHz: 0})
            compare(findChild(spectrum, "fundamentalEstimate").text, "Pitch estimate: No stable value")
        }
        function test_narrowWindowRetainsReadableAnalysis() {
            fixture.width = 380
            verify(waitForRendering(spectrum))
            if (imageDirectory.length > 0)
                grabImage(fixture).save(imageDirectory + "/spectrum-narrow.png")
            verify(spectrum.height <= fixture.height)
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
            fixture.signal = Object.assign({}, fixture.signal, {inputSpectrum: [-90, -90]})
            wait(600)
            compare(tip.visible, true)
            compare(tip.text, explanation)
            mouseMove(chart, 200, 20 + (chart.height - 48) * 2 / 3)
            tryVerify(function() { return tip.text.indexOf("After gain") >= 0 })
            mouseMove(fixture, 1, 1)
            tryCompare(tip, "visible", false)
        }
    }
}
