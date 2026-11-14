import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts

Rectangle {
    id: root
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    required property var game
    required property string identity
    property bool canJoin: false
    signal actionRequested(string action, var selection)
    signal openRequested()
    readonly property var players: game.players || []
    readonly property bool participant: players.some(p => p.id === identity)
    color: Theme.raised; radius: Theme.panelRadius
    implicitHeight: content.implicitHeight + 24
    ColumnLayout {
        id: content
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
        spacing: 8
        RowLayout {
            ElementsIcon { Layout.preferredWidth: 52; Layout.preferredHeight: 52 }
            Label { text: qsTr("Elements"); font.weight: Font.DemiBold; color: Theme.text; Layout.fillWidth: true }
            Label { text: root.players.length + "/2"; color: Theme.muted }
        }
        Label {
            text: qsTr("Invited by %1").arg((root.players[0] || {}).name || "")
            textFormat: Text.PlainText; color: Theme.muted; wrapMode: Text.Wrap; Layout.fillWidth: true
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            ActionButton {
                objectName: "gameJoin"; text: qsTr("Join"); primary: true
                visible: !root.participant && root.game.phase === "waiting" && root.players.length < 2
                enabled: root.canJoin
                onClicked: root.actionRequested("join", {id: root.game.id, round: root.game.round})
            }
            ActionButton {
                objectName: "gameOpen"; text: qsTr("Open"); primary: true
                visible: root.participant
                onClicked: root.openRequested()
            }
        }
    }
}
