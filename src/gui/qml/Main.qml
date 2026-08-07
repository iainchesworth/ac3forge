import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

ApplicationWindow {
    id: window

    width: 820
    height: 680
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

                // ---- capture (pending the WASAPI backend) ------------------
                Card {
                    title: qsTr("Live capture")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        ComboBox {
                            Layout.fillWidth: true
                            enabled: EncoderController.captureSupported
                            model: EncoderController.captureSupported
                                   ? EncoderController.captureDevices
                                   : [qsTr("No capture backend yet")]
                        }

                        Button {
                            text: qsTr("Record")
                            enabled: EncoderController.captureSupported
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: !EncoderController.captureSupported
                        text: qsTr("Microphone and system-loopback capture arrive with the WASAPI backend — the next item on the roadmap.")
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
                            text: qsTr("2.0 stereo · long blocks · rematrixing on")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
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
