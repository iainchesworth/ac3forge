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
        // Object mode writes E-AC-3, which is a different codec in a
        // different container, so the suffix follows the mode.
        title: EncoderController.atmosEnabled ? qsTr("Save Dolby Atmos stream")
                                              : qsTr("Save AC-3 stream")
        fileMode: FileDialog.SaveFile
        defaultSuffix: EncoderController.atmosEnabled ? "ec3" : "ac3"
        nameFilters: EncoderController.atmosEnabled
                     ? [qsTr("E-AC-3 elementary stream (*.ec3)")]
                     : [qsTr("AC-3 elementary stream (*.ac3)")]
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
                            // In object mode the source layout stops being the
                            // output layout: whatever comes in becomes objects
                            // over a 5.1 bed.
                            text: EncoderController.atmosEnabled
                                  ? qsTr("E-AC-3 5.1 bed · JOC + OAMD · one object per source channel")
                                  : (EncoderController.hasLevels
                                     ? EncoderController.layoutName
                                     : qsTr("2/0 stereo")) + qsTr(" · long blocks · rematrixing on")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                        }
                    }
                }

                // ---- objects -------------------------------------------------
                Card {
                    title: qsTr("Dolby Atmos objects")

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        Switch {
                            id: atmosSwitch
                            text: qsTr("Encode as objects")
                            enabled: !EncoderController.busy
                            checked: EncoderController.atmosEnabled
                            onToggled: EncoderController.atmosEnabled = checked
                        }

                        Item { Layout.fillWidth: true }

                        // The metadata costs a few hundred bits a frame, which
                        // is not the problem. The problem is that the bed is
                        // always 5.1, so a rate that was generous for the
                        // source's own layout may not be for six channels.
                        Text {
                            visible: EncoderController.atmosEnabled
                                     && EncoderController.bitrateKbps < 384
                            text: qsTr("⚠ the bed is 5.1 — 384 kbps or more")
                            color: Theme.bad
                            font.pixelSize: Theme.fontSmall
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Every source channel becomes an object, spread either side of the point below. They are panned into a 5.1 bed that any decoder can play, and the object positions ride alongside as metadata — so a height is carried even though no bed channel can reproduce it.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: EncoderController.atmosEnabled
                        spacing: Theme.pad

                        // Plan view of the room: §4.2.1's x to the right, y
                        // towards the back, listener in the middle.
                        Rectangle {
                            id: room
                            Layout.preferredWidth: 190
                            Layout.preferredHeight: 190
                            radius: Theme.radius
                            color: Theme.surfaceAlt
                            border.color: Theme.border
                            border.width: 1

                            readonly property real spread: 0.15

                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.top: parent.top
                                anchors.topMargin: 4
                                text: qsTr("front")
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                            }

                            Rectangle {
                                anchors.centerIn: parent
                                width: 6
                                height: 6
                                radius: 3
                                color: Theme.textMuted
                            }

                            Repeater {
                                model: [-room.spread, room.spread]
                                Rectangle {
                                    width: 16
                                    height: 16
                                    radius: 8
                                    color: Theme.accent
                                    opacity: 0.9
                                    x: Math.max(0, Math.min(1, EncoderController.objectX
                                                            + modelData)) * (room.width - width)
                                    y: EncoderController.objectY * (room.height - height)
                                }
                            }

                            MouseArea {
                                anchors.fill: parent
                                enabled: !EncoderController.busy
                                onPositionChanged: (mouse) => place(mouse)
                                onPressed: (mouse) => place(mouse)
                                function place(mouse) {
                                    EncoderController.objectX = mouse.x / room.width;
                                    EncoderController.objectY = mouse.y / room.height;
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            Text {
                                text: qsTr("Height")
                                color: Theme.text
                                font.pixelSize: Theme.fontNormal
                            }

                            Slider {
                                Layout.fillWidth: true
                                from: -1.0
                                to: 1.0
                                enabled: !EncoderController.busy
                                value: EncoderController.objectZ
                                onMoved: EncoderController.objectZ = value
                            }

                            Text {
                                text: qsTr("x %1 · y %2 · z %3")
                                      .arg(EncoderController.objectX.toFixed(2))
                                      .arg(EncoderController.objectY.toFixed(2))
                                      .arg(EncoderController.objectZ.toFixed(2))
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                font.family: "monospace"
                            }

                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Height changes the metadata, not the bed — a 5.1 ring has no speakers above it. That is what an object renderer is for.")
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                wrapMode: Text.WordWrap
                            }
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
