import QtQuick
import QtQuick.Controls.Basic

ComboBox {
    id: root
    implicitHeight: 38
    implicitWidth: 180
    leftPadding: 12
    rightPadding: 34
    hoverEnabled: true
    opacity: enabled ? 1 : 0.45
    contentItem: Text {
        text: root.displayText
        textFormat: Text.PlainText
        font: root.font
        color: Theme.text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
    indicator: Glyph {
        x: root.width - width - 10
        y: (root.height - height) / 2
        symbol: "down"
        color: Theme.muted
    }
    background: Rectangle {
        radius: Theme.controlRadius
        color: root.down ? Theme.raised : Theme.input
        border.color: root.activeFocus ? Theme.accent : Theme.border
    }
    delegate: ItemDelegate {
        required property int index
        width: root.width - 2
        text: root.textAt(index)
        highlighted: root.highlightedIndex === index
        contentItem: Text { text: parent.text; textFormat: Text.PlainText; font: root.font; color: Theme.text; elide: Text.ElideRight; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: Theme.smallRadius; color: parent.highlighted ? Theme.raised : "transparent" }
    }
    popup: Popup {
        popupType: Popup.Item
        y: root.height + 4
        width: root.width
        implicitHeight: Math.min(contentItem.implicitHeight + 8, 280)
        padding: 4
        background: Rectangle { radius: Theme.controlRadius; color: Theme.surface; border.color: Theme.border }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: root.popup.visible ? root.delegateModel : null
            currentIndex: root.highlightedIndex
            ScrollIndicator.vertical: ScrollIndicator { }
        }
    }
}
