import QtQuick
import QtQuick.Controls.Basic

ToolTip {
    id: root
    contentItem: Text {
        text: root.text
        textFormat: Text.PlainText
        font: root.font
        wrapMode: Text.Wrap
        color: Theme.text
    }
    background: Rectangle {
        color: Theme.surface; border.color: Theme.border; radius: Theme.smallRadius
    }
}
