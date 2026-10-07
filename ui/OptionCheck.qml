import QtQuick
import QtQuick.Controls.Basic

CheckBox {
    id: root
    implicitHeight: 32
    spacing: 8
    padding: 0
    opacity: enabled ? 1 : 0.45
    indicator: Rectangle {
        x: root.leftPadding
        y: (root.height - height) / 2
        implicitWidth: 18
        implicitHeight: 18
        radius: Theme.smallRadius
        color: root.down ? Theme.raised : Theme.input
        border.color: root.activeFocus || root.checked ? Theme.accent : Theme.border
        Glyph { anchors.centerIn: parent; width: 14; height: 14; symbol: "check"; color: Theme.accent; visible: root.checked }
    }
    contentItem: Text {
        text: root.text
        font: root.font
        color: Theme.text
        leftPadding: root.indicator.width + root.spacing
        verticalAlignment: Text.AlignVCenter
        wrapMode: Text.Wrap
    }
}
