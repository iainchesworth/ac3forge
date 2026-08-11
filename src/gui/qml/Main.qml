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

    // ---- Guided / Advanced / Expert and the tab bar ------------------------
    // "guided" | "advanced" | "expert". Defaults to Guided - a step-by-step
    // sequence for a new user, over GuidedWizard.qml (appended as the last
    // StackLayout page below, shown instead of the tab bar + the other
    // pages, not a fourth tab alongside them). Advanced is what "Basic" used
    // to mean here - full channel control on one page, no coding-tools/
    // metadata clutter; Expert is what "Advanced" used to mean - the same
    // page plus Coding tools, Metadata and Passthrough. Kept as plain QML
    // state rather than a controller enum: nothing outside this file reads
    // it (Preferences, which would want to persist a default, is still an
    // unbuilt placeholder - see preferencesDialog below), so there is
    // nothing yet for a C++-side property to usefully gate.
    property string tier: "guided"
    property string currentTab: "format"
    // "coded" | "rendered" - persisted preference, the fourteen-rows-for-
    // twelve-speakers question turned into a mode rather than a puzzle.
    // Defaults to "coded" so the meters keep showing every transmitted
    // channel unless asked otherwise - what the app has always done.
    property string meterMode: "coded"

    // ---- the panel banner: one of the three feedback homes -----------------
    // Field-level messages sit next to the control they concern (layoutDetail,
    // routingSummary, the object bit-rate warning...) and the run strip covers
    // anything about a specific run; this is the third - a banner at the top
    // of the panel that caused the problem. -1 means "show whichever run most
    // recently failed"; a run's own Details button points the banner at it
    // explicitly, and Dismiss remembers that id so it does not reappear.
    property int bannerRunId: -1
    // The run whose banner Dismiss was clicked on, so dismissing run 12's
    // failure does not also suppress the banner the NEXT failure deserves.
    property int dismissedRunId: -1
    readonly property var bannerRun: {
        const runs = EncoderController.runs;
        let candidate = null;
        if (bannerRunId >= 0) {
            for (const run of runs) {
                if (run.id === bannerRunId) { candidate = run; break; }
            }
        } else {
            for (const run of runs) {
                if (run.status === "failed") { candidate = run; break; }
            }
        }
        return candidate && candidate.id !== dismissedRunId ? candidate : null;
    }
    readonly property var tabOrder: ["format", "coding", "meta", "objects", "session"]
    readonly property var visibleTabs: {
        const tabs = [{ key: "format", label: qsTr("Format") }];
        if (tier === "expert") {
            tabs.push({ key: "coding", label: qsTr("Coding tools") });
            tabs.push({ key: "meta", label: qsTr("Metadata") });
        }
        tabs.push({ key: "objects", label: qsTr("Objects") });
        // Only exists to be jumped into, the way the handoff's own prototype
        // only shows it once something is actually running - a tab for a
        // session that is not there yet has nothing to show.
        if (EncoderController.liveActive) {
            tabs.push({ key: "session", label: qsTr("Live session") });
        }
        return tabs;
    }
    onTierChanged: {
        // Leaving Expert while on a tab only it shows falls back to Format
        // rather than showing an empty panel - covers both Guided (which
        // shows no tab bar at all) and Advanced.
        if (tier !== "expert" && (currentTab === "coding" || currentTab === "meta")) {
            currentTab = "format";
        }
    }

    Connections {
        target: EncoderController
        function onLiveActiveChanged() {
            if (EncoderController.liveActive) {
                currentTab = "session";
            } else if (currentTab === "session") {
                currentTab = "format";
            }
        }
    }

    // ---- the plan headline and a best-effort CLI line ----------------------
    // Both are read from properties carrying NOTIFY planChanged (codecIndex,
    // channelShapeName, bitrateKbps), so they stay live even though
    // outputSuffix() itself is a plain invokable with no notify signal of its
    // own.
    readonly property string planLine: {
        const codec = EncoderController.codecNames[EncoderController.codecIndex] || "";
        const shape = EncoderController.atmosEnabled
                      ? qsTr("5.1") : EncoderController.channelShapeName;
        const rate = EncoderController.vbrAvailable && EncoderController.vbrEnabled
                     ? qsTr("quality %1").arg(EncoderController.vbrQuality)
                     : qsTr("%1 kbps").arg(EncoderController.bitrateKbps);
        return qsTr("%1 · %2 · %3 · .%4")
            .arg(codec).arg(shape).arg(rate)
            .arg(EncoderController.outputSuffix());
    }
    // ac3cli's actual [layout] argument takes either a preset name or this
    // exact comma-separated Table E2.5 list (plan::parse_channels), so this
    // is real, pasteable syntax rather than an aspirational one - the
    // handoff's own "--bed/--extras" sketch does not match ac3cli's actual
    // (positional) subcommand grammar, which also differs in shape between
    // 'encode' (AC-3, no tools argument), 'eac3-encode' (E-AC-3) and
    // 'atmos-encode'. Placeholder vocabulary otherwise: gains --paths once
    // objects gain authored paths, and does not yet account for the Matroska
    // container, which ac3cli muxes as a second 'mkv' invocation rather than
    // an encode-command argument.
    readonly property string cliLine: {
        const source = EncoderController.sourcePath.length > 0
                       ? window.baseName(EncoderController.sourcePath) : "<source>";
        const out = "out." + EncoderController.outputSuffix();
        const rate = String(EncoderController.bitrateKbps);
        if (EncoderController.atmosEnabled) {
            return ["ac3cli", "atmos-encode", source, out, rate].join(" ");
        }
        if (EncoderController.codecIndex === 0) {
            return ["ac3cli", "encode", source, out, rate,
                    EncoderController.channelLocationsText].join(" ");
        }
        const parts = ["ac3cli", "eac3-encode", source, out, rate,
                       EncoderController.toolsToken.length > 0
                           ? EncoderController.toolsToken : "none",
                       EncoderController.channelLocationsText];
        // [vbr] is the next positional after [layout] - only appended when
        // actually on, so the common CBR case stays exactly what it always
        // was rather than growing a trailing "off" nobody typed.
        if (EncoderController.vbrAvailable && EncoderController.vbrEnabled) {
            parts.push(EncoderController.vbrToken);
        }
        return parts.join(" ");
    }

    FileDialog {
        id: openDialog
        title: qsTr("Choose a WAV file")
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        onAccepted: EncoderController.loadSourceFile(selectedFile)
    }

    FileDialog {
        id: addSourceDialog
        title: qsTr("Add another source")
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        onAccepted: EncoderController.addSourceFile(selectedFile)
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

    FileDialog {
        id: liveSessionDialog
        title: qsTr("Save the live take")
        fileMode: FileDialog.SaveFile
        onAccepted: EncoderController.startLiveSession(
                        deviceBox.currentIndex, liveMonitorCheck.checked,
                        liveReceiverBox.currentIndex - 1, true, selectedFile)
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
                    model: [{ value: "guided", label: qsTr("Guided") },
                            { value: "advanced", label: qsTr("Advanced") },
                            { value: "expert", label: qsTr("Expert") }]
                    currentValue: window.tier
                    onSelected: (value) => window.tier = value
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
                                objectName: "chooseWavButton"
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

                            Button {
                                objectName: "addSourceButton"
                                text: qsTr("Add source…")
                                enabled: EncoderController.sourceReady && !EncoderController.busy
                                onClicked: addSourceDialog.open()
                            }
                        }

                        // ---- multi-source list + assignment -----------------------
                        // Functional first cut, not the handoff's full Assign table
                        // (that needs the Guided/Advanced/Expert tiers it is meant to
                        // live behind - see docs/design/handoff-workbench). Only
                        // appears once a second source exists; a single loaded file
                        // keeps using the plain path/info line above, unchanged.
                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            spacing: 4
                            visible: EncoderController.sourceModel.length > 1

                            Repeater {
                                model: EncoderController.sourceModel
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: Theme.gap

                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("%1 · %2 ch").arg(modelData.label)
                                                                .arg(modelData.channels)
                                        color: Theme.text
                                        font.pixelSize: Theme.fontSmall
                                        elide: Text.ElideMiddle
                                    }
                                    Button {
                                        text: qsTr("Remove")
                                        flat: true
                                        onClicked: EncoderController.removeSource(modelData.index)
                                    }
                                }
                            }

                            Text {
                                text: qsTr("ASSIGN EACH CHANNEL")
                                font.pixelSize: 10
                                font.letterSpacing: 1
                                color: Theme.textMuted
                            }
                            Repeater {
                                model: EncoderController.assignmentRows
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: Theme.gap

                                    Text {
                                        Layout.preferredWidth: 160
                                        text: qsTr("%1 ch %2").arg(modelData.sourceLabel)
                                                              .arg(modelData.channel + 1)
                                        color: Theme.text
                                        font.pixelSize: Theme.fontSmall
                                        elide: Text.ElideMiddle
                                    }
                                    // A location name (e.g. "L", "Ls"), "obj", "p1",
                                    // "p2" or "none" - EncoderController.
                                    // setAssignment's own vocabulary
                                    // (plan::parse_destination), typed directly
                                    // rather than picked from a dropdown until the
                                    // Assign table proper exists.
                                    TextField {
                                        Layout.preferredWidth: 80
                                        text: modelData.destToken
                                        font.family: "monospace"
                                        font.pixelSize: Theme.fontSmall
                                        onEditingFinished: EncoderController.setAssignment(
                                                               modelData.source, modelData.channel, text)
                                    }
                                }
                            }
                            Repeater {
                                model: EncoderController.unassignedWarnings
                                delegate: Text {
                                    required property string modelData
                                    Layout.fillWidth: true
                                    text: modelData
                                    color: Theme.accent
                                    font.pixelSize: Theme.fontSmall
                                    wrapMode: Text.WordWrap
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

                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider
                                   visible: EncoderController.captureSupported
                                            && !EncoderController.liveActive }

                        // A live session keeps capturing and encoding after
                        // Record… would have finished writing a fixed take:
                        // every frame optionally goes to a monitor speaker
                        // and a bitstreamed receiver as it is produced, not
                        // just to a file at the end.
                        RowLayout {
                            Layout.fillWidth: true
                            visible: EncoderController.captureSupported
                                     && !EncoderController.liveActive
                            spacing: Theme.gap

                            CheckBox {
                                id: liveMonitorCheck
                                text: qsTr("Monitor")
                                checked: true
                                enabled: !EncoderController.busy
                            }

                            ComboBox {
                                id: liveReceiverBox
                                Layout.fillWidth: true
                                enabled: !EncoderController.busy
                                model: [qsTr("No passthrough")].concat(EncoderController.outputDevices)
                            }

                            CheckBox {
                                id: liveWriteCheck
                                text: qsTr("Also write to disk")
                                enabled: !EncoderController.busy
                            }

                            Button {
                                text: qsTr("Start live session…")
                                enabled: EncoderController.captureSupported && !EncoderController.busy
                                onClicked: {
                                    if (liveWriteCheck.checked) {
                                        window.openSaveDialog(liveSessionDialog,
                                                              "live." + EncoderController.outputSuffix());
                                    } else {
                                        EncoderController.startLiveSession(
                                            deviceBox.currentIndex, liveMonitorCheck.checked,
                                            liveReceiverBox.currentIndex - 1, false,
                                            "");
                                    }
                                }
                            }
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
                        id: levelsCard
                        title: qsTr("Channel levels")

                        // Which coded-channel indices this mode shows: Coded
                        // shows every transmitted channel; Rendered hides a bed
                        // channel a dependent substream replaces (level.replaced),
                        // since it carries the same audio as the one that stays.
                        readonly property var visibleMeterIndices: {
                            const indices = [];
                            const levels = EncoderController.channelLevels;
                            const names = EncoderController.channelNames;
                            for (let i = 0; i < names.length; i++) {
                                const level = i < levels.length ? levels[i] : ({});
                                if (window.meterMode === "rendered" && level.replaced === true) {
                                    continue;
                                }
                                indices.push(i);
                            }
                            return indices;
                        }
                        readonly property int meterFedCount: {
                            let count = 0;
                            const levels = EncoderController.channelLevels;
                            for (const i of visibleMeterIndices) {
                                const level = i < levels.length ? levels[i] : ({});
                                if (level.fed !== false) {
                                    count++;
                                }
                            }
                            return count;
                        }

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

                            SegmentedControl {
                                visible: EncoderController.hasLevels
                                model: [{ value: "coded", label: qsTr("Coded") },
                                        { value: "rendered", label: qsTr("Rendered") }]
                                currentValue: window.meterMode
                                segHeight: 22
                                fontSize: 11
                                onSelected: (value) => window.meterMode = value
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: EncoderController.hasLevels
                            spacing: Theme.gap

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 3

                                Repeater {
                                    objectName: "channelMeters"
                                    model: levelsCard.visibleMeterIndices

                                    delegate: Item {
                                        required property int modelData

                                        readonly property var level:
                                            modelData < EncoderController.channelLevels.length
                                            ? EncoderController.channelLevels[modelData] : ({})
                                        // Coded mode groups a bed channel a dependent
                                        // replaces behind a left rule so the
                                        // duplication reads as structure; Rendered
                                        // mode never sees these rows at all.
                                        readonly property bool grouped:
                                            window.meterMode === "coded" && level.replaced === true

                                        Layout.fillWidth: true
                                        implicitHeight: meter.implicitHeight

                                        Rectangle {
                                            visible: grouped
                                            anchors.left: parent.left
                                            anchors.top: parent.top
                                            anchors.bottom: parent.bottom
                                            width: 2
                                            color: Theme.accent300
                                        }

                                        ChannelMeter {
                                            id: meter
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.leftMargin: grouped ? 8 : 0
                                            channelName: modelData < EncoderController.channelNames.length
                                                         ? EncoderController.channelNames[modelData] : ""
                                            level: parent.level
                                        }
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

                                // Half the answer to "how do routing consequences
                                // show before the fact" - the other half is the
                                // channel map in Format. Reads as a plain fact
                                // when everything is fed, and as a warning (accent
                                // top rule) when the source is narrower than the
                                // plan.
                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 6
                                    height: levelsCard.meterFedCount < levelsCard.visibleMeterIndices.length ? 2 : 1
                                    color: levelsCard.meterFedCount < levelsCard.visibleMeterIndices.length
                                           ? Theme.accent : Theme.divider
                                }
                                Text {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 4
                                    readonly property int total: levelsCard.visibleMeterIndices.length
                                    readonly property int fed: levelsCard.meterFedCount
                                    readonly property string noun: window.meterMode === "coded"
                                                                    ? qsTr("coded channels") : qsTr("channels")
                                    text: fed === total
                                          ? qsTr("All %1 %2 fed").arg(total).arg(noun)
                                          : qsTr("%1 of %2 %3 fed").arg(fed).arg(total).arg(noun)
                                    color: fed === total ? Theme.textMuted : Theme.accent700
                                    font.pixelSize: Theme.fontSmall
                                }
                            }

                            // Below the meters rather than beside them: at the
                            // rail's 340-404px width there is no longer room for
                            // both side by side without crushing the meter track
                            // down to a few pixels.
                            SoundfieldView {
                                Layout.alignment: Qt.AlignHCenter
                                visible: EncoderController.surround
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: EncoderController.hasLevels && EncoderController.dualMono
                                text: qsTr("No room to draw: dual mono has no soundstage.")
                                color: Theme.textMuted
                                font.pixelSize: Theme.fontSmall
                                horizontalAlignment: Text.AlignHCenter
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

                // ---- the panel banner: a failed run, named and explained -------
                Rectangle {
                    Layout.fillWidth: true
                    visible: window.bannerRun !== null
                    color: Theme.accent100
                    implicitHeight: bannerContent.implicitHeight + 28

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        color: Theme.accent
                    }

                    RowLayout {
                        id: bannerContent
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.margins: 14
                        spacing: 12

                        Text {
                            text: "⚠"
                            color: Theme.accent700
                            font.pixelSize: 16
                            Layout.alignment: Qt.AlignTop
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Text {
                                text: window.bannerRun
                                      ? qsTr("Run %1 stopped — %2")
                                        .arg(window.bannerRun.id).arg(window.bannerRun.filename)
                                      : ""
                                color: Theme.accent800
                                font.bold: true
                                font.pixelSize: 14
                            }
                            Text {
                                Layout.fillWidth: true
                                text: window.bannerRun ? window.bannerRun.detail : ""
                                color: Theme.accent900
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                        }
                        Button {
                            text: qsTr("Dismiss")
                            Layout.alignment: Qt.AlignTop
                            onClicked: {
                                window.dismissedRunId = window.bannerRun.id;
                                window.bannerRunId = -1;
                            }
                        }
                    }
                }

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
                // Guided has no tabs at all - GuidedWizard.qml, appended as the last
                // StackLayout page below, takes the whole content area instead. The
                // divider right after this stays up regardless, as the wizard's own
                // top border.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Layout.rightMargin: 24
                    visible: window.tier !== "guided"
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
                        // GuidedWizard is appended as one more page after every entry
                        // tabOrder names, so its index is always tabOrder.length -
                        // nothing above needs to know it exists to compute this.
                        currentIndex: window.tier === "guided"
                                      ? window.tabOrder.length
                                      : window.tabOrder.indexOf(window.currentTab)

                        // ---- Format ---------------------------------------------
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            Card {
                                title: qsTr("Format")

                                // Presets are starting points, not the model: they set
                                // bed + LFE + extras together, but the picker below is
                                // what the plan actually reads.
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.gap

                                    Repeater {
                                        model: ["5.1", "7.1", "5.1.4", "7.1.4", "5.2"]
                                        delegate: Button {
                                            required property string modelData
                                            text: modelData
                                            enabled: !EncoderController.busy
                                                     && !EncoderController.atmosEnabled
                                            onClicked: EncoderController.applyChannelPreset(modelData)
                                        }
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 6
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

                                // Not object mode, not a live session (see vbrAvailable()'s own
                                // comment on why) - the panel itself disappears rather than
                                // showing a control that would silently do nothing. Shared with
                                // the Guided wizard's own Rate mode step - see VbrPanel.qml.
                                VbrPanel {
                                    Layout.fillWidth: true
                                    Layout.topMargin: Theme.gap
                                }

                                Rectangle { Layout.fillWidth: true; height: 2; color: Theme.divider }

                                // ---- the channel model: bed + LFE + extras ------------------
                                Text {
                                    text: qsTr("CHANNELS")
                                    font.pixelSize: 11
                                    font.letterSpacing: 1
                                    color: Theme.textMuted
                                }

                                // Tier 1 - the bed, exactly one, always, plus its
                                // independent LFE. All seven stay live under AC-3;
                                // only the extras below are Dolby Digital Plus only.
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6

                                    // 1+1 is always bedChoices[0] - drawn apart from
                                    // the seven location-mask beds with a rule
                                    // rather than sharing their row, so it reads as
                                    // categorically different (a bed, not a
                                    // soundstage) without fighting Fusion's own
                                    // button chrome for a literal dashed border.
                                    Button {
                                        objectName: "bedDualMonoButton"
                                        text: EncoderController.bedChoices[0].id
                                        highlighted: EncoderController.bedIndex === 0
                                        enabled: !EncoderController.busy
                                                 && !EncoderController.atmosEnabled
                                        onClicked: EncoderController.bedIndex = 0

                                        ToolTip.visible: hovered
                                        ToolTip.text: qsTr("Dual mono — two independent programmes, not a stereo pair")
                                    }

                                    Rectangle {
                                        Layout.preferredWidth: 1
                                        Layout.fillHeight: true
                                        color: Theme.divider
                                    }

                                    Repeater {
                                        // bedChoices[0] is the dual-mono button above;
                                        // this repeater is every other bed, offset by
                                        // one so its own bedIndex stays correct.
                                        model: EncoderController.bedChoices.length - 1

                                        delegate: Button {
                                            required property int index
                                            readonly property var choice: EncoderController.bedChoices[index + 1]
                                            text: choice.id
                                            highlighted: EncoderController.bedIndex === index + 1
                                            enabled: !EncoderController.busy
                                                     && !EncoderController.atmosEnabled
                                            onClicked: EncoderController.bedIndex = index + 1

                                            ToolTip.visible: hovered
                                            ToolTip.text: choice.channels
                                        }
                                    }

                                    CheckBox {
                                        text: qsTr("LFE")
                                        checked: EncoderController.bedLfe
                                        enabled: !EncoderController.busy
                                                 && !EncoderController.bedLfeLocked
                                        onToggled: EncoderController.bedLfe = checked
                                    }

                                    Item { Layout.fillWidth: true }

                                    Text {
                                        text: EncoderController.channelShapeName
                                        font.pixelSize: Theme.fontNormal
                                        font.bold: true
                                        color: Theme.text
                                    }
                                }

                                // Tier 2 - extras, added to the bed. Each is a single
                                // toggle; a disabled row's tooltip says why (locked,
                                // over budget, or - the one real cross-extra rule -
                                // an LFE2 about to be left with no full-bandwidth
                                // companion once its last partner is unticked).
                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 3
                                    columnSpacing: Theme.gap
                                    rowSpacing: 4

                                    Repeater {
                                        model: EncoderController.extrasModel

                                        delegate: CheckBox {
                                            required property var modelData
                                            text: qsTr("%1 (%2 ch)")
                                                  .arg(modelData.label).arg(modelData.channels)
                                            checked: modelData.checked
                                            enabled: modelData.enabled
                                            opacity: enabled ? 1.0 : 0.3
                                            onToggled: EncoderController.toggleExtra(modelData.id)

                                            ToolTip.visible: modelData.reason.length > 0 && hovered
                                            ToolTip.text: modelData.reason
                                        }
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("%1 of %2 channel positions used")
                                          .arg(EncoderController.channelBudgetUsed)
                                          .arg(EncoderController.channelBudgetMax)
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSmall
                                    font.family: "monospace"
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

                            // ---- loudness: Advanced only ---------------------------------
                            // Expert moves this onto Metadata instead, alongside Downmix, so
                            // it is never shown twice - same LoudnessGroup component either
                            // way. Guided has its own copy on the wizard's Loudness step
                            // (GuidedWizard.qml) rather than sharing this Format-tab card,
                            // since Guided shows no tab bar for this card to live under.
                            Card {
                                title: qsTr("Loudness")
                                visible: window.tier === "advanced"

                                LoudnessGroup {}

                                Text {
                                    Layout.topMargin: 2
                                    text: qsTr("Coding tools and broadcast metadata →")
                                    color: Theme.accent
                                    font.pixelSize: Theme.fontSmall

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: window.tier = "expert"
                                    }
                                }
                            }

                            // ---- passthrough: Expert only --------------------------------
                            // Not in Advanced's own list of what it shows (source, codec,
                            // channel picker, bit rate, output path and container, Loudness,
                            // Objects) - a receiver endpoint is a codec-developer concern,
                            // not a mix-encoding one.
                            Card {
                                title: qsTr("Passthrough to a receiver")
                                visible: window.tier === "expert"

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
                        // Two columns: Loudness + Downmix on the left, Heavy
                        // compression + Mixing metadata on the right. Loudness
                        // lives here rather than on Format because Advanced is
                        // active - LoudnessGroup itself is the same component
                        // Format uses in Basic, never both at once.
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            spacing: 40

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                spacing: Theme.gap

                                Card {
                                    title: qsTr("Loudness")
                                    LoudnessGroup {}
                                }

                                Card {
                                    title: qsTr("Downmix")

                                    GridLayout {
                                        Layout.fillWidth: true
                                        columns: 2
                                        columnSpacing: Theme.gap
                                        rowSpacing: Theme.gap

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
                                }

                                Item { Layout.fillHeight: true }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                spacing: Theme.gap

                                Card {
                                    title: qsTr("Heavy compression")

                                    CheckBox {
                                        text: qsTr("Heavy compression")
                                        enabled: !EncoderController.busy
                                        checked: EncoderController.heavy
                                        onToggled: EncoderController.heavy = checked
                                    }

                                    // The indent rule replaces injecting controls
                                    // into the middle of the column: a sub-group
                                    // reads as one whether it is showing or not.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: EncoderController.heavy
                                        spacing: Theme.gap

                                        Rectangle { Layout.preferredWidth: 2; Layout.fillHeight: true; color: Theme.accent200 }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 8
                                            spacing: Theme.gap

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: Theme.gap

                                                Text {
                                                    text: qsTr("ceiling")
                                                    color: Theme.textMuted
                                                    font.pixelSize: Theme.fontSmall
                                                }
                                                // Counted in tenths of a decibel: the default
                                                // ceiling is -0.5 dBFS, and a whole-number box
                                                // would show it as 0 and write that back —
                                                // throwing away exactly the headroom §7.7.2
                                                // exists to reserve.
                                                SpinBox {
                                                    from: -200
                                                    to: 0
                                                    stepSize: 5
                                                    enabled: !EncoderController.busy
                                                    value: Math.round(EncoderController.ceilingDb * 10)
                                                    textFromValue: (value) => (value / 10).toFixed(1) + " dBFS"
                                                    valueFromText: (text) => Math.round(parseFloat(text) * 10)
                                                    onValueModified: EncoderController.ceilingDb = value / 10
                                                }

                                                Text {
                                                    text: qsTr("dialogue at")
                                                    color: Theme.textMuted
                                                    font.pixelSize: Theme.fontSmall
                                                }
                                                SpinBox {
                                                    from: -40
                                                    to: -5
                                                    enabled: !EncoderController.busy
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
                                            }
                                        }
                                    }
                                }

                                // ---- mixing metadata: E-AC-3 only ------------------------
                                Card {
                                    title: qsTr("Mixing metadata")
                                    visible: EncoderController.mixmetaAvailable

                                    CheckBox {
                                        text: qsTr("Mixing metadata")
                                        enabled: !EncoderController.busy
                                        checked: EncoderController.mixmeta
                                        onToggled: EncoderController.mixmeta = checked
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: EncoderController.mixmeta
                                        spacing: Theme.gap

                                        Rectangle { Layout.preferredWidth: 2; Layout.fillHeight: true; color: Theme.accent200 }

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.leftMargin: 8
                                            spacing: Theme.gap

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: Theme.gap

                                                Text {
                                                    text: qsTr("preferred downmix")
                                                    color: Theme.textMuted
                                                    font.pixelSize: Theme.fontSmall
                                                }
                                                ComboBox {
                                                    enabled: !EncoderController.busy
                                                    model: EncoderController.dmixNames
                                                    currentIndex: EncoderController.dmixIndex
                                                    onActivated: EncoderController.dmixIndex = currentIndex
                                                }

                                                Text {
                                                    text: qsTr("LFE mix")
                                                    color: Theme.textMuted
                                                    font.pixelSize: Theme.fontSmall
                                                }
                                                SpinBox {
                                                    from: -1
                                                    to: 31
                                                    enabled: !EncoderController.busy
                                                    value: EncoderController.lfeMix
                                                    // §E2.3.1.11: the level in dB is 10 - the
                                                    // code, so 0 is the +10 dB §7.8 calls ideal.
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
                                            }
                                        }
                                    }
                                }

                                Item { Layout.fillHeight: true }
                            }
                        }

                        // ---- Objects ---------------------------------------------
                        ColumnLayout {
                            id: objectsTab
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            // "author" edits object_configs_/keyframes directly via the
                            // sliders and room plan. "live" has nothing to drive yet -
                            // real live-driven motion is follow-up work for the capture
                            // branch - so it just points at Live session instead of
                            // offering controls that would silently do nothing.
                            property string driveMode: "author"
                            property real playheadTime: 0
                            property bool previewing: false
                            // objectKeyframes()/evaluateObjectPath() are Q_INVOKABLEs, not
                            // properties, so nothing marks a binding that calls them as
                            // depending on objectsChanged. Reading this counter inside
                            // those bindings gives them something to depend on.
                            property int objectsRevision: 0

                            readonly property var selectedObj: {
                                const list = EncoderController.objectModel;
                                for (let i = 0; i < list.length; ++i) {
                                    if (list[i].index === EncoderController.selectedObjectIndex) {
                                        return list[i];
                                    }
                                }
                                return null;
                            }

                            function formatTime(t) {
                                return "0:" + t.toFixed(2).padStart(5, "0");
                            }

                            Connections {
                                target: EncoderController
                                function onObjectsChanged() { objectsTab.objectsRevision++; }
                            }

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
                                    RowLayout {
                                        visible: EncoderController.atmosEnabled
                                                 && EncoderController.bitrateKbps < 384
                                        spacing: Theme.gap

                                        Text {
                                            text: qsTr("⚠ the bed is 5.1 — 384 kbps or more")
                                            color: Theme.bad
                                            font.pixelSize: Theme.fontSmall
                                        }

                                        Button {
                                            text: qsTr("Set it")
                                            enabled: !EncoderController.busy
                                            onClicked: EncoderController.bitrateKbps = 384
                                        }
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    visible: EncoderController.atmosEnabled
                                    text: qsTr("Every source channel becomes an object, panned into a 5.1 bed that any decoder can play. The object positions ride alongside as metadata — so a height is carried even though no bed channel can reproduce it, and the LFE send is the only route to that channel, since no direction points at it.")
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSmall
                                    wrapMode: Text.WordWrap
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: EncoderController.atmosEnabled
                                    spacing: Theme.space6

                                    // Plan view of the room: §4.2.1's x to the right, y
                                    // towards the back, listener in the middle.
                                    ColumnLayout {
                                        Layout.preferredWidth: 340
                                        Layout.alignment: Qt.AlignTop
                                        spacing: Theme.space2

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                text: qsTr("ROOM — PLAN")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: qsTr("drag to place")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                                font.family: "monospace"
                                            }
                                        }

                                        Rectangle {
                                            id: room
                                            Layout.preferredWidth: 340
                                            Layout.preferredHeight: 300
                                            color: Theme.neutral100
                                            border.color: Theme.divider
                                            border.width: 1

                                            Rectangle {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                anchors.top: parent.top
                                                anchors.bottom: parent.bottom
                                                width: 1
                                                color: Theme.neutral300
                                            }
                                            Rectangle {
                                                anchors.verticalCenter: parent.verticalCenter
                                                anchors.left: parent.left
                                                anchors.right: parent.right
                                                height: 1
                                                color: Theme.neutral300
                                            }
                                            Text {
                                                anchors.left: parent.left
                                                anchors.top: parent.top
                                                anchors.margins: 6
                                                text: qsTr("front")
                                                color: Theme.neutral500
                                                font.pixelSize: 9
                                            }
                                            Text {
                                                anchors.left: parent.left
                                                anchors.bottom: parent.bottom
                                                anchors.margins: 6
                                                text: qsTr("rear")
                                                color: Theme.neutral500
                                                font.pixelSize: 9
                                            }

                                            // Drag moves the SELECTED object; declared before
                                            // the markers so it sits underneath them and a
                                            // click precisely on a marker still reaches that
                                            // marker's own MouseArea instead.
                                            MouseArea {
                                                anchors.fill: parent
                                                enabled: !EncoderController.busy
                                                         && objectsTab.driveMode === "author"
                                                         && objectsTab.selectedObj !== null
                                                onPositionChanged: (mouse) => place(mouse)
                                                onPressed: (mouse) => place(mouse)
                                                function place(mouse) {
                                                    const x = Math.max(0, Math.min(1, mouse.x / room.width));
                                                    const y = Math.max(0, Math.min(1, mouse.y / room.height));
                                                    EncoderController.setObjectPosition(
                                                        objectsTab.selectedObj.index, x, y,
                                                        objectsTab.selectedObj.z);
                                                }
                                            }

                                            Repeater {
                                                model: EncoderController.objectModel

                                                Rectangle {
                                                    id: marker
                                                    required property var modelData
                                                    readonly property bool isSelected:
                                                        modelData.index === EncoderController.selectedObjectIndex
                                                    readonly property var livePos:
                                                        objectsTab.previewing
                                                        ? EncoderController.evaluateObjectPath(
                                                              modelData.index, objectsTab.playheadTime)
                                                        : null

                                                    width: isSelected ? 18 : 14
                                                    height: isSelected ? 18 : 14
                                                    color: isSelected ? Theme.accent : Theme.neutral800
                                                    border.color: Theme.text
                                                    border.width: isSelected ? 2 : 0
                                                    x: (livePos ? livePos.x : modelData.x) * room.width - width / 2
                                                    y: (livePos ? livePos.y : modelData.y) * room.height - height / 2
                                                    z: isSelected ? 1 : 0

                                                    Rectangle {
                                                        visible: marker.isSelected
                                                        anchors.left: parent.right
                                                        anchors.leftMargin: 4
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        width: chip.implicitWidth + 6
                                                        height: chip.implicitHeight + 2
                                                        color: Theme.bg

                                                        Text {
                                                            id: chip
                                                            anchors.centerIn: parent
                                                            text: qsTr("obj %1").arg(marker.modelData.index + 1)
                                                            color: Theme.text
                                                            font.pixelSize: 10
                                                            font.family: "monospace"
                                                        }
                                                    }

                                                    MouseArea {
                                                        anchors.fill: parent
                                                        onClicked: EncoderController.selectedObjectIndex
                                                                   = marker.modelData.index
                                                    }
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: Theme.space3

                                            Repeater {
                                                model: [
                                                    { label: "x", value: objectsTab.selectedObj ? objectsTab.selectedObj.x : 0 },
                                                    { label: "y", value: objectsTab.selectedObj ? objectsTab.selectedObj.y : 0 },
                                                    { label: "z", value: objectsTab.selectedObj ? objectsTab.selectedObj.z : 0 }
                                                ]

                                                ColumnLayout {
                                                    required property var modelData
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Text {
                                                        text: modelData.label
                                                        color: Theme.neutral600
                                                        font.pixelSize: 9
                                                        font.capitalization: Font.AllUppercase
                                                    }
                                                    Text {
                                                        text: modelData.value.toFixed(2)
                                                        color: Theme.text
                                                        font.pixelSize: 13
                                                        font.family: "monospace"
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignTop
                                        spacing: Theme.space2

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                text: qsTr("OBJECTS")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Item { Layout.fillWidth: true }
                                            SegmentedControl {
                                                model: [
                                                    { value: "author", label: qsTr("Author a path") },
                                                    { value: "live", label: qsTr("Drive it live") }
                                                ]
                                                currentValue: objectsTab.driveMode
                                                onSelected: (value) => objectsTab.driveMode = value
                                            }
                                        }

                                        Rectangle {
                                            Layout.fillWidth: true
                                            visible: objectsTab.driveMode === "live"
                                            color: Theme.accent100
                                            implicitHeight: liveMsg.implicitHeight + Theme.space3 * 2

                                            Text {
                                                id: liveMsg
                                                anchors.fill: parent
                                                anchors.margins: Theme.space3
                                                text: qsTr("Live driving needs a monitored capture. Open Live session to drag objects against running audio.")
                                                color: Theme.accent800
                                                font.pixelSize: Theme.fontSmall
                                                wrapMode: Text.WordWrap
                                            }
                                        }

                                        // Header row + one row per object, matching the
                                        // room plan's markers and the sliders below.
                                        GridLayout {
                                            Layout.fillWidth: true
                                            columns: 8
                                            columnSpacing: Theme.space2
                                            rowSpacing: 2

                                            Repeater {
                                                model: [
                                                    qsTr("Object"), qsTr("Source"), qsTr("x"), qsTr("y"),
                                                    qsTr("z"), qsTr("Path"), qsTr("LFE"), qsTr("Keys")
                                                ]
                                                Text {
                                                    required property string modelData
                                                    Layout.fillWidth: true
                                                    text: modelData
                                                    color: Theme.neutral600
                                                    font.pixelSize: 9
                                                    font.capitalization: Font.AllUppercase
                                                }
                                            }

                                            Repeater {
                                                model: EncoderController.objectModel

                                                Rectangle {
                                                    id: row
                                                    required property var modelData
                                                    Layout.columnSpan: 8
                                                    Layout.fillWidth: true
                                                    implicitHeight: rowLayout.implicitHeight + 6
                                                    color: modelData.index === EncoderController.selectedObjectIndex
                                                           ? Theme.accent100 : "transparent"

                                                    RowLayout {
                                                        id: rowLayout
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        width: parent.width
                                                        spacing: Theme.space2

                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: row.modelData.index + 1
                                                            font.family: "monospace"
                                                            font.pixelSize: Theme.fontSmall
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: row.modelData.sourceLabel
                                                            font.pixelSize: Theme.fontSmall
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: row.modelData.x.toFixed(2)
                                                            font.family: "monospace"
                                                            font.pixelSize: Theme.fontSmall
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: row.modelData.y.toFixed(2)
                                                            font.family: "monospace"
                                                            font.pixelSize: Theme.fontSmall
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: row.modelData.z.toFixed(2)
                                                            font.family: "monospace"
                                                            font.pixelSize: Theme.fontSmall
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: row.modelData.hasPath ? qsTr("path") : qsTr("static")
                                                            font.pixelSize: Theme.fontSmall
                                                            color: row.modelData.hasPath ? Theme.text : Theme.textMuted
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: row.modelData.lfeSend.toFixed(2)
                                                            font.family: "monospace"
                                                            font.pixelSize: Theme.fontSmall
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: row.modelData.keyCount
                                                            font.family: "monospace"
                                                            font.pixelSize: Theme.fontSmall
                                                        }
                                                    }

                                                    MouseArea {
                                                        anchors.fill: parent
                                                        onClicked: EncoderController.selectedObjectIndex
                                                                   = row.modelData.index
                                                    }
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.topMargin: Theme.space3
                                            spacing: Theme.space6

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: Theme.space2

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Text {
                                                        text: qsTr("Height — object %1")
                                                              .arg((objectsTab.selectedObj
                                                                    ? objectsTab.selectedObj.index : 0) + 1)
                                                        color: Theme.neutral600
                                                        font.pixelSize: 10
                                                    }
                                                    Item { Layout.fillWidth: true }
                                                    Text {
                                                        text: (objectsTab.selectedObj
                                                               ? objectsTab.selectedObj.z : 0).toFixed(2)
                                                        color: Theme.text
                                                        font.pixelSize: 11
                                                        font.family: "monospace"
                                                    }
                                                }
                                                Slider {
                                                    Layout.fillWidth: true
                                                    from: -1.0
                                                    to: 1.0
                                                    enabled: !EncoderController.busy
                                                             && objectsTab.driveMode === "author"
                                                             && objectsTab.selectedObj !== null
                                                    value: objectsTab.selectedObj ? objectsTab.selectedObj.z : 0
                                                    onMoved: EncoderController.setObjectPosition(
                                                                 objectsTab.selectedObj.index,
                                                                 objectsTab.selectedObj.x,
                                                                 objectsTab.selectedObj.y, value)
                                                }
                                            }

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: Theme.space2

                                                RowLayout {
                                                    Layout.fillWidth: true
                                                    Text {
                                                        text: qsTr("LFE send")
                                                        color: Theme.neutral600
                                                        font.pixelSize: 10
                                                    }
                                                    Item { Layout.fillWidth: true }
                                                    Text {
                                                        text: (objectsTab.selectedObj
                                                               ? objectsTab.selectedObj.lfeSend : 0).toFixed(2)
                                                        color: Theme.text
                                                        font.pixelSize: 11
                                                        font.family: "monospace"
                                                    }
                                                }
                                                Slider {
                                                    Layout.fillWidth: true
                                                    from: 0.0
                                                    to: 1.0
                                                    enabled: !EncoderController.busy
                                                             && objectsTab.driveMode === "author"
                                                             && objectsTab.selectedObj !== null
                                                    value: objectsTab.selectedObj ? objectsTab.selectedObj.lfeSend : 0
                                                    onMoved: EncoderController.setObjectLfeSend(
                                                                 objectsTab.selectedObj.index, value)
                                                }
                                            }
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("Height changes the metadata, not the bed — a 5.1 ring has no speakers above it. The LFE send is the only route to that channel: no direction points at it, so panning never reaches it.")
                                            color: Theme.textMuted
                                            font.pixelSize: Theme.fontSmall
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: EncoderController.atmosEnabled
                                    height: 2
                                    color: Theme.divider
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    visible: EncoderController.atmosEnabled
                                    spacing: Theme.space2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text: qsTr("MOTION")
                                            color: Theme.neutral600
                                            font.pixelSize: 10
                                        }
                                        Text {
                                            text: objectsTab.formatTime(objectsTab.playheadTime)
                                                  + " / " + objectsTab.formatTime(8)
                                            color: Theme.textMuted
                                            font.pixelSize: 11
                                            font.family: "monospace"
                                        }
                                        Item { Layout.fillWidth: true }
                                        Button {
                                            text: qsTr("Add key")
                                            enabled: !EncoderController.busy && objectsTab.selectedObj !== null
                                            onClicked: EncoderController.addObjectKeyframe(
                                                           objectsTab.selectedObj.index, objectsTab.playheadTime)
                                        }
                                        Button {
                                            text: objectsTab.previewing ? qsTr("Stop") : qsTr("Preview")
                                            onClicked: {
                                                if (objectsTab.previewing) {
                                                    objectsTab.previewing = false;
                                                } else {
                                                    objectsTab.playheadTime = 0;
                                                    objectsTab.previewing = true;
                                                }
                                            }
                                        }
                                    }

                                    Timer {
                                        interval: 33
                                        repeat: true
                                        running: objectsTab.previewing
                                        onTriggered: {
                                            objectsTab.playheadTime += interval / 1000;
                                            if (objectsTab.playheadTime >= 8) {
                                                objectsTab.playheadTime = 8;
                                                objectsTab.previewing = false;
                                            }
                                        }
                                    }

                                    Item {
                                        id: timelineWrap
                                        Layout.fillWidth: true
                                        implicitHeight: timelineColumn.implicitHeight

                                        Rectangle {
                                            anchors.fill: parent
                                            color: "transparent"
                                            border.color: Theme.divider
                                            border.width: 1
                                        }

                                        ColumnLayout {
                                            id: timelineColumn
                                            width: parent.width
                                            spacing: 0

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Layout.leftMargin: 70
                                                Layout.rightMargin: 8
                                                Layout.topMargin: 5
                                                Layout.bottomMargin: 5

                                                Repeater {
                                                    model: 9
                                                    Text {
                                                        required property int index
                                                        Layout.fillWidth: true
                                                        horizontalAlignment: index === 0 ? Text.AlignLeft
                                                                             : index === 8 ? Text.AlignRight
                                                                             : Text.AlignHCenter
                                                        text: index === 8 ? qsTr("8 s") : String(index)
                                                        color: Theme.neutral600
                                                        font.pixelSize: 9
                                                        font.family: "monospace"
                                                    }
                                                }
                                            }

                                            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

                                            Repeater {
                                                model: EncoderController.objectModel

                                                RowLayout {
                                                    id: laneRow
                                                    required property var modelData
                                                    Layout.fillWidth: true
                                                    spacing: 0

                                                    Text {
                                                        Layout.preferredWidth: 70
                                                        Layout.leftMargin: 8
                                                        text: qsTr("obj %1").arg(laneRow.modelData.index + 1)
                                                        color: laneRow.modelData.index === EncoderController.selectedObjectIndex
                                                               ? Theme.text : Theme.neutral700
                                                        font.pixelSize: 10
                                                        font.family: "monospace"
                                                    }

                                                    Rectangle {
                                                        id: lane
                                                        Layout.fillWidth: true
                                                        Layout.preferredHeight: 24
                                                        readonly property bool isSelected:
                                                            laneRow.modelData.index === EncoderController.selectedObjectIndex
                                                        readonly property var keys:
                                                            (objectsTab.objectsRevision,
                                                             EncoderController.objectKeyframes(laneRow.modelData.index))
                                                        color: isSelected ? Theme.accent100 : "transparent"

                                                        Rectangle {
                                                            anchors.left: parent.left
                                                            anchors.top: parent.top
                                                            anchors.bottom: parent.bottom
                                                            width: 1
                                                            color: Theme.divider
                                                        }

                                                        Rectangle {
                                                            visible: lane.keys.length > 1
                                                            x: lane.keys.length > 1 ? (lane.keys[0].time / 8) * lane.width : 0
                                                            width: lane.keys.length > 1
                                                                   ? Math.max(0, ((lane.keys[lane.keys.length - 1].time
                                                                                   - lane.keys[0].time) / 8) * lane.width)
                                                                   : 0
                                                            y: lane.height / 2
                                                            height: 1
                                                            color: lane.isSelected ? Theme.accent400 : Theme.neutral400
                                                        }

                                                        Repeater {
                                                            model: lane.keys
                                                            Rectangle {
                                                                required property var modelData
                                                                width: 8
                                                                height: 8
                                                                rotation: 45
                                                                color: lane.isSelected ? Theme.accent : Theme.text
                                                                x: (modelData.time / 8) * lane.width - width / 2
                                                                y: lane.height / 2 - height / 2
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Rectangle {
                                            x: 70 + (objectsTab.playheadTime / 8) * (timelineWrap.width - 70 - 8)
                                            y: 0
                                            width: 2
                                            height: timelineWrap.height
                                            color: Theme.accent
                                        }
                                    }
                                }
                            }

                            Item { Layout.fillHeight: true }
                        }

                        // ---- Live session ------------------------------------------
                        // Only ever shown while EncoderController.liveActive - a session
                        // that has not started has nothing here to show, which is why
                        // this tab does not exist in visibleTabs until then.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            Rectangle {
                                Layout.fillWidth: true
                                visible: EncoderController.liveReconnecting
                                color: Theme.accent100
                                implicitHeight: reconnectMsg.implicitHeight + Theme.space3 * 2

                                Text {
                                    id: reconnectMsg
                                    anchors.fill: parent
                                    anchors.margins: Theme.space3
                                    text: qsTr("Renegotiating with the receiver. It is re-locking to the new bitstream format — expect a second of silence.")
                                    color: Theme.accent800
                                    font.pixelSize: Theme.fontSmall
                                    wrapMode: Text.WordWrap
                                }
                            }

                            Card {
                                title: qsTr("Live session")

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space6

                                    Button {
                                        text: qsTr("Stop session")
                                        highlighted: true
                                        onClicked: EncoderController.stopLiveSession()
                                    }

                                    ColumnLayout {
                                        spacing: 2
                                        Text { text: qsTr("RUNNING"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            text: {
                                                const s = EncoderController.liveRunningSeconds;
                                                const m = Math.floor(s / 60);
                                                const rem = s - m * 60;
                                                return m + ":" + rem.toFixed(1).padStart(4, "0");
                                            }
                                            color: Theme.text
                                            font.pixelSize: 15
                                            font.family: "monospace"
                                        }
                                    }
                                    ColumnLayout {
                                        spacing: 2
                                        Text { text: qsTr("FRAMES"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            text: EncoderController.liveFramesEncoded
                                            color: Theme.text
                                            font.pixelSize: 15
                                            font.family: "monospace"
                                        }
                                    }
                                    ColumnLayout {
                                        spacing: 2
                                        Text { text: qsTr("DROPPED"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            text: EncoderController.liveFramesDropped
                                            color: EncoderController.liveFramesDropped > 0 ? Theme.bad : Theme.text
                                            font.pixelSize: 15
                                            font.family: "monospace"
                                        }
                                    }

                                    Item { Layout.fillWidth: true }

                                    CheckBox {
                                        text: qsTr("Also writing the take to disk")
                                        checked: EncoderController.liveWritingToDisk
                                        enabled: false
                                    }
                                }
                            }

                            Card {
                                title: qsTr("Chain")

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Text { text: qsTr("CAPTURE"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("Capture device")
                                            color: Theme.text
                                            font.pixelSize: Theme.fontNormal
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                    Text {
                                        text: "→"
                                        color: Theme.neutral500
                                        font.pixelSize: 18
                                        Layout.leftMargin: Theme.space2
                                        Layout.rightMargin: Theme.space2
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Text { text: qsTr("LIVE ENCODE"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            Layout.fillWidth: true
                                            text: window.planLine
                                            color: Theme.text
                                            font.pixelSize: Theme.fontNormal
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                    Text {
                                        text: "→"
                                        color: Theme.neutral500
                                        font.pixelSize: 18
                                        Layout.leftMargin: Theme.space2
                                        Layout.rightMargin: Theme.space2
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Text { text: qsTr("RECEIVER LEG"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            Layout.fillWidth: true
                                            text: EncoderController.liveReceiverPlanText
                                            color: Theme.text
                                            font.pixelSize: Theme.fontNormal
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                visible: EncoderController.liveGap
                                color: Theme.accent100
                                implicitHeight: gapMsg.implicitHeight + Theme.space3 * 2

                                Text {
                                    id: gapMsg
                                    anchors.fill: parent
                                    anchors.margins: Theme.space3
                                    text: qsTr("Everything past what the receiver leg carries — the extra channels, every object move — is visible on the meters and the soundfield but not audible on the amplifier.")
                                    color: Theme.accent800
                                    font.pixelSize: Theme.fontSmall
                                    wrapMode: Text.WordWrap
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.space6

                                Card {
                                    visible: EncoderController.atmosEnabled
                                    Layout.preferredWidth: 340
                                    title: qsTr("Live room")

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text: qsTr("drag to move")
                                            color: Theme.neutral600
                                            font.pixelSize: 10
                                            font.family: "monospace"
                                        }
                                        Item { Layout.fillWidth: true }
                                        Text {
                                            text: qsTr("latency %1 ms").arg(EncoderController.liveLatencyMs.toFixed(0))
                                            color: Theme.neutral600
                                            font.pixelSize: 10
                                            font.family: "monospace"
                                        }
                                    }

                                    Rectangle {
                                        id: liveRoom
                                        Layout.preferredWidth: 320
                                        Layout.preferredHeight: 320
                                        color: Theme.neutral100
                                        border.color: Theme.divider
                                        border.width: 1

                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: EncoderController.liveActive
                                            onPositionChanged: (mouse) => place(mouse)
                                            onPressed: (mouse) => place(mouse)
                                            function place(mouse) {
                                                const list = EncoderController.objectModel;
                                                const sel = EncoderController.selectedObjectIndex;
                                                let selected = null;
                                                for (let i = 0; i < list.length; ++i) {
                                                    if (list[i].index === sel) { selected = list[i]; break; }
                                                }
                                                if (!selected) {
                                                    return;
                                                }
                                                const x = Math.max(0, Math.min(1, mouse.x / liveRoom.width));
                                                const y = Math.max(0, Math.min(1, mouse.y / liveRoom.height));
                                                EncoderController.setObjectPosition(selected.index, x, y, selected.z);
                                            }
                                        }

                                        Repeater {
                                            model: EncoderController.objectModel
                                            Rectangle {
                                                required property var modelData
                                                readonly property bool isSelected:
                                                    modelData.index === EncoderController.selectedObjectIndex
                                                width: isSelected ? 18 : 14
                                                height: isSelected ? 18 : 14
                                                color: isSelected ? Theme.accent : Theme.neutral800
                                                x: modelData.x * liveRoom.width - width / 2
                                                y: modelData.y * liveRoom.height - height / 2

                                                MouseArea {
                                                    anchors.fill: parent
                                                    onClicked: EncoderController.selectedObjectIndex = modelData.index
                                                }
                                            }
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignTop
                                    spacing: Theme.gap

                                    Card {
                                        title: qsTr("Current layout")

                                        Text {
                                            Layout.fillWidth: true
                                            text: EncoderController.atmosEnabled
                                                  ? qsTr("Atmos objects over a 5.1 bed")
                                                  : EncoderController.channelShapeName
                                            color: Theme.text
                                            font.pixelSize: Theme.fontNormal
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("Fixed for this run — change it from the Format tab and start a new session for it to take effect.")
                                            color: Theme.textMuted
                                            font.pixelSize: Theme.fontSmall
                                            wrapMode: Text.WordWrap
                                        }
                                    }

                                    Card {
                                        title: qsTr("Receiver reports")

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.preferredWidth: 80
                                                text: qsTr("Receiver")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: EncoderController.liveReceiverPlanText
                                                color: Theme.text
                                                font.pixelSize: Theme.fontNormal
                                                wrapMode: Text.WordWrap
                                            }
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.preferredWidth: 80
                                                text: qsTr("Lock")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Text {
                                                text: !EncoderController.livePassthrough ? qsTr("no passthrough")
                                                      : EncoderController.liveReconnecting ? qsTr("re-locking")
                                                      : qsTr("locked")
                                                color: EncoderController.liveReconnecting ? Theme.bad : Theme.text
                                                font.pixelSize: Theme.fontNormal
                                            }
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.preferredWidth: 80
                                                text: qsTr("Underruns")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Text {
                                                text: EncoderController.liveUnderruns
                                                color: EncoderController.liveUnderruns > 0 ? Theme.bad : Theme.text
                                                font.pixelSize: Theme.fontNormal
                                                font.family: "monospace"
                                            }
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.preferredWidth: 80
                                                text: qsTr("Monitor")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Text {
                                                text: EncoderController.liveMonitoring ? qsTr("on") : qsTr("off")
                                                color: Theme.text
                                                font.pixelSize: Theme.fontNormal
                                            }
                                        }
                                    }
                                }
                            }

                            Item { Layout.fillHeight: true }
                        }

                        // ---- Guided wizard ---------------------------------------
                        // Appended last, past every tabOrder entry - see the
                        // StackLayout's own currentIndex comment above for why
                        // nothing needs tabOrder to name this page for it to work.
                        GuidedWizard {
                            Layout.fillWidth: true
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 2; color: Theme.divider }

                // ---- runs --------------------------------------------------------
                // Encoding is a job with a history, not a modal moment: one chip per
                // past run plus whichever is in flight, newest first, scrolling
                // horizontally rather than replacing itself every time.
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

                    ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: runStrip.implicitWidth
                        ScrollBar.vertical.policy: ScrollBar.AlwaysOff

                        RowLayout {
                            id: runStrip
                            height: parent.height
                            spacing: 0

                            Repeater {
                                model: EncoderController.runs

                                delegate: RowLayout {
                                    required property var modelData
                                    readonly property bool encoding: modelData.status === "encoding"
                                    readonly property bool failed: modelData.status === "failed"

                                    Layout.leftMargin: 16
                                    spacing: 8

                                    Rectangle {
                                        width: 8
                                        height: 8
                                        color: encoding || failed ? Theme.accent : Theme.neutral400
                                    }
                                    Text {
                                        font.family: "monospace"
                                        font.pixelSize: 12
                                        color: Theme.text
                                        text: encoding
                                              ? qsTr("%1 · %2 · %3 · %4%")
                                                .arg(modelData.id).arg(modelData.filename)
                                                .arg(modelData.rateText)
                                                .arg(Math.round(EncoderController.progress * 100))
                                              : qsTr("%1 · %2 · %3 · %4%5")
                                                .arg(modelData.id).arg(modelData.filename)
                                                .arg(modelData.rateText).arg(modelData.durationText)
                                                .arg(modelData.sizeText.length > 0
                                                     ? " · " + modelData.sizeText : "")
                                    }
                                    ProgressBar {
                                        visible: encoding
                                        Layout.preferredWidth: 90
                                        Layout.preferredHeight: 5
                                        from: 0
                                        to: 1
                                        value: EncoderController.progress
                                    }
                                    Button {
                                        visible: encoding
                                        text: qsTr("Cancel")
                                        flat: true
                                        onClicked: EncoderController.cancel()
                                    }
                                    Button {
                                        visible: failed
                                        text: qsTr("Details")
                                        flat: true
                                        onClicked: {
                                            window.bannerRunId = modelData.id;
                                            if (window.dismissedRunId === modelData.id) {
                                                window.dismissedRunId = -1;
                                            }
                                        }
                                    }
                                    Rectangle {
                                        Layout.preferredWidth: 1
                                        Layout.fillHeight: true
                                        Layout.topMargin: 8
                                        Layout.bottomMargin: 8
                                        Layout.leftMargin: 8
                                        color: Theme.divider
                                    }
                                }
                            }

                            Text {
                                visible: EncoderController.runs.length === 0
                                Layout.leftMargin: 16
                                text: EncoderController.status
                                font.family: "monospace"
                                font.pixelSize: 12
                                color: Theme.textMuted
                            }
                        }
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
