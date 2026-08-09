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

    width: 900
    // Tall enough that the meters and the soundfield ring are on screen
    // beside the controls rather than below the fold.
    height: 940
    minimumWidth: 720
    minimumHeight: 560
    visible: true
    title: qsTr("ac3forge — AC-3 / E-AC-3 encoder")
    color: Theme.background

    // Fusion draws every standard control - Button, CheckBox, Switch,
    // Slider, ProgressBar, ComboBox, SpinBox - from these palette roles (see
    // e.g. QtQuick/Controls/Fusion/impl/SwitchIndicator.qml's
    // Fusion.buttonColor(control.palette, ...) calls), never from a literal.
    // Left unset, Fusion falls back to its own default palette regardless of
    // Theme - the "pale pink on every switch and slider" the handoff calls
    // out as the single most visible inconsistency today. Setting it here,
    // on the root window, means every control inherits it unless a control
    // overrides its own palette.
    palette.window: Theme.bg
    palette.windowText: Theme.text
    palette.base: Theme.surface
    palette.alternateBase: Theme.neutral100
    palette.text: Theme.text
    palette.button: Theme.surface
    palette.buttonText: Theme.text
    palette.brightText: Theme.text
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.bg
    palette.light: Theme.neutral100
    palette.midlight: Theme.neutral200
    palette.mid: Theme.neutral400
    palette.dark: Theme.neutral600
    palette.shadow: Theme.neutral900
    palette.toolTipBase: Theme.surface
    palette.toolTipText: Theme.text
    palette.placeholderText: Theme.textMuted

    FileDialog {
        id: openDialog
        title: qsTr("Choose a WAV file")
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        onAccepted: EncoderController.loadSourceFile(selectedFile)
    }

    // The suffix and the filter follow the plan rather than being typed, so a
    // .ac3 file can never end up holding E-AC-3. Both are set when the dialog
    // is opened: outputSuffix() is a method, and a binding to it would go
    // stale the moment the codec or container changed.
    FileDialog {
        id: saveDialog
        title: qsTr("Save encoded audio")
        fileMode: FileDialog.SaveFile
        onAccepted: EncoderController.encodeTo(selectedFile)
    }

    FileDialog {
        id: recordDialog
        title: qsTr("Record to a file")
        fileMode: FileDialog.SaveFile
        onAccepted: EncoderController.startRecording(deviceBox.currentIndex, selectedFile)
    }

    function openSaveDialog(dialog, name) {
        const suffix = EncoderController.outputSuffix();
        dialog.defaultSuffix = suffix;
        dialog.nameFilters = [qsTr("%1 file (*.%2)").arg(suffix.toUpperCase()).arg(suffix),
                              qsTr("All files (*)")];
        dialog.selectedFile = name;
        dialog.open();
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
                        text: qsTr("Clean-room AC-3 / E-AC-3 (ATSC A/52, ETSI TS 103 420) encoder")
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
                                    window.openSaveDialog(recordDialog,
                                                          "capture." + EncoderController.outputSuffix());
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
                        text: qsTr("A capture endpoint feeds the same format, layout and metadata a file does — its channels are routed onto whatever layout is selected below.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                        visible: EncoderController.captureSupported
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
                                Layout.leftMargin: 58 + 8
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
                        text: qsTr("Load a WAV file or start recording, and every channel it carries appears here — named and ordered as A/52 Table 5.8 defines them. During an encode the meters follow the CODED channels, which for an immersive layout include the bed a 5.1 decoder would play.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                // ---- format --------------------------------------------------
                Card {
                    title: qsTr("Format")

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 4
                        columnSpacing: Theme.gap
                        rowSpacing: Theme.gap

                        Text {
                            text: qsTr("Codec")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            enabled: !EncoderController.busy && !EncoderController.atmosEnabled
                            model: EncoderController.codecNames
                            currentIndex: EncoderController.codecIndex
                            onActivated: EncoderController.codecIndex = currentIndex
                        }

                        Text {
                            text: qsTr("Layout")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            enabled: !EncoderController.busy && !EncoderController.atmosEnabled
                            model: EncoderController.layoutNames
                            currentIndex: EncoderController.layoutIndex
                            onActivated: EncoderController.layoutIndex = currentIndex
                        }

                        Text {
                            text: qsTr("Bit rate")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            enabled: !EncoderController.busy
                            model: EncoderController.bitrates
                            currentIndex: EncoderController.bitrates.indexOf(
                                              EncoderController.bitrateKbps)
                            displayText: currentText + " kbps"
                            onActivated: EncoderController.bitrateKbps =
                                             EncoderController.bitrates[currentIndex]
                        }

                        Text {
                            text: qsTr("Container")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            enabled: !EncoderController.busy
                            model: EncoderController.containerNames
                            currentIndex: EncoderController.containerIndex
                            onActivated: EncoderController.containerIndex = currentIndex
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: EncoderController.layoutDetail
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    Text {
                        Layout.fillWidth: true
                        text: EncoderController.routingSummary
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                        visible: text.length > 0
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: EncoderController.atmosEnabled
                        text: qsTr("Object mode is on, so the codec and layout are fixed: objects ride in an E-AC-3 stream over a 5.1 bed.")
                        color: Theme.warn
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }
                }

                // ---- Annex E coding tools -----------------------------------
                Card {
                    title: qsTr("Annex E coding tools")
                    visible: EncoderController.toolsAvailable

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Each of these buys bits somewhere and spends quality somewhere else, so none is on by default. Encoding the same material with and without one is the only way to say whether it earned its place.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 3
                        columnSpacing: Theme.gap
                        rowSpacing: 4

                        CheckBox {
                            text: qsTr("Channel coupling")
                            enabled: !EncoderController.busy
                            checked: EncoderController.coupling
                            onToggled: EncoderController.coupling = checked
                        }
                        Text {
                            text: qsTr("begin band")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            visible: EncoderController.coupling
                        }
                        SpinBox {
                            from: -1
                            to: 15
                            enabled: !EncoderController.busy
                            visible: EncoderController.coupling
                            value: EncoderController.cplBegf
                            textFromValue: (value) => value < 0 ? qsTr("auto") : String(value)
                            valueFromText: (text) => text === qsTr("auto") ? -1 : parseInt(text)
                            onValueModified: EncoderController.cplBegf = value
                        }

                        CheckBox {
                            text: qsTr("Spectral extension")
                            enabled: !EncoderController.busy
                            checked: EncoderController.spx
                            onToggled: EncoderController.spx = checked
                        }
                        Text {
                            text: qsTr("begin band")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            visible: EncoderController.spx
                        }
                        SpinBox {
                            from: -1
                            to: 7
                            enabled: !EncoderController.busy
                            visible: EncoderController.spx
                            value: EncoderController.spxBegf
                            textFromValue: (value) => value < 0 ? qsTr("auto") : String(value)
                            valueFromText: (text) => text === qsTr("auto") ? -1 : parseInt(text)
                            onValueModified: EncoderController.spxBegf = value
                        }

                        CheckBox {
                            text: qsTr("Adaptive hybrid transform")
                            enabled: !EncoderController.busy
                            checked: EncoderController.aht
                            onToggled: EncoderController.aht = checked
                        }
                        Text {
                            text: qsTr("GAQ mode")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            visible: EncoderController.aht
                        }
                        SpinBox {
                            from: -1
                            to: 3
                            enabled: !EncoderController.busy
                            visible: EncoderController.aht
                            value: EncoderController.gaqMode
                            textFromValue: (value) => value < 0 ? qsTr("auto") : String(value)
                            valueFromText: (text) => text === qsTr("auto") ? -1 : parseInt(text)
                            onValueModified: EncoderController.gaqMode = value
                        }
                    }

                    CheckBox {
                        text: qsTr("Attenuate the spectral-extension seam")
                        visible: EncoderController.spx
                        enabled: !EncoderController.busy
                        checked: EncoderController.spxAtten
                        onToggled: EncoderController.spxAtten = checked
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("GAQ mode 0 is the transform with gain-adaptive quantisation switched off, which is how GAQ's own contribution gets measured.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                        visible: EncoderController.aht
                    }

                    // The same selection in the vocabulary ac3cli takes, so a
                    // setting found here can be reproduced on the command line.
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("ac3cli tools token:  %1").arg(EncoderController.toolsToken)
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        font.family: "monospace"
                    }
                }

                // ---- metadata -------------------------------------------------
                Card {
                    title: qsTr("Dynamic range and metadata")

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 4
                        columnSpacing: Theme.gap
                        rowSpacing: Theme.gap

                        Text {
                            text: qsTr("DRC profile")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            enabled: !EncoderController.busy
                            model: EncoderController.drcNames
                            currentIndex: EncoderController.drcIndex
                            onActivated: EncoderController.drcIndex = currentIndex
                        }

                        Text {
                            text: qsTr("dialnorm")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            SpinBox {
                                from: 1
                                to: 31
                                enabled: !EncoderController.busy
                                         && !EncoderController.measureDialnorm
                                value: EncoderController.dialnorm
                                onValueModified: EncoderController.dialnorm = value
                            }
                            CheckBox {
                                text: qsTr("measure")
                                enabled: !EncoderController.busy
                                checked: EncoderController.measureDialnorm
                                onToggled: EncoderController.measureDialnorm = checked
                            }
                        }

                        Text {
                            text: qsTr("Centre downmix")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            enabled: !EncoderController.busy
                            model: EncoderController.cmixNames
                            currentIndex: EncoderController.cmixIndex
                            onActivated: EncoderController.cmixIndex = currentIndex
                        }

                        Text {
                            text: qsTr("Surround downmix")
                            color: Theme.text
                            font.pixelSize: Theme.fontNormal
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            enabled: !EncoderController.busy
                            model: EncoderController.surmixNames
                            currentIndex: EncoderController.surmixIndex
                            onActivated: EncoderController.surmixIndex = currentIndex
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("dialnorm says where dialogue sits below full scale (§5.4.2.8). Measuring derives it from BS.1770-4 gated loudness over the whole programme; getting it wrong is not cosmetic, since a levelled system plays the difference.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.gap

                        CheckBox {
                            text: qsTr("Heavy compression")
                            enabled: !EncoderController.busy
                            checked: EncoderController.heavy
                            onToggled: EncoderController.heavy = checked
                        }

                        Text {
                            text: qsTr("ceiling")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            visible: EncoderController.heavy
                        }
                        // Counted in tenths of a decibel: the default ceiling
                        // is -0.5 dBFS, and a whole-number box would show it
                        // as 0 and write that back — throwing away exactly the
                        // headroom §7.7.2 exists to reserve.
                        SpinBox {
                            from: -200
                            to: 0
                            stepSize: 5
                            enabled: !EncoderController.busy
                            visible: EncoderController.heavy
                            value: Math.round(EncoderController.ceilingDb * 10)
                            textFromValue: (value) => (value / 10).toFixed(1) + " dBFS"
                            valueFromText: (text) => Math.round(parseFloat(text) * 10)
                            onValueModified: EncoderController.ceilingDb = value / 10
                        }

                        Text {
                            text: qsTr("dialogue at")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            visible: EncoderController.heavy
                        }
                        SpinBox {
                            from: -40
                            to: -5
                            enabled: !EncoderController.busy
                            visible: EncoderController.heavy
                            value: Math.round(EncoderController.dialogueDb)
                            textFromValue: (value) => value + " dBFS"
                            valueFromText: (text) => parseInt(text)
                            onValueModified: EncoderController.dialogueDb = value
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("Heavy compression (§7.7.2) is a peak ceiling in the mono downmix at syncframe resolution — an assurance for links that overmodulate, not the subjectively pleasing reduction dynrng provides.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                        visible: EncoderController.heavy
                    }

                    // ---- mixing metadata: E-AC-3 only ------------------------
                    RowLayout {
                        Layout.fillWidth: true
                        visible: EncoderController.mixmetaAvailable
                        spacing: Theme.gap

                        CheckBox {
                            text: qsTr("Mixing metadata")
                            enabled: !EncoderController.busy
                            checked: EncoderController.mixmeta
                            onToggled: EncoderController.mixmeta = checked
                        }

                        Text {
                            text: qsTr("preferred downmix")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            visible: EncoderController.mixmeta
                        }
                        ComboBox {
                            enabled: !EncoderController.busy
                            visible: EncoderController.mixmeta
                            model: EncoderController.dmixNames
                            currentIndex: EncoderController.dmixIndex
                            onActivated: EncoderController.dmixIndex = currentIndex
                        }

                        Text {
                            text: qsTr("LFE mix")
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
                            visible: EncoderController.mixmeta
                        }
                        SpinBox {
                            from: -1
                            to: 31
                            enabled: !EncoderController.busy
                            visible: EncoderController.mixmeta
                            value: EncoderController.lfeMix
                            // §E2.3.1.11: the level in dB is 10 - the code, so
                            // 0 is the +10 dB §7.8 calls ideal.
                            textFromValue: (value) => value < 0
                                           ? qsTr("off") : (10 - value) + " dB"
                            valueFromText: (text) => text === qsTr("off") ? -1 : parseInt(text)
                            onValueModified: EncoderController.lfeMix = value
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: qsTr("E-AC-3 dropped bsi's two coarse levels and carries a richer group inside mixmdate instead (Table E1.2), including an LFE mix level AC-3 has no way to express. \"Off\" is a decision in its own right: LFE mixing disabled, not merely turned down.")
                        color: Theme.textMuted
                        font.pixelSize: Theme.fontSmall
                        wrapMode: Text.WordWrap
                        visible: EncoderController.mixmetaAvailable && EncoderController.mixmeta
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

                        Text {
                            visible: EncoderController.atmosEnabled
                                     && EncoderController.objectCount > 0
                            text: qsTr("%1 objects + the bed's LFE")
                                  .arg(EncoderController.objectCount)
                            color: Theme.textMuted
                            font.pixelSize: Theme.fontSmall
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

                            // One marker per object, at the offset the encoder
                            // will actually place it: the same even spread
                            // either side of the chosen point.
                            Repeater {
                                model: Math.max(EncoderController.objectCount, 1)

                                Rectangle {
                                    required property int index

                                    readonly property int count:
                                        Math.max(EncoderController.objectCount, 1)
                                    readonly property real offset: count < 2
                                        ? 0
                                        : EncoderController.objectSpread
                                          * (2 * index / (count - 1) - 1)

                                    width: 16
                                    height: 16
                                    radius: 8
                                    color: Theme.accent
                                    opacity: 0.9
                                    x: Math.max(0, Math.min(1, EncoderController.objectX
                                                            + offset)) * (room.width - width)
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
                                text: qsTr("Spread")
                                color: Theme.text
                                font.pixelSize: Theme.fontNormal
                            }

                            Slider {
                                Layout.fillWidth: true
                                from: 0.0
                                to: 0.5
                                enabled: !EncoderController.busy
                                value: EncoderController.objectSpread
                                onMoved: EncoderController.objectSpread = value
                            }

                            Text {
                                text: qsTr("LFE send")
                                color: Theme.text
                                font.pixelSize: Theme.fontNormal
                            }

                            Slider {
                                Layout.fillWidth: true
                                from: 0.0
                                to: 1.0
                                enabled: !EncoderController.busy
                                value: EncoderController.objectLfeSend
                                onMoved: EncoderController.objectLfeSend = value
                            }

                            Text {
                                text: qsTr("x %1 · y %2 · z %3 · spread %4 · lfe %5")
                                      .arg(EncoderController.objectX.toFixed(2))
                                      .arg(EncoderController.objectY.toFixed(2))
                                      .arg(EncoderController.objectZ.toFixed(2))
                                      .arg(EncoderController.objectSpread.toFixed(2))
                                      .arg(EncoderController.objectLfeSend.toFixed(2))
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                font.family: "monospace"
                            }

                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Height changes the metadata, not the bed — a 5.1 ring has no speakers above it. Spread matters because objects reaching the bed by the same route are exactly the ones JOC cannot pull apart again. The LFE send is the only route to that channel: no direction points at it, so panning never reaches it.")
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
                        text: qsTr("Sends the encoded stream as IEC 61937 bursts in exclusive mode, so the receiver decodes it. The packer emits AC-3 bursts only (data type 1), so an E-AC-3 stream is refused here rather than sent as something it is not. Only S/PDIF and HDMI endpoints can bitstream at all.")
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
                            text: EncoderController.busy
                                  ? qsTr("Encoding…")
                                  : qsTr("Encode to .%1…").arg(EncoderController.outputSuffix())
                            enabled: EncoderController.sourceReady && !EncoderController.busy
                            highlighted: true
                            onClicked: window.openSaveDialog(
                                           saveDialog,
                                           EncoderController.suggestedOutputName())
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
