import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

ColumnLayout {
    id: setting
    property string caption
    property string valueText
    property real from: 0
    property real to: 100
    property real value: 0
    property real stepSize: 1
    property real levelValue: NaN
    property color levelColor: Theme.accent
    property bool toggleAvailable: false
    property bool filterEnabled: true
    property var applyEnabled: null
    property bool autoAvailable: false
    property bool automatic: false
    property var applyAutomatic: null
    required property var applyValue
    signal editingFinished()
    property bool ready: false
    property bool restoring: false
    Component.onCompleted: ready = true
    spacing: 0
    RowLayout {
        Layout.fillWidth: true
        SettingToggle {
            objectName: "filterEnabledControl"
            visible: setting.toggleAvailable
            checked: setting.filterEnabled
            Accessible.name: setting.caption
            onToggled: {
                setting.applyEnabled(checked)
                checked = Qt.binding(function() { return setting.filterEnabled })
            }
        }
        Label { objectName: "filterCaption"; text: caption; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        CheckBox {
            id: automaticCheck
            objectName: "automaticControl"
            visible: setting.autoAvailable
            enabled: setting.filterEnabled
            text: qsTr("Auto")
            padding: 0; spacing: 4; font.pixelSize: 12
            checked: setting.automatic
            indicator: Rectangle {
                implicitWidth: 14; implicitHeight: 14
                y: (automaticCheck.height - height) / 2
                color: "transparent"; radius: Theme.smallRadius
                border.color: automaticCheck.checked ? Theme.accent : Theme.muted
                Glyph { anchors.fill: parent; anchors.margins: 1; symbol: "check"; visible: automaticCheck.checked; color: Theme.accent }
            }
            Accessible.name: setting.caption + ", " + qsTr("Automatic")
            onCheckedChanged: {
                if (!setting.ready || checked === setting.automatic) return
                setting.applyAutomatic(checked)
                checked = Qt.binding(function() { return setting.automatic })
            }
        }
        Label { text: setting.filterEnabled ? valueText : qsTranslate("AudioSettings", "Off"); textFormat: Text.PlainText; font.family: "monospace" }
    }
    Slider {
        id: control
        enabled: setting.filterEnabled
        objectName: "valueControl"
        Layout.fillWidth: true
        from: parent.from
        to: parent.to
        value: parent.value
        stepSize: parent.stepSize
        Accessible.name: parent.caption
        implicitHeight: 28
        property bool editedWhilePressed: false
        onPressedChanged: {
            if (!pressed && editedWhilePressed) {
                editedWhilePressed = false
                setting.editingFinished()
            }
        }
        background: Rectangle {
            x: control.leftPadding
            y: control.topPadding + control.availableHeight / 2 - height / 2
            width: control.availableWidth
            height: 4
            radius: Theme.smallRadius
            color: Theme.border
            Rectangle {
                width: parent.width * (isFinite(setting.levelValue)
                    ? Math.max(0, Math.min(1, (setting.levelValue - setting.from) / (setting.to - setting.from)))
                    : control.visualPosition)
                height: parent.height; radius: Theme.smallRadius
                color: isFinite(setting.levelValue) ? setting.levelColor : Theme.accent
            }
        }
        handle: Rectangle {
            x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
            y: control.topPadding + control.availableHeight / 2 - height / 2
            width: 12; height: 18; radius: Theme.smallRadius
            color: control.pressed ? Theme.text : Theme.accent
            border.width: control.activeFocus ? 2 : 0
            border.color: Theme.text
        }
        onValueChanged: {
            if (!setting.ready || setting.restoring || value === setting.value)
                return
            setting.restoring = true
            const accepted = setting.applyValue(value)
            value = Qt.binding(function() { return setting.value })
            setting.restoring = false
            if (accepted) {
                if (pressed) editedWhilePressed = true
                else setting.editingFinished()
            }
        }
    }
}
