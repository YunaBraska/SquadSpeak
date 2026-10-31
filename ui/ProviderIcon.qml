import QtQuick
import QtQuick.Controls.Basic

Item {
    id: root
    property string provider: "squadspeak"
    readonly property string label: provider === "mumble" ? "Mumble" : provider === "teamspeak" ? "TeamSpeak" : provider === "squadspeak" ? "SquadSpeak" : ""
    Image {
        objectName: "providerArtwork"
        anchors.fill: parent
        source: root.label.length === 0 ? "" : root.provider === "mumble" ? "icons/mumble.png" : root.provider === "teamspeak" ? "icons/teamspeak.png" : "icons/app.png"
        sourceSize.width: Math.ceil(width * Screen.devicePixelRatio)
        sourceSize.height: Math.ceil(height * Screen.devicePixelRatio)
        fillMode: Image.PreserveAspectFit
    }
    implicitWidth: 18; implicitHeight: 18
    Accessible.role: Accessible.Graphic
    Accessible.name: label
    ToolTip.text: label
    ToolTip.visible: label.length > 0 && hover.hovered
    ToolTip.delay: 450
    HoverHandler { id: hover }
}
