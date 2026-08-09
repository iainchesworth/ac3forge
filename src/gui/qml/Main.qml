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

    // The handoff's honest floor: below 1280x900 the rail (340px) and the
    // Format grid (180px columns) no longer have room to reflow rather than
    // clip. The brief's 720x560 predates this content and is not achievable
    // with it.
    width: 1280
    height: 900
    minimumWidth: 1280
    minimumHeight: 900
    visible: true
    color: Theme.background

    // "live capture" is approximated from `recording` until the input model
    // is unified (the handoff's "one input, with a source selector") - that
    // lands with the channel-model checkpoint, alongside the Monitor
    // behaviour change it depends on.
    function baseName(path) {
        const normalized = path.replace(/\\/g, "/");
        const slash = normalized.lastIndexOf("/");
        return slash >= 0 ? normalized.substring(slash + 1) : normalized;
    }
    readonly property string sourceLabel: EncoderController.recording
                                           ? qsTr("live capture")
                                           : (EncoderController.sourcePath.length > 0
                                              ? window.baseName(EncoderController.sourcePath)
                                              : qsTr("no source"))
    title: qsTr("ac3forge — %1").arg(sourceLabel)

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

    // ---- Basic / Advanced and the tab bar ----------------------------------
    // Defaults to Basic per the handoff. Which FIELDS a tab shows in each
    // mode (checkpoint 6) is not implemented yet; this is the shell-level
    // half - which TABS exist at all - that the handoff specifies as part of
    // the tab bar itself.
    property bool advanced: false
    property string currentTab: "format"
    readonly property var tabOrder: ["format", "coding", "meta", "objects"]
    readonly property var visibleTabs: {
        const tabs = [{ key: "format", label: qsTr("Format") }];
        if (advanced) {
            tabs.push({ key: "coding", label: qsTr("Coding tools") });
            tabs.push({ key: "meta", label: qsTr("Metadata") });
        }
        tabs.push({ key: "objects", label: qsTr("Objects") });
        return tabs;
    }
    onAdvancedChanged: {
        // Switching to Basic while on a tab it hides falls back to Format
        // rather than showing an empty panel.
        if (!advanced && (currentTab === "coding" || currentTab === "meta")) {
            currentTab = "format";
        }
    }

    // ---- the plan headline and a best-effort CLI line ----------------------
    // Both are read from properties carrying NOTIFY planChanged (codecIndex,
    // layoutIndex, bitrateKbps), so they stay live even though outputSuffix()
    // itself is a plain invokable with no notify signal of its own.
    readonly property string planLine: {
        const codec = EncoderController.codecNames[EncoderController.codecIndex] || "";
        const shape = EncoderController.layoutNames[EncoderController.layoutIndex] || "";
        return qsTr("%1 · %2 · %3 kbps · .%4")
            .arg(codec).arg(shape).arg(EncoderController.bitrateKbps)
            .arg(EncoderController.outputSuffix());
    }
    // Placeholder vocabulary: gains --bed/--extras once the channel model
    // checkpoint replaces --layout, and --paths once objects gain authored
    // paths. Kept here rather than in C++ so it can be reshaped as those
    // land without touching the controller's own settings surface.
    readonly property string cliLine: {
        const parts = ["ac3cli",
                       "--codec", EncoderController.codecIndex === 0 ? "ac3" : "eac3",
                       "--layout", EncoderController.layoutNames[EncoderController.layoutIndex] || "",
                       "--bitrate", String(EncoderController.bitrateKbps)];
        if (EncoderController.toolsToken.length > 0) {
            parts.push("--tools", EncoderController.toolsToken);
        }
        if (EncoderController.atmosEnabled) {
            parts.push("--objects");
        }
        parts.push(EncoderController.sourcePath.length > 0
                   ? window.baseName(EncoderController.sourcePath) : "<source>");
        parts.push("out." + EncoderController.outputSuffix());
        return parts.join(" ");
    }

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

    // Hidden text surface for the command bar's Copy button - the standard
    // QML idiom for clipboard access without an extra module.
    TextEdit {
        id: clipboardProxy
        visible: false
        function copyText(text) {
            clipboardProxy.text = text;
            clipboardProxy.selectAll();
            clipboardProxy.copy();
        }
    }

    Dialog {
        id: preferencesDialog
        title: qsTr("Preferences")
        modal: true
        anchors.centerIn: parent
        standardButtons: Dialog.Close

        Label {
            text: qsTr("Appearance, capture and default-run settings land with a later checkpoint.")
            wrapMode: Text.WordWrap
            color: Theme.text
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ==== header =========================================================
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 14
            Layout.bottomMargin: 14
            spacing: 20

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Text {
                    text: qsTr("ac3forge")
                    color: Theme.text
                    font.pixelSize: Theme.fontTitle
                    font.bold: true
                    font.letterSpacing: -0.2
                }
                Text {
                    text: qsTr("Clean-room AC-3 / E-AC-3 encoder — ATSC A/52, ETSI TS 103 420")
                    color: Theme.textMuted
                    font.pixelSize: Theme.fontSmall
                }
            }

            RowLayout {
                spacing: 8

                Text {
                    text: qsTr("CONTROLS")
                    font.pixelSize: 11
                    font.letterSpacing: 1
                    color: Theme.textMuted
                }
                SegmentedControl {
                    model: [{ value: "basic", label: qsTr("Basic") },
                            { value: "advanced", label: qsTr("Advanced") }]
                    currentValue: window.advanced ? "advanced" : "basic"
                    onSelected: (value) => window.advanced = value === "advanced"
                }
                Button {
                    text: qsTr("Preferences")
                    onClicked: preferencesDialog.open()
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 2; color: Theme.divider }

        // ==== body: the two panes ===========================================
        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // ---- left rail: the signal, always visible ---------------------
            ScrollView {
                Layout.preferredWidth: 404
                Layout.minimumWidth: 340
                Layout.fillHeight: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: parent.width
                    spacing: Theme.gap

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

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: EncoderController.hasLevels
                            spacing: Theme.gap

                            ColumnLayout {
                                Layout.fillWidth: true
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

                            // Below the meters rather than beside them: at the
                            // rail's 340-404px width there is no longer room for
                            // both side by side without crushing the meter track
                            // down to a few pixels. The two-ring (ear/ceiling)
                            // soundfield redesign is its own checkpoint; this is
                            // just the arrangement fix the narrower rail forces.
                            SoundfieldView {
                                Layout.alignment: Qt.AlignHCenter
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
                }
            }

            Rectangle { Layout.preferredWidth: 2; Layout.fillHeight: true; color: Theme.divider }

            // ---- right panel: the stream ------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // ---- plan strip -----------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Layout.rightMargin: 24
                    Layout.topMargin: 18
                    Layout.bottomMargin: 16
                    spacing: 24

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Text {
                            text: qsTr("THE STREAM")
                            font.pixelSize: 10
                            font.letterSpacing: 1.2
                            color: Theme.textMuted
                        }
                        Text {
                            text: window.planLine
                            font.pixelSize: 26
                            font.bold: true
                            font.letterSpacing: -0.2
                            color: Theme.text
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: EncoderController.layoutDetail
                            font.family: "monospace"
                            font.pixelSize: 12
                            color: Theme.textMuted
                        }
                    }

                    ColumnLayout {
                        spacing: 6

                        Text {
                            text: qsTr("TOOLS")
                            font.pixelSize: 10
                            font.letterSpacing: 1.2
                            color: Theme.textMuted
                        }
                        Rectangle {
                            color: Theme.neutral200
                            implicitWidth: toolsText.implicitWidth + 20
                            implicitHeight: toolsText.implicitHeight + 10

                            Text {
                                id: toolsText
                                anchors.centerIn: parent
                                text: EncoderController.toolsToken.length > 0
                                      ? EncoderController.toolsToken : qsTr("none")
                                font.family: "monospace"
                                font.pixelSize: 13
                                color: Theme.text
                            }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 2; color: Theme.divider }

                // ---- tab bar ----------------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Layout.rightMargin: 24
                    spacing: 28

                    Repeater {
                        model: window.visibleTabs

                        delegate: Item {
                            id: tabDelegate
                            required property var modelData
                            implicitWidth: tabLabel.implicitWidth
                            implicitHeight: 13 + tabLabel.implicitHeight + 13 + 3

                            Text {
                                id: tabLabel
                                anchors.top: parent.top
                                anchors.topMargin: 13
                                text: modelData.label
                                font.pixelSize: 13
                                font.bold: true
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 1
                                color: Theme.text
                                opacity: window.currentTab === modelData.key ? 1.0 : 0.55
                            }
                            Rectangle {
                                anchors.top: tabLabel.bottom
                                anchors.topMargin: 13
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 3
                                color: window.currentTab === modelData.key ? Theme.accent : "transparent"
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: window.currentTab = tabDelegate.modelData.key
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
                Rectangle { Layout.fillWidth: true; height: 2; color: Theme.divider }

                // ---- tab content --------------------------------------------------
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth

                    StackLayout {
                        width: parent.width
                        currentIndex: window.tabOrder.indexOf(window.currentTab)

                        // ---- Format ---------------------------------------------
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

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

                                // layoutNames() ends with a synthetic "Custom…" entry - not a
                                // LayoutId, just the signal to read/write this field instead.
                                // This is the general allocator's minimal stopgap control; the
                                // two-tier bed+extras+LFE picker replaces it.
                                TextField {
                                    Layout.fillWidth: true
                                    visible: EncoderController.customLayoutSelected
                                    enabled: !EncoderController.busy && !EncoderController.atmosEnabled
                                    placeholderText: qsTr("L,C,R,LFE,Vhl,Vhr — comma-separated Table E2.5 locations")
                                    text: EncoderController.customChannels
                                    onEditingFinished: EncoderController.customChannels = text
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

                            Item { Layout.fillHeight: true }
                        }

                        // ---- Coding tools (Advanced only) ------------------------
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

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

                            Item { Layout.fillHeight: true }
                        }

                        // ---- Metadata (Advanced only) -----------------------------
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

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

                            Item { Layout.fillHeight: true }
                        }

                        // ---- Objects ---------------------------------------------
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

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

                            Item { Layout.fillHeight: true }
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 2; color: Theme.divider }

                // ---- runs --------------------------------------------------------
                // A history of past runs (README §6 Q9) needs run-tracking state the
                // controller does not carry yet; this shows only the current run
                // until that lands with the runs/feedback checkpoint.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    spacing: 0

                    Text {
                        Layout.preferredWidth: 90
                        Layout.leftMargin: 16
                        text: qsTr("RUNS")
                        font.pixelSize: 10
                        font.letterSpacing: 1
                        color: Theme.textMuted
                    }
                    Rectangle { Layout.preferredWidth: 1; Layout.fillHeight: true; color: Theme.divider }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 16
                        Layout.rightMargin: 16
                        spacing: 10

                        Rectangle {
                            width: 8
                            height: 8
                            visible: EncoderController.busy
                            color: Theme.accent
                        }
                        Text {
                            visible: EncoderController.busy
                            text: qsTr("encoding · %1%").arg(Math.round(EncoderController.progress * 100))
                            font.family: "monospace"
                            font.pixelSize: 12
                            color: Theme.text
                        }
                        ProgressBar {
                            visible: EncoderController.busy
                            Layout.preferredWidth: 90
                            Layout.preferredHeight: 5
                            from: 0
                            to: 1
                            value: EncoderController.progress
                        }
                        Button {
                            visible: EncoderController.busy
                            text: qsTr("Cancel")
                            flat: true
                            onClicked: EncoderController.cancel()
                        }
                        Text {
                            visible: !EncoderController.busy
                            Layout.fillWidth: true
                            text: EncoderController.status
                            font.family: "monospace"
                            font.pixelSize: 12
                            color: Theme.textMuted
                            elide: Text.ElideRight
                        }
                        Item { Layout.fillWidth: true; visible: EncoderController.busy }
                    }
                }
                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

                // ---- command bar ---------------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 20
                    Layout.rightMargin: 20
                    Layout.topMargin: 12
                    Layout.bottomMargin: 12
                    spacing: 16

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 38
                        color: Theme.neutral100

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 2
                            color: Theme.text
                        }

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 10

                            Text {
                                Layout.fillWidth: true
                                text: window.cliLine
                                font.family: "monospace"
                                font.pixelSize: 12
                                color: Theme.text
                                elide: Text.ElideRight
                            }
                            Button {
                                text: qsTr("Copy")
                                flat: true
                                onClicked: clipboardProxy.copyText(window.cliLine)
                            }
                        }
                    }

                    Button {
                        text: EncoderController.busy
                              ? qsTr("Encoding…")
                              : qsTr("Encode to .%1").arg(EncoderController.outputSuffix())
                        enabled: EncoderController.sourceReady && !EncoderController.busy
                        highlighted: true
                        implicitHeight: 44
                        implicitWidth: Math.max(190, contentItem.implicitWidth + 40)
                        onClicked: window.openSaveDialog(
                                       saveDialog,
                                       EncoderController.suggestedOutputName())
                    }
                }
            }
        }
    }
}
