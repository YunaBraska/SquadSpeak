import QtQuick
import QtQuick.Controls.Basic

Switch {
    id: control
    spacing: 8
    padding: 4
    implicitWidth: Math.max(indicator.implicitWidth, contentItem.implicitWidth) + leftPadding + rightPadding
    implicitHeight: Math.max(indicator.implicitHeight, contentItem.implicitHeight) + topPadding + bottomPadding
    indicator: Rectangle {
        implicitWidth: 38; implicitHeight: 22
        x: control.mirrored ? control.width - width - control.rightPadding : control.leftPadding
        y: (control.height - height) / 2
        radius: height / 2
        color: control.checked ? Theme.accent : Theme.border
        opacity: control.enabled ? 1 : 0.45
        border.width: control.activeFocus ? 2 : 0
        border.color: Theme.text
        Rectangle {
            x: 3 + control.visualPosition * (parent.width - width - 6)
            y: 3; width: 16; height: 16; radius: 8
            color: control.checked ? Theme.background : Theme.text
        }
    }
    contentItem: Text {
        text: control.text; font: control.font; color: Theme.text
        leftPadding: control.mirrored || !control.text.length ? 0 : control.indicator.width + control.spacing
        rightPadding: !control.mirrored || !control.text.length ? 0 : control.indicator.width + control.spacing
        verticalAlignment: Text.AlignVCenter
        opacity: control.enabled ? 1 : 0.45
    }
}
