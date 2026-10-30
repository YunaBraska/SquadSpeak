import QtQuick
import QtQuick.Controls.Basic

TextField {
    id: root
    implicitHeight: Math.max(38, contentHeight + topPadding + bottomPadding)
    leftPadding: 12
    rightPadding: 12
    color: Theme.text
    placeholderTextColor: Theme.muted
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    selectByMouse: true
    background: Rectangle {
        color: Theme.input
        radius: Theme.controlRadius
        border.color: root.activeFocus ? Theme.accent : Theme.border
    }
}
