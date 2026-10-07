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
        }
        function test_eventCuesRespectOutputAndDeafen() {
            for (const kind of ["join", "memberJoin", "leave", "memberLeave", "kick", "ban", "message", "announcement"]) {
                compare(fixtures.playEvent(kind, false), audio.outputAvailable, kind)
                verify(!fixtures.playEvent(kind, true), kind + " is silent while deafened")
            }
            verify(!fixtures.playEvent("future-event", false))
            verify(!fixtures.playEvent("", false))
            if (audio.outputAvailable) {
                const volume = audio.outputVolume
                verify(audio.setOutputVolume(0))
                const muted = !fixtures.playEvent("announcement", false)
                verify(audio.setOutputVolume(volume))
                verify(muted, "Zero app volume suppresses the cue")
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
