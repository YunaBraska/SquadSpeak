import QtQuick
import QtQuick.Controls.Basic

Button {
    id: root
    property bool iconOnly: false
    property bool primary: false
    property bool destructive: false
    property string glyph: ""
    property int horizontalAlignment: Text.AlignHCenter
    implicitHeight: 36
    implicitWidth: iconOnly ? 36 : Math.max(36, label.implicitWidth + (glyph.length > 0 ? 24 : 0) + 24)
    padding: 10
    spacing: 8
    hoverEnabled: true
    Accessible.name: text
    TextToolTip {
        parent: root; text: root.text; delay: 450
        visible: root.iconOnly && (root.hovered || root.activeFocus)
    }
    opacity: enabled ? 1 : 0.45
    contentItem: Item {
        implicitWidth: root.iconOnly ? 18 : label.implicitWidth + (root.glyph.length > 0 ? 24 : 0)
        implicitHeight: 20
        Glyph {
            visible: root.glyph.length > 0
            symbol: root.glyph
            x: root.mirrored && !root.iconOnly ? parent.width - width : 0
            anchors.horizontalCenter: root.iconOnly ? parent.horizontalCenter : undefined
            anchors.verticalCenter: parent.verticalCenter
            color: root.primary ? Theme.accentText : root.destructive ? Theme.danger : Theme.text
        }
        Text {
            id: label
            visible: !root.iconOnly
            anchors.fill: parent
            anchors.leftMargin: root.glyph.length > 0 ? 24 : 0
            text: root.text
            textFormat: Text.PlainText
            font: root.font
            color: root.primary ? Theme.accentText : root.destructive ? Theme.danger : Theme.text
            verticalAlignment: Text.AlignVCenter
            horizontalAlignment: root.horizontalAlignment
            elide: Text.ElideRight
        }
    }
    background: Rectangle {
        radius: Theme.controlRadius
        color: root.primary ? (root.down ? Qt.darker(Theme.accent, 1.12) : Theme.accent)
            : root.down ? Theme.input : root.hovered || root.checked ? Theme.raised : root.flat ? "transparent" : Theme.surface
        border.width: root.flat && !root.activeFocus ? 0 : 1
        border.color: root.activeFocus ? Theme.accent : root.destructive ? Theme.danger : Theme.border
    }
}
