import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

MenuItem {
    id: action
    property bool destructive: false
    property string glyph: ""
    leftPadding: 12
    rightPadding: checkable ? 36 : 12
    height: visible ? implicitHeight : 0
    contentItem: RowLayout {
        spacing: 8
        opacity: action.enabled ? 1 : 0.45
        Glyph {
            visible: action.glyph.length > 0
            symbol: action.glyph
            color: action.destructive ? Theme.danger : Theme.text
        }
        Label {
            Layout.fillWidth: true
            text: action.text; textFormat: Text.PlainText
            font: action.font
            color: action.destructive ? Theme.danger : Theme.text
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
    background: Rectangle { implicitWidth: 240; implicitHeight: 36; radius: Theme.smallRadius; color: action.highlighted ? Theme.raised : "transparent" }
    indicator: Rectangle {
        x: action.width - width - 12
        y: (action.height - height) / 2
        implicitWidth: 16; implicitHeight: 16
        visible: action.checkable
        color: "transparent"
        border.color: action.checked ? Theme.accent : Theme.muted
        radius: Theme.smallRadius
        Glyph { anchors.centerIn: parent; width: 12; height: 12; symbol: "check"; color: Theme.accent; visible: action.checked }
    }
}
