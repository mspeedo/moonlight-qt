import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import RemoteSunshineConfig 1.0

Item {
    id: root
    property var hostInfo: ({})
    // Keep every remote settings control and description at least 14 pt.
    readonly property int settingsFontPointSize: 14
    readonly property int descriptionFontPointSize: 14
    objectName: qsTr("Sunshine Remote Settings")
    focus: true
    activeFocusOnTab: true

    RemoteSunshineConfig { id: remote }
    Component.onCompleted: remote.connectPaired(root.hostInfo)
    StackView.onDeactivating: remote.close()
    Component.onDestruction: remote.close()

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
            font.pointSize: root.descriptionFontPointSize
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Access uses your existing Moonlight pairing. No additional Sunshine administrator login is needed.")
            visible: !remote.connected
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
                font.pointSize: root.descriptionFontPointSize
                Layout.fillWidth: true
                text: remote.status
                color: remote.status.indexOf("failed") >= 0 || remote.status.indexOf("denied") >= 0
                     ? "#f36a6a" : palette.text
                wrapMode: Text.WordWrap
            }
            Button {
                font.pointSize: root.settingsFontPointSize
                text: qsTr("Retry")
                visible: !remote.connected && !remote.busy
                onClicked: remote.connectPaired(root.hostInfo)
            }
        }

        TabBar {
            id: pages
            Layout.fillWidth: true
            visible: remote.connected
            TabButton { font.pointSize: root.settingsFontPointSize; text: qsTr("Settings") }
            TabButton { font.pointSize: root.settingsFontPointSize; text: qsTr("Presets & Restart") }
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
                        font.pointSize: root.descriptionFontPointSize
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Existing non-default Sunshine options are shown below. " +
                                   "Add an option by its Sunshine configuration key to change a default value. " +
                                   "Reset removes that override. Saving does not apply changes until restart.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            font.pointSize: root.settingsFontPointSize
                            id: searchInput
                            Layout.fillWidth: true
                            placeholderText: qsTr("Filter options...")
                        }
                        Button {
                            font.pointSize: root.settingsFontPointSize
                            text: qsTr("Reload")
                            enabled: !remote.busy
                            onClicked: remote.reload()
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
                                font.pointSize: root.settingsFontPointSize
                                text: modelData.key
                                Layout.preferredWidth: 245
                                elide: Text.ElideRight
                                ToolTip {
                                    visible: mouseOver.containsMouse
                                    text: modelData.key
                                    font.pointSize: root.settingsFontPointSize
                                }
                                MouseArea {
                                    id: mouseOver
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    acceptedButtons: Qt.NoButton
                                }
                            }
                            TextField {
                                font.pointSize: root.settingsFontPointSize
                                Layout.fillWidth: true
                                text: modelData.value
                                selectByMouse: true
                                onEditingFinished: remote.setOption(modelData.key, text)
                            }
                            Button {
                                font.pointSize: root.settingsFontPointSize
                                text: qsTr("Reset")
                                onClicked: remote.removeOption(modelData.key)
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            font.pointSize: root.settingsFontPointSize
                            id: newKey
                            Layout.preferredWidth: 220
                            placeholderText: qsTr("Setting key")
                        }
                        TextField {
                            font.pointSize: root.settingsFontPointSize
                            id: newValue
                            Layout.fillWidth: true
                            placeholderText: qsTr("Value")
                        }
                        Button {
                            font.pointSize: root.settingsFontPointSize
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
                            font.pointSize: root.settingsFontPointSize
                            Layout.fillWidth: true
                            text: remote.dirty ? qsTr("Unsaved changes") : ""
                        }
                        Button {
                            font.pointSize: root.settingsFontPointSize
                            text: qsTr("Save")
                            enabled: !remote.busy
                            onClicked: remote.save(false)
                        }
                        Button {
                            font.pointSize: root.settingsFontPointSize
                            text: qsTr("Save & Restart Sunshine")
                            enabled: !remote.busy
                            onClicked: remote.save(true)
                        }
                    }
                }
            }

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    Label {
                        font.pointSize: root.descriptionFontPointSize
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Presets are stored on the Sunshine PC, not in Moonlight. " +
                                   "A preset captures the current saved configuration. " +
                                   "Switching replaces the active configuration and restarts Sunshine.")
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            font.pointSize: root.settingsFontPointSize
                            id: presetName
                            Layout.fillWidth: true
                            placeholderText: qsTr("New preset name (letters, numbers, _ or -)")
                        }
                        CheckBox {
                            font.pointSize: root.settingsFontPointSize
                            id: overwritePreset
                            text: qsTr("Overwrite existing")
                        }
                        Button {
                            font.pointSize: root.settingsFontPointSize
                            text: qsTr("Save current as preset")
                            enabled: !remote.busy && presetName.text.length > 0
                            onClicked: remote.createPreset(presetName.text, overwritePreset.checked)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        ComboBox {
                            font.pointSize: root.settingsFontPointSize
                            id: presetSelect
                            Layout.fillWidth: true
                            model: remote.presets
                            enabled: remote.presets.length > 0
                        }
                        Button {
                            font.pointSize: root.settingsFontPointSize
                            text: qsTr("Switch & Restart")
                            enabled: !remote.busy && presetSelect.currentIndex >= 0
                            onClicked: remote.switchPreset(presetSelect.currentText)
                        }
                        Button {
                            font.pointSize: root.settingsFontPointSize
                            text: qsTr("Delete")
                            enabled: !remote.busy && presetSelect.currentIndex >= 0
                            onClicked: remote.deletePreset(presetSelect.currentText)
                        }
                    }

                    Label {
                        font.pointSize: root.descriptionFontPointSize
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Before a preset is applied, Sunshine keeps a recovery copy " +
                                   "named sunshine.conf.before-remote-preset.bak beside the active configuration.")
                    }

                    Button {
                        font.pointSize: root.settingsFontPointSize
                        text: qsTr("Restart Sunshine without changing settings")
                        enabled: !remote.busy
                        onClicked: remote.restart()
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }
    }

}
