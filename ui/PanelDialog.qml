import QtQuick
import QtQuick.Controls.Basic

Dialog {
    id: root
    property int buttons: Dialog.NoButton
    popupType: Popup.Item
    background: Rectangle { radius: Theme.panelRadius; color: Theme.surface; border.color: Theme.border }
    footer: DialogButtonBox {
        visible: root.buttons !== Dialog.NoButton
        ActionButton {
            objectName: "dialogCloseButton"
            visible: root.buttons & Dialog.Close
            text: qsTranslate("Channels", "Close")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        ActionButton {
            objectName: "dialogCancelButton"
            visible: root.buttons & Dialog.Cancel
            text: qsTranslate("Channels", "Cancel")
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }
}
