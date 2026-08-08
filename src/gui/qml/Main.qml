import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Our own module: brings in the Theme singleton and the EncoderController
// singleton registered from C++ (QML_ELEMENT + QML_SINGLETON). The implicit
// same-directory import covers the QML-defined types but not the C++ ones.
import Ac3Forge

ApplicationWindow {
    id: window

    width: 820
    // Tall enough that the meters and the soundfield ring are on screen
    // beside the controls rather than below the fold.
    height: 880
    minimumWidth: 640
    minimumHeight: 560
    visible: true
    title: qsTr("ac3forge — AC-3 encoder")
    color: Theme.background

    readonly property var bitrates: [96, 128, 160, 192, 224, 256, 320, 384, 448, 640]

    FileDialog {
        id: openDialog
        title: qsTr("Choose a WAV file")
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        onAccepted: EncoderController.loadSourceFile(selectedFile)
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Save AC-3 stream")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "ac3"
        nameFilters: [qsTr("AC-3 elementary stream (*.ac3)")]
        onAccepted: EncoderController.encodeTo(selectedFile)
    }

    FileDialog {
        id: recordDialog
        title: qsTr("Record to AC-3")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "ac3"
        nameFilters: [qsTr("AC-3 elementary stream (*.ac3)")]
        onAccepted: EncoderController.startRecording(deviceBox.currentIndex, selectedFile)
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: window.width
            spacing: Theme.gap
            anchors.margins: Theme.pad

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: Theme.pad
                spacing: Theme.gap

                // ---- header ------------------------------------------------
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    Text {
                        text: qsTr("ac3forge")
                        color: Theme.text
                        font.pixelSize: Theme.fontTitle
                        font.bold: true
                    }
                    Text {
                        text: qsTr("Clean-room AC-3 (ATSC A/52) encoder")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                    }
                }

                // ---- source ------------------------------------------------
                Card {
                    title: qsTr("Source")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        Button {
                            text: qsTr("Choose WAV…")
                            enabled: !EncoderController.busy
                            onClicked: openDialog.open()
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                Layout.fillWidth: true
                                text: EncoderController.sourcePath.length > 0
                                      ? EncoderController.sourcePath
                                      : qsTr("No file selected")
                                color: EncoderController.sourcePath.length > 0
                                       ? Theme.text : Theme.textMuted
                                font.pixelSize: Theme.fontNormal
                                elide: Text.ElideMiddle
                            }
                            Text {
                                text: EncoderController.sourceInfo
                                color: EncoderController.sourceReady ? Theme.good : Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                visible: text.length > 0
                            }
                        }
                    }
                }

                // ---- live capture -------------------------------------------
                Card {
                    title: qsTr("Live capture")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        ComboBox {
                            id: deviceBox
                            Layout.fillWidth: true
                            enabled: EncoderController.captureSupported && !EncoderController.busy
                            model: EncoderController.captureSupported
                                   ? EncoderController.captureDevices
                                   : [qsTr("No capture devices found")]
                        }

                        Button {
                            text: qsTr("Refresh")
                            enabled: !EncoderController.busy
                            onClicked: EncoderController.refreshCaptureDevices()
                        }

                        Button {
                            text: EncoderController.recording ? qsTr("Stop") : qsTr("Record…")
                            highlighted: EncoderController.recording
                            enabled: EncoderController.captureSupported
                                     && (EncoderController.recording || !EncoderController.busy)
                            onClicked: {
                                if (EncoderController.recording) {
                                    EncoderController.stopRecording();
                                } else {
                                    recordDialog.selectedFile = "capture.ac3";
                                    recordDialog.open();
                                }
                            }
                        }
                    }

                    Text {
                        visible: EncoderController.recording
                        text: qsTr("Recording — %1 s").arg(EncoderController.recordedSeconds.toFixed(1))
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                        font.family: "monospace"
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: !EncoderController.captureSupported
                        text: qsTr("No active capture endpoints were found. Plug in a microphone, or use a playback device's loopback entry to capture what the machine is playing.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                // ---- channel levels ------------------------------------------
                Card {
                    title: qsTr("Channel levels")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        Text {
                            text: EncoderController.hasLevels
                                  ? EncoderController.layoutName
                                  : qsTr("no source")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                            font.bold: true
                        }

                        // A steady dot while a run is live, so a frozen
                        // display is never mistaken for a silent one.
                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            visible: EncoderController.metering
                            color: Theme.bad
                        }

                        Text {
                            text: EncoderController.metering
                                  ? qsTr("live")
                                  : qsTr("peak and RMS over the whole signal")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            visible: EncoderController.hasLevels
                        }

                        Item { Layout.fillWidth: true }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: EncoderController.hasLevels
                        spacing: Theme.gap

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            spacing: 4

                            Repeater {
                                objectName: "channelMeters"
                                model: EncoderController.channelNames

                                delegate: ChannelMeter {
                                    required property int index
                                    required property string modelData

                                    Layout.fillWidth: true
                                    channelName: modelData
                                    level: index < EncoderController.channelLevels.length
                                           ? EncoderController.channelLevels[index] : ({})
                                }
                            }

                            // The scale the bars are drawn against, with the
                            // tick positions asked of the same mapping the
                            // bars themselves use.
                            Item {
                                // Inset to match ChannelMeter's track: the
                                // label to its left, and the readout and clip
                                // flag to its right, each plus a row spacing.
                                Layout.fillWidth: true
                                Layout.leftMargin: 30 + 8
                                Layout.rightMargin: 8 + 46 + 8 + 30
                                Layout.preferredHeight: 14

                                Repeater {
                                    model: [-60, -50, -40, -30, -20, -10, 0]

                                    delegate: Text {
                                        required property int modelData
                                        x: parent.width * EncoderController.meterFraction(modelData)
                                           - width / 2
                                        text: modelData
                                        color: Theme.border
                                        font.pixelSize: 9
                                    }
                                }
                            }
                        }

                        SoundfieldView {
                            Layout.alignment: Qt.AlignTop
                            visible: EncoderController.surround
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: !EncoderController.hasLevels
                        text: qsTr("Load a WAV file or start recording, and every channel it carries appears here — named and ordered as A/52 Table 5.8 defines them.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                // ---- settings ----------------------------------------------
                Card {
                    title: qsTr("Encoder settings")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        Text {
                            text: qsTr("Bit rate")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }

                        ComboBox {
                            id: bitrateBox
                            enabled: !EncoderController.busy
                            model: window.bitrates
                            currentIndex: window.bitrates.indexOf(EncoderController.bitrateKbps)
                            textRole: ""
                            displayText: currentText + " kbps"
                            onActivated: EncoderController.bitrateKbps = window.bitrates[currentIndex]
                        }

                        Item { Layout.fillWidth: true }

                        Text {
                            text: (EncoderController.hasLevels
                                   ? EncoderController.layoutName
                                   : qsTr("2/0 stereo")) + qsTr(" · long blocks · rematrixing on")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                }

                // ---- passthrough --------------------------------------------
                Card {
                    title: qsTr("Passthrough to a receiver")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        ComboBox {
                            id: outputBox
                            Layout.fillWidth: true
                            enabled: EncoderController.outputDevices.length > 0
                                     && !EncoderController.playing
                            model: EncoderController.outputDevices.length > 0
                                   ? EncoderController.outputDevices
                                   : [qsTr("No render endpoints found")]
                        }

                        Button {
                            text: qsTr("Refresh")
                            enabled: !EncoderController.playing
                            onClicked: EncoderController.refreshOutputDevices()
                        }

                        Button {
                            text: EncoderController.playing ? qsTr("Streaming…") : qsTr("Play")
                            enabled: EncoderController.canPlay && !EncoderController.playing
                                     && !EncoderController.busy
                            onClicked: EncoderController.playToReceiver(outputBox.currentIndex)
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Sends the encoded stream as IEC 61937 bursts in exclusive mode, so the receiver decodes the AC-3 itself. Only S/PDIF and HDMI endpoints can bitstream; an endpoint marked \"cannot bitstream\" takes exclusive PCM but not AC-3, while \"no exclusive access\" means exclusive mode is off or the device is in use.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                // ---- action ------------------------------------------------
                Card {
                    title: qsTr("Encode")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        Button {
                            text: EncoderController.busy ? qsTr("Encoding…") : qsTr("Encode to AC-3…")
                            enabled: EncoderController.sourceReady && !EncoderController.busy
                            highlighted: true
                            onClicked: {
                                saveDialog.selectedFile = EncoderController.suggestedOutputName();
                                saveDialog.open();
                            }
                        }

                        Button {
                            text: qsTr("Cancel")
                            visible: EncoderController.busy
                            onClicked: EncoderController.cancel()
                        }

                        ProgressBar {
                            Layout.fillWidth: true
                            visible: EncoderController.busy
                            from: 0
                            to: 1
                            value: EncoderController.progress
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: EncoderController.status
                        color: Theme.text
                        font.pixelSize: Theme.fontNormal
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }
}
