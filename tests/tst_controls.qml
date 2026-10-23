import QtQuick
import QtTest
import "../ui"

Item {
    id: fixture
    width: 400
    height: 200
    property real storedValue: 0
    property bool acceptEdits: true
    property int writes: 0
    SignalSpy { id: editFinished; signalName: "editingFinished" }
    Component {
        id: actionComponent
        ActionButton {
            property bool rtl: false
            LayoutMirroring.enabled: rtl
            LayoutMirroring.childrenInherit: true
            width: 140; text: "Balanced"; glyph: "signal"
        }
    }
    Component { id: choiceComponent; ChoiceBox { width: 300 } }
    Component { id: literalText; Text { textFormat: Text.PlainText } }
    Component {
        id: editorComponent
        SettingSlider {
            width: 300
            from: -24
            to: 24
            value: storedValue
            caption: "Verstärkung"
            valueText: storedValue.toString()
            applyValue: function(next) {
                if (!acceptEdits) return false
                ++writes
                storedValue = next
                return true
            }
        }
    }
    TestCase {
        name: "AudioSettingsControls"
        when: windowShown
        property var editor
        function init() {
            acceptEdits = true
            storedValue = 0
            writes = 0
            editor = createTemporaryObject(editorComponent, fixture)
            verify(editor !== null)
            editFinished.target = editor
            editFinished.clear()
        }
        function test_sliderFeedbackFollowsCompletedUserEdits() {
            verify(editFinished.valid)
            const control = findChild(editor, "valueControl")
            storedValue = -4
            compare(editFinished.count, 0, "Loading a profile is silent")
            mousePress(control, control.width / 2, control.height / 2)
            mouseMove(control, control.width * 0.7, control.height / 2)
            mouseMove(control, control.width * 0.8, control.height / 2)
            compare(editFinished.count, 0, "Dragging must not queue repeated tones")
            mouseRelease(control, control.width * 0.8, control.height / 2)
            compare(editFinished.count, 1)
            control.forceActiveFocus()
            keyClick(Qt.Key_Left)
            compare(editFinished.count, 2, "Keyboard changes also get feedback")
            control.value = 3
            compare(editFinished.count, 3, "Accessible value edits also get feedback")
            acceptEdits = false
            keyClick(Qt.Key_Right)
            compare(editFinished.count, 3, "Rejected changes stay silent")
        }
        function test_liveAutomaticValuesUpdateWithoutSavingAnOverride() {
            editor.autoAvailable = true
            editor.automatic = true
            editor.applyAutomatic = function(next) { editor.automatic = next }
            const control = findChild(editor, "valueControl")
            const valueLabel = findChild(editor, "valueLabel")
            verify(valueLabel !== null)
            for (const value of [-1.0137, -0.067, 0, -0.8421]) {
                storedValue = value
                compare(control.value, value)
                compare(valueLabel.text, value.toString())
                compare(editor.automatic, true)
                compare(writes, 0)
                compare(editFinished.count, 0)
            }
            control.forceActiveFocus()
            keyClick(Qt.Key_Right)
            compare(writes, 1)
            compare(editFinished.count, 1)
        }
        function test_eventCuesRespectOutputAndDeafen() {
            compare(typeof audio.notificationVolume, "number")
            for (const kind of ["join", "memberJoin", "leave", "memberLeave", "kick", "ban", "message", "announcement", "request"]) {
                compare(fixtures.playEvent(kind, false), audio.outputAvailable, kind)
                verify(!fixtures.playEvent(kind, true), kind + " is silent while deafened")
            }
            verify(!fixtures.playEvent("future-event", false))
            verify(!fixtures.playEvent("", false))
            if (audio.outputAvailable) {
                const notificationVolume = audio.notificationVolume
                verify(audio.setNotificationVolume(0))
                verify(!fixtures.playEvent("message", false), "Zero notification volume suppresses every app cue")
                verify(audio.setNotificationVolume(notificationVolume))
                const volume = audio.outputVolume
                verify(audio.setOutputVolume(0))
                const independent = fixtures.playEvent("announcement", false)
                verify(audio.setOutputVolume(volume))
                verify(independent, "Zero voice volume leaves app cues audible")
            }
        }
        function test_outputFeedbackRespectsVolumeAndMute() {
            const original = audio.notificationVolume
            try {
                if (audio.outputAvailable) verify(audio.setNotificationVolume(0))
                for (const volume of [0.2, 0.6, 1, 0, -1, 2, NaN, Infinity]) {
                    for (const muted of [false, true]) {
                        const result = fixtures.previewOutput(volume, muted)
                        compare(result.accepted, audio.outputAvailable && !muted && volume > 0 && volume <= 1)
                        verify(!result.testing, "Slider feedback does not enter microphone test mode")
                        verify(!result.capturing, "Slider feedback must not start the microphone")
                    }
                }
                if (audio.outputAvailable) {
                    compare(audio.notificationVolume, 0, "A voice/music preview does not change app sound volume")
                    verify(!fixtures.playEvent("message", false), "Regular notifications remain silent")
                }
            } finally {
                if (audio.outputAvailable) audio.setNotificationVolume(original)
            }
        }
        function test_accessibleValueUpdatesSettings() {
            const control = findChild(editor, "valueControl")
            verify(control !== null)
            control.value = 3
            compare(storedValue, 3)
        }
        function test_loadingProfileDoesNotWriteItAgain() {
            storedValue = -4
            compare(findChild(editor, "valueControl").value, -4)
            compare(writes, 0)
        }
        function test_rejectedEditRestoresAuthoritativeValue() {
            storedValue = 2
            acceptEdits = false
            findChild(editor, "valueControl").value = 6
            compare(storedValue, 2)
            compare(findChild(editor, "valueControl").value, 2)
            storedValue = -3
            compare(findChild(editor, "valueControl").value, -3)
        }
        function test_keyboardEditUpdatesSettings() {
            const control = findChild(editor, "valueControl")
            control.forceActiveFocus()
            keyClick(Qt.Key_Right)
            compare(storedValue, 1)
        }
        function test_deviceNamesRenderLiterally_data() {
            return [{tag: "ordinary", name: "Studio & USB"},
                    {tag: "markup", name: "<b>Studio</b>"},
                    {tag: "font", name: "<font size='1'>Microphone</font>"}]
        }
        function test_deviceNamesRenderLiterally(data) {
            const choice = createTemporaryObject(choiceComponent, fixture, {model: [data.name]})
            verify(choice)
            const reference = createTemporaryObject(literalText, fixture, {text: data.name, font: choice.font})
            verify(reference)
            waitForRendering(choice)
            fuzzyCompare(choice.contentItem.implicitWidth, reference.implicitWidth, 0.1)
            choice.popup.open()
            tryCompare(choice.popup, "opened", true)
            const list = choice.popup.contentItem
            list.forceLayout()
            const option = list.itemAtIndex(0)
            verify(option)
            fuzzyCompare(option.contentItem.implicitWidth, reference.implicitWidth, 0.1)
            choice.popup.close()
        }
        function test_iconAndTextDoNotOverlapInEitherDirection_data() {
            return [{tag: "LTR", rtl: false}, {tag: "RTL", rtl: true}]
        }
        function test_iconAndTextDoNotOverlapInEitherDirection(data) {
            const button = createTemporaryObject(actionComponent, fixture, {rtl: data.rtl})
            verify(button)
            waitForRendering(button)
            const icon = button.contentItem.children.find(function(item) { return item.symbol === "signal" })
            const label = button.contentItem.children.find(function(item) { return item.text === "Balanced" })
            verify(icon && label)
            if (data.rtl) verify(label.x + label.width + 4 <= icon.x)
            else verify(icon.x + icon.width + 4 <= label.x)
        }
    }
}
