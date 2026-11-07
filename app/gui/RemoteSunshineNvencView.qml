import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

// Native representation of Sunshine's NvidiaNvencEncoder.vue. The host's
// configured overrides are authoritative; omitted keys show Sunshine defaults.
// Controls only stage edits via the shared RemoteSunshineConfig instance.
Item {
    id: root
    property var remoteConfig
    readonly property int fieldFontSize: 14
    readonly property int descriptionFontSize: 14
    readonly property bool windowsHost: remoteConfig && remoteConfig.platform === "windows"
    property bool miscExpanded: true
    readonly property var twoPassValues: ["disabled", "quarter_res", "full_res"]
    readonly property var splitValues: ["disabled", "driver_decides", "enabled"]

    function optionValue(key, fallback) {
        // Accessing settings establishes a QML dependency on stateChanged,
        // so reloads, preset switches and raw-editor changes refresh the form.
        var settings = remoteConfig ? remoteConfig.settings : []
        for (var i = 0; i < settings.length; ++i) {
            if (settings[i].key === key)
                return String(settings[i].value)
        }
        return fallback
    }

    function optionIndex(key, values, fallback) {
        var index = values.indexOf(optionValue(key, fallback))
        return index >= 0 ? index : values.indexOf(fallback)
    }

    function optionEnabled(key, defaultEnabled) {
        var value = optionValue(key, defaultEnabled ? "enabled" : "disabled").toLowerCase()
        return value === "enabled" || value === "true" ||
               value === "1" || value === "yes" || value === "on"
    }

    function optionNumber(key, fallback) {
        var value = Number(optionValue(key, String(fallback)))
        return isFinite(value) ? Math.max(0, Math.min(400, Math.round(value))) : fallback
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 10

        Label {
            Layout.fillWidth: true
            font.pointSize: 18
            font.bold: true
            text: qsTr("NVIDIA NVENC Encoder")
        }

        Label {
            Layout.fillWidth: true
            font.pointSize: root.descriptionFontSize
            wrapMode: Text.WordWrap
            text: qsTr("These options apply when Sunshine uses the NVIDIA NVENC encoder. " +
                       "Unconfigured options display Sunshine's default values. " +
                       "Changes must be saved and Sunshine restarted to take effect.")
        }

        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: form.height
            flickableDirection: Flickable.VerticalFlick
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            function ensureVisible(control) {
                if (!control)
                    return
                var pos = control.mapToItem(form, 0, 0)
                var margin = 14
                var maxScroll = Math.max(0, contentHeight - height)
                if (pos.y - margin < contentY)
                    contentY = Math.max(0, pos.y - margin)
                else if (pos.y + control.height + margin > contentY + height)
                    contentY = Math.min(maxScroll, pos.y + control.height + margin - height)
            }

            ColumnLayout {
                id: form
                width: scroller.width - 12
                spacing: 16

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Label {
                        font.pointSize: root.fieldFontSize
                        text: qsTr("Performance preset")
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        font.pointSize: root.fieldFontSize
                        model: ["P1 (fastest, default)", "P2", "P3", "P4", "P5", "P6", "P7 (slowest)"]
                        currentIndex: Math.max(0, Number(root.optionValue("nvenc_preset", "1")) - 1)
                        onActivated: remoteConfig.setOption("nvenc_preset", String(index + 1))
                        onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pointSize: root.descriptionFontSize
                        opacity: 0.78
                        text: qsTr("Higher numbers improve compression (quality at a given bitrate) at the cost of increased encoding latency. " +
                                   "Change only if limited by network or decoder; increasing bitrate can have a similar effect.")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.windowsHost
                    spacing: 5
                    Label {
                        font.pointSize: root.fieldFontSize
                        text: qsTr("Split frame encoding")
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        font.pointSize: root.fieldFontSize
                        model: [qsTr("Disabled"), qsTr("Driver decides (default)"), qsTr("Enabled")]
                        currentIndex: root.optionIndex("nvenc_split_encode", root.splitValues, "driver_decides")
                        onActivated: remoteConfig.setOption("nvenc_split_encode", root.splitValues[index])
                        onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pointSize: root.descriptionFontSize
                        opacity: 0.78
                        text: qsTr("Split frames across multiple NVENC hardware units to reduce encoding latency with a small compression penalty. " +
                                   "The driver default enables it for 2+ NVENC units, 4K or higher and presets P1-P4. " +
                                   "Enabled can also use split encoding at lower resolutions or higher presets.")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Label {
                        font.pointSize: root.fieldFontSize
                        text: qsTr("Two-pass mode")
                    }
                    ComboBox {
                        Layout.fillWidth: true
                        font.pointSize: root.fieldFontSize
                        model: [qsTr("Disabled (fastest, not recommended)"),
                                qsTr("Quarter resolution (faster, default)"),
                                qsTr("Full resolution (slower)")]
                        currentIndex: root.optionIndex("nvenc_twopass", root.twoPassValues, "quarter_res")
                        onActivated: remoteConfig.setOption("nvenc_twopass", root.twoPassValues[index])
                        onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pointSize: root.descriptionFontSize
                        opacity: 0.78
                        text: qsTr("Adds a preliminary encoding pass to detect more motion vectors, distribute bitrate and enforce bitrate limits. " +
                                   "Disabling can cause bitrate overshoot and packet loss.")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 3
                    CheckBox {
                        Layout.fillWidth: true
                        font.pointSize: root.fieldFontSize
                        text: qsTr("Spatial AQ")
                        checked: root.optionEnabled("nvenc_spatial_aq", false)
                        onClicked: remoteConfig.setOption("nvenc_spatial_aq", checked ? "enabled" : "disabled")
                        onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                    }
                    Label {
                        Layout.fillWidth: true
                        font.pointSize: root.descriptionFontSize
                        opacity: 0.78
                        text: qsTr("Disabled by default")
                    }
                    Label {
                        Layout.fillWidth: true
                        font.pointSize: root.descriptionFontSize
                        wrapMode: Text.WordWrap
                        opacity: 0.78
                        text: qsTr("Assigns higher QP values to flat image regions. Recommended at lower bitrates; may reduce performance.")
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5
                    Label {
                        font.pointSize: root.fieldFontSize
                        text: qsTr("Single-frame VBV/HRD percentage increase")
                    }
                    SpinBox {
                        Layout.preferredWidth: 210
                        font.pointSize: root.fieldFontSize
                        from: 0
                        to: 400
                        stepSize: 1
                        editable: true
                        value: root.optionNumber("nvenc_vbv_increase", 0)
                        onValueModified: remoteConfig.setOption("nvenc_vbv_increase", String(value))
                        onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                    }
                    Label {
                        Layout.fillWidth: true
                        font.pointSize: root.descriptionFontSize
                        wrapMode: Text.WordWrap
                        opacity: 0.78
                        text: qsTr("Default: 0. Allows a frame to exceed the usual single-frame bitrate limit. " +
                                   "This can improve variable-bitrate quality but may cause packet loss when the network lacks headroom. " +
                                   "Maximum 400 corresponds to five times the normal frame-size limit.")
                    }
                    Label {
                        font.pointSize: root.descriptionFontSize
                        textFormat: Text.RichText
                        text: "<a href=\"https://en.wikipedia.org/wiki/Video_buffering_verifier\">VBV/HRD</a>"
                        onLinkActivated: Qt.openUrlExternally(link)
                    }
                }

                Button {
                    Layout.fillWidth: true
                    font.pointSize: root.fieldFontSize
                    text: (root.miscExpanded ? "▼  " : "▶  ") + qsTr("Miscellaneous options")
                    onClicked: root.miscExpanded = !root.miscExpanded
                    onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: root.miscExpanded
                    spacing: 15

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.windowsHost
                        spacing: 3
                        CheckBox {
                            Layout.fillWidth: true
                            font.pointSize: root.fieldFontSize
                            text: qsTr("Use realtime priority in hardware accelerated GPU scheduling")
                            checked: root.optionEnabled("nvenc_realtime_hags", true)
                            onClicked: remoteConfig.setOption("nvenc_realtime_hags", checked ? "enabled" : "disabled")
                            onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                        }
                        Label {
                            font.pointSize: root.descriptionFontSize
                            opacity: 0.78
                            text: qsTr("Enabled by default")
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: root.descriptionFontSize
                            wrapMode: Text.WordWrap
                            opacity: 0.78
                            text: qsTr("With HAGS and nearly full VRAM, NVIDIA drivers may freeze when NVENC uses realtime priority. " +
                                       "Disabling reduces priority to high, avoiding the freeze but potentially reducing capture performance under heavy GPU load.")
                        }
                        Label {
                            font.pointSize: root.descriptionFontSize
                            textFormat: Text.RichText
                            text: "<a href=\"https://devblogs.microsoft.com/directx/hardware-accelerated-gpu-scheduling/\">HAGS</a>"
                            onLinkActivated: Qt.openUrlExternally(link)
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.windowsHost
                        spacing: 3
                        CheckBox {
                            Layout.fillWidth: true
                            font.pointSize: root.fieldFontSize
                            text: qsTr("Prefer lower encoding latency over power savings")
                            checked: root.optionEnabled("nvenc_latency_over_power", true)
                            onClicked: remoteConfig.setOption("nvenc_latency_over_power", checked ? "enabled" : "disabled")
                            onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                        }
                        Label {
                            font.pointSize: root.descriptionFontSize
                            opacity: 0.78
                            text: qsTr("Enabled by default")
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: root.descriptionFontSize
                            wrapMode: Text.WordWrap
                            opacity: 0.78
                            text: qsTr("Sunshine requests maximum GPU clocks to reduce encoding latency. Disabling may significantly increase encoding latency.")
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: root.windowsHost
                        spacing: 3
                        CheckBox {
                            Layout.fillWidth: true
                            font.pointSize: root.fieldFontSize
                            text: qsTr("Present OpenGL/Vulkan on top of DXGI")
                            checked: root.optionEnabled("nvenc_opengl_vulkan_on_dxgi", true)
                            onClicked: remoteConfig.setOption("nvenc_opengl_vulkan_on_dxgi", checked ? "enabled" : "disabled")
                            onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                        }
                        Label {
                            font.pointSize: root.descriptionFontSize
                            opacity: 0.78
                            text: qsTr("Enabled by default")
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: root.descriptionFontSize
                            wrapMode: Text.WordWrap
                            opacity: 0.78
                            text: qsTr("Allows full-rate capture of fullscreen OpenGL/Vulkan by preferring layered DXGI presentation. " +
                                       "This system-wide setting is reverted when Sunshine exits.")
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        CheckBox {
                            Layout.fillWidth: true
                            font.pointSize: root.fieldFontSize
                            text: qsTr("Prefer CAVLC over CABAC in H.264")
                            checked: root.optionEnabled("nvenc_h264_cavlc", false)
                            onClicked: remoteConfig.setOption("nvenc_h264_cavlc", checked ? "enabled" : "disabled")
                            onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                        }
                        Label {
                            font.pointSize: root.descriptionFontSize
                            opacity: 0.78
                            text: qsTr("Disabled by default")
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: root.descriptionFontSize
                            wrapMode: Text.WordWrap
                            opacity: 0.78
                            text: qsTr("Simpler entropy coding, requiring about 10% more bitrate for equal quality. Relevant mainly to older software decoders.")
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                font.pointSize: root.fieldFontSize
                text: remoteConfig && remoteConfig.dirty ? qsTr("Unsaved changes") : ""
            }
            Button {
                font.pointSize: root.fieldFontSize
                text: qsTr("Save")
                enabled: remoteConfig && !remoteConfig.busy
                onClicked: remoteConfig.save(false)
            }
            Button {
                font.pointSize: root.fieldFontSize
                text: qsTr("Save & Restart Sunshine")
                enabled: remoteConfig && !remoteConfig.busy
                onClicked: remoteConfig.save(true)
            }
        }
    }
}
