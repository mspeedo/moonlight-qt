import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import FlatpakUpdateManager 1.0
import SdlGamepadKeyNavigation 1.0

FocusScope {
    id: updatesPage
    objectName: qsTr("Updates")

    // Map a running OSTree package to the Git source commit recorded by R2.
    // Older deployments may predate the history catalog and have no match.
    function gitForOstree(ostreeCommit) {
        if (!ostreeCommit)
            return ""
        var versions = FlatpakUpdateManager.versions
        for (var i = 0; i < versions.length; ++i) {
            if (versions[i].ostree_commit === ostreeCommit)
                return versions[i].git_commit || ""
        }
        return ""
    }

    readonly property string runningGitCommit: gitForOstree(FlatpakUpdateManager.runningCommit)
    readonly property var selectedRelease: versionSelector.currentIndex >= 0 &&
                                           versionSelector.currentIndex < FlatpakUpdateManager.versions.length
                                           ? FlatpakUpdateManager.versions[versionSelector.currentIndex]
                                           : null

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
                // Qt Quick Controls propagates this minimum to child labels,
                // buttons and radio buttons unless explicitly overridden.
                font.pixelSize: 14
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
                        text: qsTr("Running Git commit: %1")
                              .arg(updatesPage.runningGitCommit
                                   ? updatesPage.runningGitCommit.substring(0, 12)
                                   : (FlatpakUpdateManager.versions.length > 0
                                      ? qsTr("Not recorded in R2 history")
                                      : qsTr("Check for updates")))
                        color: "#B0BEC5"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("Running Flatpak (OSTree) commit: %1")
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
                // Qt Quick Controls propagates this minimum to child labels,
                // buttons and radio buttons unless explicitly overridden.
                font.pixelSize: 14
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

                    Label {
                        text: qsTr("Published versions (newest first)")
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    ComboBox {
                        id: versionSelector
                        Layout.fillWidth: true
                        model: FlatpakUpdateManager.versions
                        textRole: "label"
                        enabled: FlatpakUpdateManager.supported &&
                                 !FlatpakUpdateManager.checking &&
                                 !FlatpakUpdateManager.installing &&
                                 !FlatpakUpdateManager.restartRequired &&
                                 FlatpakUpdateManager.versions.length > 0
                        focusPolicy: Qt.StrongFocus
                    }
                    Label {
                        visible: updatesPage.selectedRelease !== null
                        text: updatesPage.selectedRelease
                              ? qsTr("Selected Git commit: %1")
                                  .arg(updatesPage.selectedRelease.git_commit.substring(0, 12))
                              : ""
                        color: "#B0BEC5"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: updatesPage.selectedRelease !== null
                        text: updatesPage.selectedRelease
                              ? qsTr("Selected Flatpak (OSTree) commit: %1")
                                  .arg(updatesPage.selectedRelease.ostree_commit.substring(0, 12))
                              : ""
                        color: "#B0BEC5"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: FlatpakUpdateManager.historyLoading
                        text: qsTr("Loading published version history...")
                        color: "#B0BEC5"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        visible: FlatpakUpdateManager.historyError.length > 0
                        text: FlatpakUpdateManager.historyError
                        color: "#FFB74D"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    Label {
                        text: qsTr("Historical versions use a signed OSTree commit. System-wide downgrades may require administrator authorization in Desktop Mode.")
                        color: "#B0BEC5"
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }

                    Button {
                        id: installButton
                        text: FlatpakUpdateManager.installing ? qsTr("Installing...") : qsTr("Install selected update")
                        enabled: versionSelector.currentIndex >= 0 &&
                                 !FlatpakUpdateManager.checking &&
                                 !FlatpakUpdateManager.installing &&
                                 !FlatpakUpdateManager.restartRequired &&
                                 FlatpakUpdateManager.versions.length > versionSelector.currentIndex &&
                                 FlatpakUpdateManager.canInstallVersion(
                                     FlatpakUpdateManager.versions[versionSelector.currentIndex].ostree_commit)
                        onClicked: FlatpakUpdateManager.installVersion(
                                       FlatpakUpdateManager.versions[versionSelector.currentIndex].ostree_commit)
                        focusPolicy: Qt.StrongFocus
                    }
                }
            }

            GroupBox {
                // Qt Quick Controls propagates this minimum to child labels,
                // buttons and radio buttons unless explicitly overridden.
                font.pixelSize: 14
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
                        indeterminate: FlatpakUpdateManager.installingHistorical
                        visible: FlatpakUpdateManager.installing ||
                                 FlatpakUpdateManager.restartRequired
                    }

                    Label {
                        visible: installProgress.visible && !FlatpakUpdateManager.installingHistorical
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
