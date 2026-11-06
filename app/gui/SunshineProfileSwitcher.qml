import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import RemoteSunshineConfig 1.0

// Small, self-contained toolbar control; reuse the existing paired-client
// authorization and preset-apply/restart code from RemoteSunshineConfig.
RowLayout {
    id: root
    property var hostInfo: ({})
    spacing: 8
    implicitWidth: 355
    implicitHeight: 42

    RemoteSunshineConfig {
        id: remote
    }

    Component.onCompleted: remote.connectPaired(root.hostInfo)
    Component.onDestruction: remote.close()

    Label {
        text: qsTr("Profile:")
        font.pointSize: 14
    }

    ComboBox {
        id: presetSelect
        Layout.preferredWidth: 180
        Layout.fillHeight: true
        font.pointSize: 14
        activeFocusOnTab: true
        Keys.onLeftPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        Keys.onRightPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
        model: remote.presets
        enabled: remote.connected && !remote.busy && remote.presets.length > 0
        displayText: !remote.connected ? (remote.busy ? qsTr("Loading profiles...") : qsTr("Profiles unavailable")) :
                     (remote.presets.length === 0 ? qsTr("No saved profiles") : currentText)
        ToolTip.visible: hovered
        ToolTip.text: remote.status
        ToolTip.delay: 700
        ToolTip.timeout: 5000
    }

    Button {
        id: applyButton
        Layout.fillHeight: true
        Layout.preferredWidth: 98
        font.pointSize: 14
        activeFocusOnTab: true
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()
        Keys.onLeftPressed: nextItemInFocusChain(false).forceActiveFocus(Qt.TabFocus)
        Keys.onRightPressed: nextItemInFocusChain(true).forceActiveFocus(Qt.TabFocus)
        Keys.onDownPressed: stackView.currentItem.forceActiveFocus(Qt.TabFocus)
        text: !remote.connected && !remote.busy ? qsTr("Retry") : qsTr("Apply")
        enabled: !remote.busy &&
                 ((!remote.connected && root.hostInfo.address) ||
                  (remote.connected && presetSelect.currentIndex >= 0 && remote.presets.length > 0))
        onClicked: {
            if (!remote.connected) {
                remote.connectPaired(root.hostInfo)
            } else {
                applyConfirm.open()
            }
        }
        ToolTip.visible: hovered
        ToolTip.text: remote.connected ?
                      qsTr("Apply the selected Sunshine profile and restart Sunshine.") :
                      remote.status
        ToolTip.delay: 700
        ToolTip.timeout: 5000
    }

    NavigableDialog {
        id: applyConfirm
        title: qsTr("Switch Sunshine profile?")
        font.pointSize: 14
        standardButtons: Dialog.Yes | Dialog.No
        contentItem: Label {
            font.pointSize: 14
            wrapMode: Text.WordWrap
            text: qsTr("Apply '%1' and restart Sunshine? Active streams may disconnect.").arg(presetSelect.currentText)
        }
        onAccepted: {
            if (remote.connected && !remote.busy && presetSelect.currentIndex >= 0)
                remote.switchPreset(presetSelect.currentText)
        }
    }
}
