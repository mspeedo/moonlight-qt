import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import SdlGamepadKeyNavigation 1.0
import LocalVrrBenchmarkController 1.0

Flickable {
    id: root
    objectName: qsTr("Local VRR Benchmark")
    focus: true
    activeFocusOnTab: true
    boundsBehavior: Flickable.StopAtBounds
    contentWidth: width
    contentHeight: panel.implicitHeight + 32
    clip: true
    ScrollBar.vertical: ScrollBar {}

    LocalVrrBenchmarkController {
        id: benchmark
        onFinished: {
            // Focus remains on the Run button after SDL relinquishes the screen.
            root.forceActiveFocus(Qt.TabFocusReason)
            runButton.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    function ensureVisible(item) {
        var p = item.mapToItem(contentItem, 0, 0)
        var margin = 28
        if (p.y < contentY + margin)
            contentY = Math.max(0, p.y - margin)
        else if (p.y + item.height > contentY + height - margin)
            contentY = Math.min(Math.max(0, contentHeight - height),
                                p.y + item.height + margin - height)
    }

    Window.onActiveFocusItemChanged: {
        if (Window.activeFocusItem && root.visible) {
            var item = Window.activeFocusItem
            while (item && item.parent !== root.contentItem)
                item = item.parent
            if (item) root.ensureVisible(item)
        }
    }
    StackView.onActivated: {
        SdlGamepadKeyNavigation.setUiNavMode(true)
        if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0)
            fpsChoice.forceActiveFocus(Qt.TabFocusReason)
    }
    StackView.onDeactivating: SdlGamepadKeyNavigation.setUiNavMode(false)

    ColumnLayout {
        id: panel
        x: 24
        y: 16
        width: Math.max(320, root.width - 48)
        spacing: 14

        Label {
            Layout.fillWidth: true
            text: qsTr("Offline Gamescope VRR rendering diagnostic")
            font.pointSize: 20
            font.bold: true
            wrapMode: Text.WordWrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Tests Moonlight's actual Pacer and libplacebo/Vulkan rendering without Sunshine, networking or decoding. Runs fullscreen in this SteamOS Game Mode session.")
            font.pointSize: 13
            wrapMode: Text.WordWrap
        }

        GroupBox {
            title: qsTr("Benchmark settings")
            font.pointSize: 14
            Layout.fillWidth: true
            GridLayout {
                width: parent.width
                columns: root.width >= 750 ? 2 : 1
                columnSpacing: 20
                rowSpacing: 10

                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Producer frame rate"); font.pointSize: 14 }
                    ComboBox {
                        id: fpsChoice
                        Layout.fillWidth: true
                        font.pointSize: 14
                        model: [qsTr("Variable 80–110 FPS (repeatable)"), qsTr("Constant 90 FPS")]
                        currentIndex: 0
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Swapchain acquisition"); font.pointSize: 14 }
                    ComboBox {
                        id: acquireChoice
                        Layout.fillWidth: true
                        font.pointSize: 14
                        model: [qsTr("Early (current Moonlight baseline)"), qsTr("Late (experimental)")]
                        currentIndex: 0
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Benchmark length"); font.pointSize: 14 }
                    ComboBox {
                        id: durationChoice
                        Layout.fillWidth: true
                        font.pointSize: 14
                        model: [qsTr("60 seconds"), qsTr("30 seconds"), qsTr("120 seconds"), qsTr("Until I stop it")]
                        currentIndex: 0
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("In-test telemetry"); font.pointSize: 14 }
                    CheckBox {
                        id: hudChoice
                        text: qsTr("Show diagnostic HUD")
                        checked: true
                        font.pointSize: 14
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Button {
                id: runButton
                text: qsTr("Start VRR benchmark")
                font.pointSize: 15
                enabled: !benchmark.running
                highlighted: true
                onClicked: {
                    var durations = [60, 30, 120, 0]
                    benchmark.start(root.Window.window, fpsChoice.currentIndex === 0,
                                    acquireChoice.currentIndex === 1,
                                    durations[durationChoice.currentIndex], hudChoice.checked)
                }
            }
            Label {
                Layout.fillWidth: true
                text: benchmark.status
                font.pointSize: 13
                wrapMode: Text.WordWrap
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("During the test: X switches frame rates • Y switches acquisition order • A toggles HUD • B returns to Moonlight. Keyboard: X/Y/H/Esc. The selected duration also ends automatically.")
            wrapMode: Text.WordWrap
            font.pointSize: 13
        }

        GroupBox {
            title: qsTr("Last benchmark results")
            Layout.fillWidth: true
            font.pointSize: 14
            ColumnLayout {
                anchors.fill: parent
                spacing: 6
                Label {
                    text: Object.keys(benchmark.results).length === 0
                          ? qsTr("Run a benchmark to display the results here.")
                          : qsTr("CPU intervals are not display timestamps; Gamescope presentation feedback is shown only if supported.")
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                    font.pointSize: 13
                }
                GridLayout {
                    visible: Object.keys(benchmark.results).length > 0
                    columns: 2
                    columnSpacing: 25
                    rowSpacing: 5
                    Label { text: qsTr("Frames produced / submitted / dropped"); font.pointSize: 13 }
                    Label {
                        font.pointSize: 13
                        text: benchmark.results.produced + " / " + benchmark.results.submitted +
                              " / " + benchmark.results.dropped
                    }
                    Label { text: qsTr("Producer interval (avg / max)"); font.pointSize: 13 }
                    Label { font.pointSize: 13; text: root.intervalText("produce") }
                    Label { text: qsTr("Swapchain acquire (avg / max)"); font.pointSize: 13 }
                    Label { font.pointSize: 13; text: root.intervalText("acquire") }
                    Label { text: qsTr("CPU render (avg / max)"); font.pointSize: 13 }
                    Label { font.pointSize: 13; text: root.intervalText("render") }
                    Label { text: qsTr("CPU submission interval (avg / max)"); font.pointSize: 13 }
                    Label { font.pointSize: 13; text: root.intervalText("submit") }
                    Label { text: qsTr("Gamescope presentation (avg / max)"); font.pointSize: 13 }
                    Label { font.pointSize: 13; text: benchmark.results.presented > 1 ? root.intervalText("present") : qsTr("Not available") }
                    Label { text: qsTr("Compositor presents / discards"); font.pointSize: 13 }
                    Label { font.pointSize: 13; text: benchmark.results.presented + " / " + benchmark.results.discarded }
                }
                Label {
                    Layout.fillWidth: true
                    visible: Object.keys(benchmark.results).length > 0
                    wrapMode: Text.WrapAnywhere
                    font.pointSize: 12
                    text: qsTr("Event-by-event CSV: %1").arg(benchmark.results.csv || qsTr("Not saved"))
                }
            }
        }

        GroupBox {
            title: qsTr("A/B comparison history (current Moonlight session)")
            Layout.fillWidth: true
            font.pointSize: 14
            ColumnLayout {
                width: parent.width
                spacing: 7
                Label {
                    visible: benchmark.history.length === 0
                    Layout.fillWidth: true
                    font.pointSize: 13
                    text: qsTr("Run several configurations to compare them here.")
                    wrapMode: Text.WordWrap
                }
                Repeater {
                    model: benchmark.history
                    delegate: Label {
                        Layout.fillWidth: true
                        font.pointSize: 12
                        wrapMode: Text.WordWrap
                        text: (index + 1) + ". " + modelData.mode + " | " + modelData.ordering +
                              " | " + (modelData.duration === 0 ? qsTr("manual") : modelData.duration + " s") +
                              "  — " + qsTr("drops") + ": " + modelData.dropped +
                              ", " + qsTr("acquire avg") + ": " + Number(modelData.acquireAvg).toFixed(2) + " ms" +
                              ", " + qsTr("present avg") + ": " +
                              (modelData.presented > 1 ? Number(modelData.presentAvg).toFixed(2) + " ms" : "N/A")
                    }
                }
                Button {
                    text: qsTr("Clear comparison history")
                    enabled: benchmark.history.length > 0 && !benchmark.running
                    onClicked: benchmark.clearHistory()
                }
            }
        }
    }

    function intervalText(metric) {
        var data = benchmark.results
        if (!data || data[metric + "Avg"] === undefined)
            return "—"
        return Number(data[metric + "Avg"]).toFixed(2) + " / " +
               Number(data[metric + "Max"]).toFixed(2) + " ms"
    }
}
