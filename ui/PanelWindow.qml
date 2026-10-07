import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    color: Theme.background
    palette.window: Theme.background
    palette.windowText: Theme.text
    palette.base: Theme.input
    palette.alternateBase: Theme.surface
    palette.button: Theme.raised
    palette.text: Theme.text
    palette.buttonText: Theme.text
    palette.placeholderText: Theme.muted
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentText
    palette.light: Theme.border
    palette.mid: Theme.border
    palette.dark: Theme.muted
    palette.link: Theme.accent
    font.pixelSize: 14
}
