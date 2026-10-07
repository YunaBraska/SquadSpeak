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
    property bool autoAvailable: false
    property bool automatic: false
    property var applyAutomatic: null
    required property var applyValue
    property bool ready: false
    property bool restoring: false
    Component.onCompleted: ready = true
    spacing: 0
    RowLayout {
        Layout.fillWidth: true
        Label { text: caption; textFormat: Text.PlainText; Layout.fillWidth: true; wrapMode: Text.WordWrap }
        CheckBox {
            id: automaticCheck
            objectName: "automaticControl"
            visible: setting.autoAvailable
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
        Label { text: valueText; textFormat: Text.PlainText; font.family: "monospace" }
    }
    Slider {
        id: control
        objectName: "valueControl"
        Layout.fillWidth: true
        from: parent.from
        to: parent.to
        value: parent.value
        stepSize: parent.stepSize
        Accessible.name: parent.caption
        implicitHeight: 28
        background: Rectangle {
            x: control.leftPadding
            y: control.topPadding + control.availableHeight / 2 - height / 2
            width: control.availableWidth
            height: 4
            radius: Theme.smallRadius
            color: Theme.border
            Rectangle { width: control.visualPosition * parent.width; height: parent.height; radius: Theme.smallRadius; color: Theme.accent }
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
            setting.applyValue(value)
            value = Qt.binding(function() { return setting.value })
            setting.restoring = false
        }
    }
}
