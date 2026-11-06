import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import RemoteSunshineConfig 1.0

Item {
    id: root
    property var hostInfo: ({})
    objectName: qsTr("Sunshine Remote Settings")
    focus: true
    activeFocusOnTab: true

    RemoteSunshineConfig { id: remote }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: qsTr("Sunshine on %1").arg(hostInfo.name || "PC")
            font.pointSize: 18
            font.bold: true
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Manage your paired Sunshine host with its web administrator credentials. " +
                       "These credentials stay in memory and are not saved by Moonlight.")
            visible: !remote.connected
        }

        RowLayout {
            Layout.fillWidth: true
            visible: !remote.connected
            Label { text: qsTr("User") }
            TextField {
                id: usernameInput
                Layout.preferredWidth: 170
                placeholderText: qsTr("Sunshine username")
                text: "admin"
            }
            Label { text: qsTr("Password") }
            TextField {
                id: passwordInput
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: qsTr("Sunshine password")
                onAccepted: connectButton.clicked()
            }
            Button {
                id: connectButton
                text: qsTr("Connect")
                enabled: !remote.busy
                onClicked: {
                    remote.login(root.hostInfo, usernameInput.text, passwordInput.text)
                    passwordInput.text = ""
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            BusyIndicator {
                running: remote.busy
                visible: remote.busy
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
            }
            Label {
                Layout.fillWidth: true
                text: remote.status
                color: remote.status.indexOf("failed") >= 0 || remote.status.indexOf("denied") >= 0
                     ? "#f36a6a" : palette.text
                wrapMode: Text.WordWrap
            }
        }

        TabBar {
            id: pages
            Layout.fillWidth: true
            visible: remote.connected
            TabButton { text: qsTr("Settings") }
            TabButton { text: qsTr("Presets & Restart") }
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: pages.currentIndex
            visible: remote.connected

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Existing non-default Sunshine options are shown below. " +
                                   "Add an option by its Sunshine configuration key to change a default value. " +
                                   "Reset removes that override. Saving does not apply changes until restart.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: searchInput
                            Layout.fillWidth: true
                            placeholderText: qsTr("Filter options...")
                        }
                        Button {
                            text: qsTr("Reload")
                            enabled: !remote.busy
                            onClicked: {
                                if (remote.dirty) reloadConfirm.open()
                                else remote.reload()
                            }
                        }
                    }

                    ListView {
                        id: optionList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 5
                        model: remote.settings.filter(function(option) {
                            return option.key.toLowerCase().indexOf(searchInput.text.toLowerCase()) !== -1
                        })
                        delegate: RowLayout {
                            width: optionList.width - 12
                            spacing: 8
                            Label {
                                text: modelData.key
                                Layout.preferredWidth: 245
                                elide: Text.ElideRight
                                ToolTip.visible: mouseOver.containsMouse
                                ToolTip.text: modelData.key
                                MouseArea {
                                    id: mouseOver
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.NoButton
                                }
                            }
                            TextField {
                                Layout.fillWidth: true
                                text: modelData.value
                                selectByMouse: true
                                onEditingFinished: remote.setOption(modelData.key, text)
                            }
                            Button {
                                text: qsTr("Reset")
                                onClicked: remote.removeOption(modelData.key)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: newKey
                            Layout.preferredWidth: 220
                            placeholderText: qsTr("Setting key")
                        }
                        TextField {
                            id: newValue
                            Layout.fillWidth: true
                            placeholderText: qsTr("Value")
                        }
                        Button {
                            text: qsTr("Set option")
                            onClicked: {
                                remote.setOption(newKey.text, newValue.text)
                                newKey.text = ""
                                newValue.text = ""
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: remote.dirty ? qsTr("Unsaved changes") : ""
                        }
                        Button {
                            text: qsTr("Save")
                            enabled: !remote.busy
                            onClicked: remote.save(false)
                        }
                        Button {
                            text: qsTr("Save & Restart Sunshine")
                            enabled: !remote.busy
                            onClicked: saveRestartConfirm.open()
                        }
                    }
                }
            }

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Presets are stored on the Sunshine PC, not in Moonlight. " +
                                   "A preset captures the current saved configuration. " +
                                   "Switching replaces the active configuration and restarts Sunshine.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: presetName
                            Layout.fillWidth: true
                            placeholderText: qsTr("New preset name (letters, numbers, _ or -)")
                        }
                        CheckBox {
                            id: overwritePreset
                            text: qsTr("Overwrite existing")
                        }
                        Button {
                            text: qsTr("Save current as preset")
                            enabled: !remote.busy && presetName.text.length > 0
                            onClicked: remote.createPreset(presetName.text, overwritePreset.checked)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            id: presetSelect
                            Layout.fillWidth: true
                            model: remote.presets
                            enabled: remote.presets.length > 0
                        }
                        Button {
                            text: qsTr("Switch & Restart")
                            enabled: !remote.busy && presetSelect.currentIndex >= 0
                            onClicked: switchConfirm.open()
                        }
                        Button {
                            text: qsTr("Delete")
                            enabled: !remote.busy && presetSelect.currentIndex >= 0
                            onClicked: deleteConfirm.open()
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Before a preset is applied, Sunshine keeps a recovery copy " +
                                   "named sunshine.conf.before-remote-preset.bak beside the active configuration.")
                    }

                    Button {
                        text: qsTr("Restart Sunshine without changing settings")
                        enabled: !remote.busy
                        onClicked: restartConfirm.open()
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }
    }

    Dialog {
        id: saveRestartConfirm
        title: qsTr("Apply Sunshine settings?")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Yes | Dialog.No
        contentItem: Label { text: qsTr("Save all changes and restart Sunshine? The current stream may disconnect."); wrapMode: Text.WordWrap }
        onAccepted: remote.save(true)
    }
    Dialog {
        id: switchConfirm
        title: qsTr("Switch preset?")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Yes | Dialog.No
        contentItem: Label { text: qsTr("Replace Sunshine configuration with '%1' and restart? The current stream may disconnect.").arg(presetSelect.currentText); wrapMode: Text.WordWrap }
        onAccepted: remote.switchPreset(presetSelect.currentText)
    }
    Dialog {
        id: deleteConfirm
        title: qsTr("Delete preset?")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Yes | Dialog.No
        contentItem: Label { text: qsTr("Delete '%1'? This cannot be undone.").arg(presetSelect.currentText) }
        onAccepted: remote.deletePreset(presetSelect.currentText)
    }
    Dialog {
        id: restartConfirm
        title: qsTr("Restart Sunshine?")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Yes | Dialog.No
        contentItem: Label { text: qsTr("Restart Sunshine service now? Active streams may disconnect.") }
        onAccepted: remote.restart()
    }
    Dialog {
        id: reloadConfirm
        title: qsTr("Discard changes?")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Yes | Dialog.No
        contentItem: Label { text: qsTr("Discard unsaved local edits and reload from Sunshine?") }
        onAccepted: remote.reload()
    }
}
