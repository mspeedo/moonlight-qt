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

    function presetIndex() {
        var preset = Number(optionValue("nvenc_preset", "1"))
        return isFinite(preset) && preset >= 1 && preset <= 7 ? preset - 1 : 0
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
                enabled: remoteConfig && !remoteConfig.busy
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
                        currentIndex: root.presetIndex()
                        onActivated: remoteConfig.setOption("nvenc_preset", String(index + 1))
                        onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        font.pointSize: root.descriptionFontSize
                        opacity: 0.78
                        text: qsTr("Higher numbers improve compression (quality at given bitrate) at the cost of increased encoding latency. Recommended to change only when limited by network or decoder, otherwise similar effect can be accomplished by increasing bitrate.")
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
                        text: qsTr("Split the encoding of each video frame over multiple NVENC hardware units. Significantly reduces host processing latency with a marginal compression efficiency penalty. The default option enables SFE if the following conditions are met: there are 2+ NVENC units, the stream is 4K resolution or higher, and the preset is P1-P4. Set this to Enabled to use SFE at lower resolutions or higher presets.")
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
                        text: qsTr("Adds preliminary encoding pass. This allows to detect more motion vectors, better distribute bitrate across the frame and more strictly adhere to bitrate limits. Disabling it is not recommended since this can lead to occasional bitrate overshoot and subsequent packet loss.")
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
                        text: qsTr("Default: unchecked")
                    }
                    Label {
                        Layout.fillWidth: true
                        font.pointSize: root.descriptionFontSize
                        wrapMode: Text.WordWrap
                        opacity: 0.78
                        text: qsTr("Assign higher QP values to flat regions of the video. Recommended to enable when streaming at lower bitrates.")
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
                        text: qsTr("By default sunshine uses single-frame VBV/HRD, which means any encoded video frame size is not expected to exceed requested bitrate divided by requested frame rate. Relaxing this restriction can be beneficial and act as low-latency variable bitrate, but may also lead to packet loss if the network doesn't have buffer headroom to handle bitrate spikes. Maximum accepted value is 400, which corresponds to 5x increased encoded video frame upper size limit.")
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
                            text: qsTr("Use realtime priority in hardware accelerated gpu scheduling")
                            checked: root.optionEnabled("nvenc_realtime_hags", true)
                            onClicked: remoteConfig.setOption("nvenc_realtime_hags", checked ? "enabled" : "disabled")
                            onActiveFocusChanged: if (activeFocus) scroller.ensureVisible(this)
                        }
                        Label {
                            font.pointSize: root.descriptionFontSize
                            opacity: 0.78
                            text: qsTr("Default: checked")
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: root.descriptionFontSize
                            wrapMode: Text.WordWrap
                            opacity: 0.78
                            text: qsTr("Currently NVIDIA drivers may freeze in encoder when HAGS is enabled, realtime priority is used and VRAM utilization is close to maximum. Disabling this option lowers the priority to high, sidestepping the freeze at the cost of reduced capture performance when the GPU is heavily loaded.")
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
                            text: qsTr("Default: checked")
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: root.descriptionFontSize
                            wrapMode: Text.WordWrap
                            opacity: 0.78
                            text: qsTr("Sunshine requests maximum GPU clock speed while streaming to reduce encoding latency. Disabling it is not recommended since this can lead to significantly increased encoding latency.")
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
                            text: qsTr("Default: checked")
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: root.descriptionFontSize
                            wrapMode: Text.WordWrap
                            opacity: 0.78
                            text: qsTr("Sunshine can't capture fullscreen OpenGL and Vulkan programs at full frame rate unless they present on top of DXGI. This is system-wide setting that is reverted on sunshine program exit.")
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
                            text: qsTr("Default: unchecked")
                        }
                        Label {
                            Layout.fillWidth: true
                            font.pointSize: root.descriptionFontSize
                            wrapMode: Text.WordWrap
                            opacity: 0.78
                            text: qsTr("Simpler form of entropy coding. CAVLC needs around 10% more bitrate for same quality. Only relevant for really old decoding devices.")
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
