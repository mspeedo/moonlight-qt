import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import FlatpakUpdateManager 1.0
import SdlGamepadKeyNavigation 1.0

FocusScope {
    id: updatesPage
    objectName: qsTr("Updates")

    StackView.onActivated: {
        SdlGamepadKeyNavigation.setUiNavMode(true)
        checkButton.forceActiveFocus(Qt.TabFocus)
    }
    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setUiNavMode(false)
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 18
        contentWidth: width
        contentHeight: contentColumn.implicitHeight + 25
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: contentColumn
            width: parent.width
            spacing: 18

            GroupBox {
                title: qsTr("Installed version")
                Layout.fillWidth: true
                padding: 15

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    Label {
                        text: qsTr("Moonlight %1").arg(FlatpakUpdateManager.runningVersion)
                        font.pixelSize: 20
                        font.bold: true
                    }
                    Label {
                        text: qsTr("Running Flatpak commit: %1")
                              .arg(FlatpakUpdateManager.runningCommit
                                   ? FlatpakUpdateManager.runningCommit.substring(0, 12)
                                   : qsTr("Not reported yet"))
                        color: "#B0BEC5"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("Updates are installed without interrupting Moonlight. They take effect after a manual restart.")
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        color: "#B0BEC5"
                    }
                }
            }

            GroupBox {
                title: qsTr("Available updates")
                Layout.fillWidth: true
                padding: 15

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Button {
                            id: checkButton
                            text: FlatpakUpdateManager.checking ? qsTr("Checking...") : qsTr("Check for updates")
                            enabled: FlatpakUpdateManager.supported &&
                                     !FlatpakUpdateManager.checking &&
                                     !FlatpakUpdateManager.installing
                            onClicked: FlatpakUpdateManager.checkForUpdates()
                            focusPolicy: Qt.StrongFocus
                        }
                        BusyIndicator {
                            implicitWidth: 30
                            implicitHeight: 30
                            running: FlatpakUpdateManager.checking
                            visible: running
                        }
                        Item { Layout.fillWidth: true }
                    }

                    Label {
                        visible: !FlatpakUpdateManager.supported
                        text: qsTr("In-app updating requires Moonlight to be installed from a Flatpak remote.")
                        color: "#FFB74D"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    Label {
                        text: FlatpakUpdateManager.statusText
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    Label {
                        visible: FlatpakUpdateManager.errorText.length > 0
                        text: FlatpakUpdateManager.errorText
                        color: "#EF9A9A"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    RadioButton {
                        id: latestBuild
                        text: FlatpakUpdateManager.updateAvailable
                              ? qsTr("Latest available build · %1").arg(FlatpakUpdateManager.latestCommit.substring(0, 12))
                              : qsTr("Latest available build")
                        checked: true
                        enabled: FlatpakUpdateManager.updateAvailable &&
                                 !FlatpakUpdateManager.installing &&
                                 !FlatpakUpdateManager.restartRequired
                        focusPolicy: Qt.StrongFocus
                    }

                    Label {
                        text: qsTr("MVP: only the newest build from your Flatpak remote is supported. Previous builds and rollback are not available.")
                        font.pixelSize: 12
                        color: "#B0BEC5"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    Button {
                        id: installButton
                        text: FlatpakUpdateManager.installing ? qsTr("Installing...") : qsTr("Install selected update")
                        enabled: FlatpakUpdateManager.supported &&
                                 FlatpakUpdateManager.updateAvailable &&
                                 !FlatpakUpdateManager.checking &&
                                 !FlatpakUpdateManager.installing &&
                                 !FlatpakUpdateManager.restartRequired
                        onClicked: FlatpakUpdateManager.installLatest()
                        focusPolicy: Qt.StrongFocus
                    }
                }
            }

            GroupBox {
                title: qsTr("Installation status")
                Layout.fillWidth: true
                padding: 15

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    ProgressBar {
                        id: installProgress
                        Layout.fillWidth: true
                        from: 0
                        to: 100
                        value: FlatpakUpdateManager.progress
                        visible: FlatpakUpdateManager.installing ||
                                 FlatpakUpdateManager.restartRequired
                    }

                    Label {
                        visible: installProgress.visible
                        text: qsTr("%1% complete").arg(FlatpakUpdateManager.progress)
                    }

                    Label {
                        text: FlatpakUpdateManager.restartRequired
                              ? qsTr("Installed — restart Moonlight from Steam whenever you're ready. Your current session stays active.")
                              : qsTr("No automatic restart. You can continue using Moonlight while an update installs.")
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        color: FlatpakUpdateManager.restartRequired ? "#A5D6A7" : "#B0BEC5"
                    }
                }
            }
        }
    }
}
