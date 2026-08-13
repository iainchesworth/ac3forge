import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

// Our own module: brings in the Theme singleton and the EncoderController
// singleton registered from C++ (QML_ELEMENT + QML_SINGLETON). The implicit
// same-directory import covers the QML-defined types but not the C++ ones.
import Ac3Forge

// The two-pane workbench of the design handoff: the SIGNAL on a permanent
// left rail (input, levels, soundfield — never scrolled away by
// configuration), the STREAM in a tabbed panel on the right, a plan line
// above and the command bar below. Until a source has ever been chosen the
// body is the first-run screen instead.
ApplicationWindow {
    id: window

    // The handoff's honest floor: below 1280x900 the rail (340px) and the
    // Format grid no longer have room to reflow rather than clip.
    width: 1280
    height: 900
    minimumWidth: 1280
    minimumHeight: 900
    visible: true
    color: Theme.bg

    function baseName(path) {
        const normalized = path.replace(/\\/g, "/");
        const slash = normalized.lastIndexOf("/");
        return slash >= 0 ? normalized.substring(slash + 1) : normalized;
    }
    readonly property string sourceLabel: (EncoderController.recording || EncoderController.liveActive)
                                           ? qsTr("live capture")
                                           : (EncoderController.sourcePath.length > 0
                                              ? window.baseName(EncoderController.sourcePath)
                                              : qsTr("no source"))
    title: qsTr("ac3forge — %1").arg(sourceLabel)

    // Fusion draws every standard control - Button, CheckBox, Switch,
    // Slider, ProgressBar, ComboBox, SpinBox - from these palette roles,
    // never from a literal. Left unset, Fusion falls back to its own default
    // palette regardless of Theme - the "pale pink on every switch and
    // slider" the handoff calls out as the single most visible inconsistency.
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

    // ---- persisted preferences ---------------------------------------------
    // main.cpp sets the application/organization name, so an interactive run
    // persists; the QML test binary sets neither, so tests read defaults.
    Settings {
        id: appSettings
        category: "workbench"
        property string theme: "system"
        property string controlsOnOpen: "guided"
        property string lastTier: "guided"
        property string meterMode: "coded"
        property bool showExplanations: true
        property bool warnCodecChange: false
        property bool restoreSession: true
        property bool restoreScreen: false
        property string outputFolder: ""
        property string namePattern: "{source}.{ext}"
        property bool keepPartial: true
        property bool showCli: true
        property int defaultContainerIndex: 0
        property bool defaultVbr: false
        property int defaultBitrateKbps: 192
        property int defaultVbrQuality: 75
        property int defaultDrcIndex: 0
        property bool defaultMeasureDialnorm: false
        property bool autoMonitor: true
        property bool askRecordName: false
        // The last session, saved on close: source paths, assignment tokens
        // and the channel plan they were made against - restored on open
        // when restoreSession is on. JSON strings because Settings stores
        // flat values.
        property string sessionSources: "[]"
        property string sessionAssignments: "[]"
        property string sessionBed: ""
        property bool sessionLfe: false
        property string sessionExtras: "[]"
        property bool sessionAtmos: false
        property string sessionTab: "format"
        property string sessionInput: "file"
    }

    // The Preferences "show the plain-language notes" knob, read once per
    // binding site rather than each note reaching into appSettings itself.
    readonly property bool showExplanations: appSettings.showExplanations

    // For the Qt Quick Test harness: the settings store and the dialog are
    // otherwise unreachable ids (Settings is not an Item, and a Dialog lives
    // on the Overlay), and the tests exercise preference-driven flows.
    readonly property alias settings: appSettings
    readonly property alias prefsDialog: preferencesDialog

    Component.onCompleted: {
        Theme.preference = appSettings.theme;
        meterMode = appSettings.meterMode;
        tier = appSettings.controlsOnOpen === "last"
               ? appSettings.lastTier : appSettings.controlsOnOpen;
        EncoderController.keepPartialOutput = appSettings.keepPartial;
        EncoderController.containerIndex = appSettings.defaultContainerIndex;
        EncoderController.bitrateKbps = appSettings.defaultBitrateKbps;
        EncoderController.vbrQuality = appSettings.defaultVbrQuality;
        EncoderController.vbrEnabled = appSettings.defaultVbr;
        EncoderController.drcIndex = appSettings.defaultDrcIndex;
        EncoderController.measureDialnorm = appSettings.defaultMeasureDialnorm;
        restoreSession();
    }

    onClosing: saveSession()

    // ---- session restore ----------------------------------------------------
    // "Reopen the last session's sources and assignments": the source list,
    // the assignment table and the channel plan those assignments were made
    // against, saved as one unit on close. A file that has gone missing
    // since fails its load with the usual status message rather than
    // aborting the rest.
    function saveSession() {
        const sources = EncoderController.sourceModel;
        const paths = [];
        for (let i = 0; i < sources.length; i++) {
            paths.push(sources[i].path);
        }
        appSettings.sessionSources = JSON.stringify(paths);

        const rows = EncoderController.assignmentRows;
        const assignments = [];
        for (let i = 0; i < rows.length; i++) {
            if (rows[i].touched === true || rows[i].destToken !== "none") {
                assignments.push({ s: rows[i].source, c: rows[i].channel,
                                   token: rows[i].destToken });
            }
        }
        appSettings.sessionAssignments = JSON.stringify(assignments);

        const beds = EncoderController.bedChoices;
        appSettings.sessionBed = EncoderController.bedIndex >= 0
                                 && EncoderController.bedIndex < beds.length
                                 ? beds[EncoderController.bedIndex].id : "";
        appSettings.sessionLfe = EncoderController.bedLfe;
        const extras = EncoderController.extrasModel;
        const on = [];
        for (let i = 0; i < extras.length; i++) {
            if (extras[i].checked) on.push(extras[i].id);
        }
        appSettings.sessionExtras = JSON.stringify(on);
        appSettings.sessionAtmos = EncoderController.atmosEnabled;
        appSettings.sessionTab = currentTab;
        appSettings.sessionInput = inputMode;
    }

    function restoreSession() {
        if (!appSettings.restoreSession) {
            return;
        }
        let paths = [];
        let assignments = [];
        let extras = [];
        try {
            paths = JSON.parse(appSettings.sessionSources);
            assignments = JSON.parse(appSettings.sessionAssignments);
            extras = JSON.parse(appSettings.sessionExtras);
        } catch (error) {
            return; // a mangled store restores nothing rather than half of it
        }
        if (paths.length === 0) {
            return;
        }
        for (let i = 0; i < paths.length; i++) {
            EncoderController.addSourceFile("file:///" + paths[i].replace(/\\/g, "/").replace(/^\//, ""));
        }
        if (EncoderController.sourceModel.length === 0) {
            return; // nothing loaded (files moved); leave the first-run screen up
        }
        // The plan the assignments were made against, before the assignments
        // themselves - loadSourceFile settled the bed on the file's natural
        // layout, which is not necessarily what was saved.
        if (!appSettings.sessionAtmos && appSettings.sessionBed.length > 0) {
            const beds = EncoderController.bedChoices;
            for (let i = 0; i < beds.length; i++) {
                if (beds[i].id === appSettings.sessionBed) {
                    EncoderController.bedIndex = i;
                    break;
                }
            }
            EncoderController.bedLfe = appSettings.sessionLfe;
            const model = EncoderController.extrasModel;
            for (let i = 0; i < model.length; i++) {
                const wantOn = extras.indexOf(model[i].id) >= 0;
                if (model[i].checked !== wantOn) {
                    EncoderController.toggleExtra(model[i].id);
                }
            }
        }
        for (let i = 0; i < assignments.length; i++) {
            EncoderController.setAssignment(assignments[i].s, assignments[i].c,
                                            assignments[i].token);
        }
        if (appSettings.sessionAtmos) {
            EncoderController.atmosEnabled = true;
        }
        if (appSettings.restoreScreen) {
            inputMode = appSettings.sessionInput;
            if (tabOrder.indexOf(appSettings.sessionTab) >= 0) {
                currentTab = appSettings.sessionTab;
            }
        }
    }

    // ---- Guided / Advanced / Expert and the tab bar ------------------------
    // Guided is a step-by-step wrapper over the SAME rules — one more
    // StackLayout page, not a separate mode with its own draft state.
    property string tier: "guided"
    property string currentTab: "format"
    // "coded" | "rendered" — the fourteen-rows-for-twelve-speakers question
    // turned into a mode rather than a puzzle.
    property string meterMode: "coded"
    // "file" | "live" — the unified input selector of rail block 01.
    property string inputMode: "file"
    // Set once anything has ever been loaded or captured; until then the
    // body shows the first-run screen instead of the workbench.
    property bool everHadSource: EncoderController.sourceModel.length > 0
                                 || EncoderController.recording
                                 || EncoderController.liveActive
    // Guided's jump into the full assignment table keeps a way back — the
    // round trip is lossless because both surfaces edit the same state.
    property bool fromGuided: false

    function goAssign() {
        if (tier === "guided") {
            fromGuided = true;
            tier = "advanced";
        }
        currentTab = "format";
    }

    // ---- the panel banner: one of the three feedback homes -----------------
    property int bannerRunId: -1
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
        const tabs = [{ key: "format", label: qsTr("Format"), badge: "" }];
        if (tier === "expert") {
            const toolsOn = (EncoderController.coupling ? 1 : 0)
                          + (EncoderController.spx ? 1 : 0)
                          + (EncoderController.aht ? 1 : 0);
            const metaOn = (EncoderController.heavy ? 1 : 0)
                         + (EncoderController.mixmeta ? 1 : 0)
                         + (EncoderController.drcIndex > 0 ? 1 : 0);
            tabs.push({ key: "coding", label: qsTr("Coding tools"),
                        badge: toolsOn > 0 ? String(toolsOn) : "" });
            tabs.push({ key: "meta", label: qsTr("Metadata"),
                        badge: metaOn > 0 ? String(metaOn) : "" });
        }
        tabs.push({ key: "objects", label: qsTr("Objects"),
                    badge: EncoderController.atmosEnabled ? qsTr("on") : "" });
        if (EncoderController.liveActive) {
            tabs.push({ key: "session", label: qsTr("Live session"), badge: "" });
        }
        return tabs;
    }
    onTierChanged: {
        appSettings.lastTier = tier;
        if (tier !== "expert" && (currentTab === "coding" || currentTab === "meta")) {
            currentTab = "format";
        }
    }

    Connections {
        target: EncoderController
        function onLiveActiveChanged() {
            if (EncoderController.liveActive) {
                window.currentTab = "session";
                window.inputMode = "live";
            } else if (window.currentTab === "session") {
                window.currentTab = "format";
            }
        }
        function onRecordingChanged() {
            if (EncoderController.recording) {
                window.inputMode = "live";
            }
        }
    }

    // ---- the plan headline and the CLI line --------------------------------
    // Derived, never typed. Both read properties carrying NOTIFY planChanged,
    // so they stay live even though outputSuffix() is a plain invokable.
    readonly property string planLine: {
        const codec = EncoderController.codecNames[EncoderController.codecIndex] || "";
        const rate = EncoderController.vbrAvailable && EncoderController.vbrEnabled
                     ? qsTr("quality %1").arg(EncoderController.vbrQuality)
                     : qsTr("%1 kbps").arg(EncoderController.bitrateKbps);
        if (EncoderController.atmosEnabled) {
            const objects = EncoderController.objectCount;
            const shape = objects > 0 ? qsTr("5.1 bed + %1 objects").arg(objects)
                                      : qsTr("5.1 bed");
            return qsTr("%1 · %2 · %3 · .%4")
                .arg(codec).arg(shape).arg(rate).arg(EncoderController.outputSuffix());
        }
        return qsTr("%1 · %2 · %3 · .%4")
            .arg(codec).arg(EncoderController.channelShapeName).arg(rate)
            .arg(EncoderController.outputSuffix());
    }
    readonly property string planSubLine: EncoderController.dualMono && !EncoderController.atmosEnabled
                                          ? qsTr("acmod 0 · two independent programmes in one stream · no soundfield, no downmix")
                                          : EncoderController.layoutDetail
    // ac3cli's actual grammar — real, pasteable syntax, not the handoff's
    // "--bed/--extras" sketch (which does not match ac3cli's positional
    // subcommands). Assignments ride as map= once one exists.
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

    // The Preferences "Name new files" pattern, applied: {source} is the
    // first source's basename, {ext} the suffix the plan derives.
    function plannedFileName(sourceStem) {
        const stem = sourceStem !== undefined ? sourceStem
                     : (EncoderController.sourcePath.length > 0
                        ? baseName(EncoderController.sourcePath).replace(/\.[^.]*$/, "")
                        : "output");
        return appSettings.namePattern
            .replace("{source}", stem)
            .replace("{ext}", EncoderController.outputSuffix());
    }
    // The Preferences output folder as a file:// url, falling back to
    // "beside the first source", the pattern's own default.
    function outputFolderUrl() {
        if (appSettings.outputFolder.length > 0) {
            return appSettings.outputFolder;
        }
        if (EncoderController.sourcePath.length > 0) {
            const normalized = EncoderController.sourcePath.replace(/\\/g, "/");
            const slash = normalized.lastIndexOf("/");
            if (slash > 0) {
                return "file:///" + normalized.substring(0, slash).replace(/^\//, "");
            }
        }
        return StandardPaths.writableLocation(StandardPaths.MusicLocation);
    }

    function openSaveDialog(dialog, name) {
        const suffix = EncoderController.outputSuffix();
        dialog.defaultSuffix = suffix;
        dialog.nameFilters = [qsTr("%1 file (*.%2)").arg(suffix.toUpperCase()).arg(suffix),
                              qsTr("All files (*)")];
        dialog.currentFolder = outputFolderUrl();
        dialog.selectedFile = name;
        dialog.open();
    }
    // What the Encode button and guided's "Encode now" both run — one flow,
    // so the two can never drift apart.
    function startEncodeFlow() {
        openSaveDialog(saveDialog, plannedFileName());
    }
    // Record honours the capture preference: ask for a filename, or write
    // straight to the output folder under a timestamped take name — the
    // status line and run strip always say where it went.
    function startRecordFlow() {
        if (appSettings.askRecordName) {
            openSaveDialog(recordDialog, plannedFileName());
            return;
        }
        const now = new Date();
        const pad = (n) => String(n).padStart(2, "0");
        const stem = "take-" + now.getFullYear() + pad(now.getMonth() + 1) + pad(now.getDate())
                     + "-" + pad(now.getHours()) + pad(now.getMinutes()) + pad(now.getSeconds());
        EncoderController.startRecording(deviceBox.currentIndex,
                                         outputFolderUrl() + "/" + plannedFileName(stem));
    }
    // "Warn before a choice changes the codec": when the preference is on
    // and an action would promote AC-3 to Dolby Digital Plus, the action
    // waits behind a confirm — the codec still follows the channels either
    // way; the warning only makes the moment deliberate.
    property var pendingPromotion: null
    function withCodecWarning(promotes, action) {
        if (promotes && appSettings.warnCodecChange
            && EncoderController.codecIndex === 0
            && !EncoderController.atmosEnabled && !EncoderController.dualMono) {
            pendingPromotion = action;
            codecWarnDialog.open();
            return;
        }
        action();
    }

    // Hidden text surface for the command bar's Copy button — the standard
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

    PreferencesDialog {
        id: preferencesDialog
        settings: appSettings
        onApplied: {
            Theme.preference = appSettings.theme;
            window.meterMode = appSettings.meterMode;
            EncoderController.keepPartialOutput = appSettings.keepPartial;
        }
    }

    Dialog {
        id: codecWarnDialog
        modal: true
        anchors.centerIn: parent
        padding: Theme.space4

        background: Rectangle {
            color: Theme.bg
            border.color: Theme.text
            border.width: 2
        }

        contentItem: ColumnLayout {
            spacing: Theme.space3

            Text {
                text: qsTr("This moves the stream to Dolby Digital Plus")
                font.pixelSize: 15
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Text {
                Layout.preferredWidth: 380
                text: qsTr("Anything past a bed and its LFE needs Dolby Digital Plus, so the codec follows the channels — the file becomes .ec3 rather than .ac3. Every modern receiver reads it; a DVD player will not.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.neutral700
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Button {
                    text: qsTr("Cancel")
                    onClicked: {
                        window.pendingPromotion = null;
                        codecWarnDialog.reject();
                    }
                }
                Button {
                    objectName: "codecWarnContinue"
                    text: qsTr("Continue")
                    highlighted: true
                    onClicked: {
                        const action = window.pendingPromotion;
                        window.pendingPromotion = null;
                        codecWarnDialog.accept();
                        if (action) action();
                    }
                }
            }
        }
    }

    // =========================================================================
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---- faux title strip ----------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.neutral200

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: Theme.space3
                spacing: 6

                Repeater {
                    model: 3
                    Rectangle { width: 11; height: 11; color: Theme.neutral400 }
                }
                Text {
                    Layout.leftMargin: Theme.space2
                    text: window.title.toUpperCase()
                    font.pixelSize: 11
                    font.letterSpacing: 1.2
                    color: Theme.neutral700
                }
                Item { Layout.fillWidth: true }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        // ---- header ----------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 14
            Layout.bottomMargin: 14
            spacing: Theme.space4

            Text {
                text: qsTr("ac3forge")
                font.pixelSize: 22
                font.family: Theme.headingFamily
                font.weight: Font.ExtraBold
                font.letterSpacing: -0.2
                color: Theme.text
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Clean-room AC-3 / E-AC-3 encoder — ATSC A/52, ETSI TS 103 420")
                font.pixelSize: 12
                elide: Text.ElideRight
                color: Theme.neutral700
            }

            Text {
                text: qsTr("CONTROLS")
                font.pixelSize: 10
                font.letterSpacing: 1.2
                color: Theme.textMuted
            }
            SegmentedControl {
                model: [
                    { value: "guided", label: qsTr("Guided") },
                    { value: "advanced", label: qsTr("Advanced") },
                    { value: "expert", label: qsTr("Expert") },
                ]
                currentValue: window.tier
                onSelected: (value) => window.tier = value
            }
            Button {
                text: qsTr("Preferences")
                onClicked: preferencesDialog.open()
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

        // ---- first run -------------------------------------------------------
        FirstRunScreen {
            visible: !window.everHadSource
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 48
            onChooseFile: openDialog.open()
            onCaptureLive: {
                window.everHadSource = true;
                window.inputMode = "live";
            }
            onOpenTestSignal: EncoderController.loadBundledTestSignal()
        }

        // ---- the workbench ---------------------------------------------------
        RowLayout {
            visible: window.everHadSource
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // =============================================================
            // Left rail — the signal. Always visible, never scrolled away
            // by configuration.
            // =============================================================
            ScrollView {
                Layout.preferredWidth: 404
                Layout.minimumWidth: 340
                Layout.fillHeight: true
                contentWidth: availableWidth
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ColumnLayout {
                    width: Math.max(340, parent ? parent.width : 340)
                    spacing: 0

                    // ---- 01 / Input -------------------------------------
                    RailBlock {
                        ordinal: "01"
                        label: qsTr("INPUT")
                        Layout.fillWidth: true
                        Layout.margins: Theme.space4

                        SegmentedControl {
                            Layout.fillWidth: true
                            segHeight: 32
                            model: [
                                { value: "file", label: qsTr("File") },
                                { value: "live", label: qsTr("Live capture") },
                            ]
                            currentValue: window.inputMode
                            onSelected: (value) => window.inputMode = value
                        }

                        // ---- file branch: the source list ---------------
                        ColumnLayout {
                            visible: window.inputMode === "file"
                            Layout.fillWidth: true
                            spacing: 0

                            Repeater {
                                id: sourceList
                                model: EncoderController.sourceModel

                                delegate: ColumnLayout {
                                    id: sourceRow
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    spacing: 0

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 8
                                        Layout.bottomMargin: 8
                                        spacing: Theme.space2

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 1
                                            Text {
                                                Layout.fillWidth: true
                                                text: sourceRow.modelData.label
                                                elide: Text.ElideMiddle
                                                font.pixelSize: 12
                                                font.family: Theme.monoFamily
                                                font.weight: Font.DemiBold
                                                color: Theme.text
                                            }
                                            Text {
                                                text: qsTr("%1 ch · %2%3")
                                                      .arg(sourceRow.modelData.channels)
                                                      .arg(sourceRow.modelData.duration)
                                                      .arg(sourceRow.modelData.primary ? "" : qsTr(" · added"))
                                                font.pixelSize: 11
                                                color: Theme.textMuted
                                            }
                                        }
                                        Button {
                                            text: qsTr("Remove")
                                            flat: true
                                            font.pixelSize: 11
                                            enabled: !EncoderController.busy
                                            onClicked: EncoderController.removeSource(sourceRow.modelData.index)
                                        }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: Theme.neutral200
                                    }
                                }
                            }

                            Text {
                                visible: sourceList.count === 0
                                Layout.topMargin: 8
                                Layout.bottomMargin: 4
                                text: qsTr("No source loaded yet.")
                                font.pixelSize: 12
                                color: Theme.textMuted
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                spacing: Theme.space2

                                Button {
                                    id: chooseWavButton
                                    objectName: "chooseWavButton"
                                    text: sourceList.count === 0 ? qsTr("Choose WAV…") : qsTr("+ Add files…")
                                    enabled: !EncoderController.busy
                                    onClicked: sourceList.count === 0 ? openDialog.open()
                                                                      : addSourceDialog.open()
                                }
                                Button {
                                    objectName: "railAssignButton"
                                    text: qsTr("Assign")
                                    flat: true
                                    visible: sourceList.count > 0
                                    onClicked: window.goAssign()
                                    contentItem: Text {
                                        text: qsTr("Assign")
                                        color: Theme.accent700
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }

                            // Totals strip on a 1px top rule.
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                Layout.preferredHeight: 1
                                color: Theme.neutral300
                                visible: sourceList.count > 0
                            }
                            RowLayout {
                                visible: sourceList.count > 0
                                Layout.fillWidth: true
                                Layout.topMargin: 6
                                spacing: Theme.space4

                                Repeater {
                                    model: {
                                        const sources = EncoderController.sourceModel;
                                        let channels = 0;
                                        let seconds = 0;
                                        for (let i = 0; i < sources.length; i++) {
                                            channels += sources[i].channels;
                                            seconds = Math.max(seconds, sources[i].seconds);
                                        }
                                        const mm = Math.floor(seconds / 60);
                                        const ss = String(Math.floor(seconds % 60)).padStart(2, "0");
                                        return [
                                            { label: qsTr("RATE"), value: sources.length > 0 ? String(sources[0].rate) : "—" },
                                            { label: qsTr("SOURCES"), value: qsTr("%1 · %2 ch").arg(sources.length).arg(channels) },
                                            { label: qsTr("LENGTH"), value: mm + ":" + ss },
                                        ];
                                    }
                                    delegate: ColumnLayout {
                                        required property var modelData
                                        spacing: 1
                                        Text {
                                            text: modelData.label
                                            font.pixelSize: 10
                                            font.letterSpacing: 1
                                            color: Theme.textMuted
                                        }
                                        Text {
                                            text: modelData.value
                                            font.pixelSize: 13
                                            font.family: Theme.monoFamily
                                            color: Theme.text
                                        }
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }

                        // ---- live branch ---------------------------------
                        ColumnLayout {
                            visible: window.inputMode === "live"
                            Layout.fillWidth: true
                            spacing: Theme.space2

                            ComboBox {
                                id: deviceBox
                                Layout.fillWidth: true
                                enabled: !EncoderController.busy
                                model: EncoderController.captureDevices
                                // "Start monitoring as soon as a device is
                                // chosen" — on the explicit pick gesture only
                                // (onActivated is user interaction, never a
                                // binding), and never over a running session.
                                onActivated: {
                                    if (appSettings.autoMonitor && !EncoderController.busy
                                        && EncoderController.captureSupported) {
                                        EncoderController.startLiveSession(
                                            deviceBox.currentIndex, true, -1, false, "");
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.space2

                                Button {
                                    text: qsTr("Refresh")
                                    enabled: !EncoderController.busy
                                    onClicked: EncoderController.refreshCaptureDevices()
                                }
                                Button {
                                    id: monitorButton
                                    objectName: "monitorButton"
                                    // Monitoring runs the meters with no filename and
                                    // nothing written — checking the signal never
                                    // commits to a take.
                                    text: EncoderController.liveActive ? qsTr("Stop") : qsTr("Monitor")
                                    highlighted: !EncoderController.liveActive
                                    enabled: EncoderController.captureSupported
                                             && (!EncoderController.busy || EncoderController.liveActive)
                                    onClicked: {
                                        if (EncoderController.liveActive) {
                                            EncoderController.stopLiveSession();
                                        } else {
                                            EncoderController.startLiveSession(
                                                deviceBox.currentIndex, true, -1, false, "");
                                        }
                                    }
                                }
                                Button {
                                    text: EncoderController.recording ? qsTr("Stop") : qsTr("Record…")
                                    enabled: EncoderController.captureSupported
                                             && (EncoderController.recording || !EncoderController.busy)
                                    onClicked: {
                                        if (EncoderController.recording) {
                                            EncoderController.stopRecording();
                                        } else {
                                            window.startRecordFlow();
                                        }
                                    }
                                }
                                Text {
                                    visible: EncoderController.liveActive || EncoderController.recording
                                    text: EncoderController.recording
                                          ? qsTr("%1 s").arg(EncoderController.recordedSeconds.toFixed(1))
                                          : qsTr("%1 s").arg(EncoderController.liveRunningSeconds.toFixed(1))
                                    font.pixelSize: 12
                                    font.family: Theme.monoFamily
                                    color: Theme.accent700
                                }
                                Item { Layout.fillWidth: true }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.space2

                                CheckBox {
                                    id: liveMonitorCheck
                                    text: qsTr("Monitor")
                                    checked: true
                                    enabled: !EncoderController.busy
                                    font.pixelSize: 12
                                }
                                ComboBox {
                                    id: liveReceiverBox
                                    Layout.fillWidth: true
                                    enabled: !EncoderController.busy
                                    model: [qsTr("No passthrough")].concat(EncoderController.outputDevices)
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.space2

                                CheckBox {
                                    id: liveWriteCheck
                                    text: qsTr("Also write the take to disk")
                                    enabled: !EncoderController.busy
                                    font.pixelSize: 12
                                }
                                Button {
                                    text: qsTr("Start live session…")
                                    enabled: EncoderController.captureSupported && !EncoderController.busy
                                    onClicked: {
                                        if (liveWriteCheck.checked) {
                                            window.openSaveDialog(liveSessionDialog,
                                                                  EncoderController.suggestedOutputName());
                                        } else {
                                            EncoderController.startLiveSession(
                                                deviceBox.currentIndex, liveMonitorCheck.checked,
                                                liveReceiverBox.currentIndex - 1, false, "");
                                        }
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }

                            Text {
                                visible: window.showExplanations
                                Layout.fillWidth: true
                                text: qsTr("Monitoring is free — nothing is written and no filename is asked for. The levels below are real. Record or start a session to commit a take.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.textMuted
                            }
                            Text {
                                objectName: "liveVbrWarning"
                                visible: EncoderController.vbrEnabled && EncoderController.vbrAvailable
                                Layout.fillWidth: true
                                text: qsTr("A live session always runs at the fixed bit rate — passthrough bursts are fixed-size, so frames cannot float. Variable rate applies to file encodes only.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.accent700
                            }
                            Text {
                                visible: !EncoderController.captureSupported
                                Layout.fillWidth: true
                                text: qsTr("No capture devices were found.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.accent700
                            }
                        }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

                    // ---- 02 / Levels -------------------------------------
                    RailBlock {
                        id: levelsBlock
                        ordinal: "02"
                        label: qsTr("LEVELS")
                        Layout.fillWidth: true
                        Layout.margins: Theme.space4

                        // Which meter rows the current mode shows, from the
                        // layout-keyed channelMeta — NEVER from channelLevels,
                        // whose 30 Hz churn must not rebuild delegates.
                        function rowVisible(meta) {
                            if (window.meterMode === "coded") {
                                return true;
                            }
                            if (meta.replaced === true) {
                                return false;
                            }
                            return EncoderController.atmosEnabled || meta.fed !== false;
                        }
                        readonly property int fedCount: {
                            const meta = EncoderController.channelMeta;
                            let fed = 0;
                            for (let i = 0; i < meta.length; i++) {
                                if (meta[i].fed !== false) fed++;
                            }
                            return fed;
                        }
                        readonly property int rowCount: EncoderController.channelMeta.length

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space2

                            Text {
                                text: EncoderController.hasLevels ? EncoderController.layoutName
                                                                  : EncoderController.channelShapeName
                                font.pixelSize: 20
                                font.family: Theme.headingFamily
                                font.weight: Font.ExtraBold
                                color: Theme.text
                            }
                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                visible: EncoderController.metering
                                color: Theme.accent
                            }
                            Text {
                                visible: EncoderController.metering
                                text: qsTr("live")
                                font.pixelSize: 11
                                color: Theme.accent700
                            }
                            Item { Layout.fillWidth: true }
                            SegmentedControl {
                                segHeight: 24
                                fontSize: 11
                                model: [
                                    { value: "coded", label: qsTr("Coded") },
                                    { value: "rendered", label: qsTr("Rendered") },
                                ]
                                currentValue: window.meterMode
                                onSelected: (value) => window.meterMode = value
                            }
                        }

                        // The dB scale, above the tracks: −60…0 mapped by the
                        // same meterFraction() the bars use.
                        Item {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 12
                            visible: EncoderController.hasLevels

                            // ChannelMeter's own row grid — 56 name + 6 gap on
                            // the left, 6 + 50 dB + 6 + 30 CLIP on the right —
                            // plus the delegate wrapper's 2px group rule and
                            // its 4px spacing before the meter starts.
                            readonly property real trackLeft: 2 + 4 + 56 + 6
                            readonly property real trackRight: 6 + 50 + 6 + 30
                            readonly property real trackWidth: width - trackLeft - trackRight

                            Repeater {
                                model: [-60, -48, -36, -24, -12, 0]
                                delegate: Text {
                                    required property var modelData
                                    x: parent.trackLeft
                                       + EncoderController.meterFraction(modelData) * parent.trackWidth
                                       - implicitWidth / 2
                                    text: String(modelData)
                                    font.pixelSize: 9
                                    font.family: Theme.monoFamily
                                    color: Theme.neutral500
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Repeater {
                                id: channelMeters
                                objectName: "channelMeters"
                                // channelMeta changes only when the LAYOUT
                                // does — the stable model the 30 Hz level
                                // stream never rebuilds.
                                model: EncoderController.channelMeta

                                delegate: RowLayout {
                                    id: meterRow
                                    required property var modelData
                                    required property int index
                                    visible: levelsBlock.rowVisible(modelData)
                                    Layout.fillWidth: true
                                    spacing: 4

                                    // Bed rows a dependent substream replaces
                                    // group behind a 2px accent rule in Coded
                                    // mode, so the duplication reads as
                                    // structure.
                                    Rectangle {
                                        Layout.preferredWidth: 2
                                        Layout.fillHeight: true
                                        color: meterRow.modelData.replaced === true
                                               ? Theme.accent300 : "transparent"
                                    }
                                    ChannelMeter {
                                        Layout.fillWidth: true
                                        channelName: meterRow.modelData.name
                                        fed: meterRow.modelData.fed !== false
                                        level: {
                                            const levels = EncoderController.channelLevels;
                                            return meterRow.index < levels.length
                                                   ? levels[meterRow.index] : ({});
                                        }
                                    }
                                }
                            }
                        }

                        // The meter footer — same fed set the dots count.
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: levelsBlock.fedCount < levelsBlock.rowCount ? 2 : 1
                            color: levelsBlock.fedCount < levelsBlock.rowCount ? Theme.accent : Theme.neutral300
                            visible: EncoderController.hasLevels
                        }
                        Text {
                            visible: EncoderController.hasLevels
                            Layout.fillWidth: true
                            text: {
                                const total = levelsBlock.rowCount;
                                const fed = levelsBlock.fedCount;
                                if (EncoderController.dualMono && !EncoderController.atmosEnabled) {
                                    return qsTr("Two independent programmes. The meters are not a pair — nothing here is correlated.");
                                }
                                if (window.meterMode === "rendered") {
                                    if (EncoderController.atmosEnabled) {
                                        return qsTr("All %1 speakers are driven — the bed carries the panned objects. Coded shows the channels as encoded.").arg(total);
                                    }
                                    if (fed < total) {
                                        return qsTr("%1 of %2 positions are driven. The rest are carried silent — switch to Coded to see them.").arg(fed).arg(total);
                                    }
                                    return qsTr("Every coded channel is driven — Coded and Rendered are the same here.");
                                }
                                if (EncoderController.atmosEnabled) {
                                    return qsTr("%1 of %2 bed positions fed — the rest of the audio rides as objects, not channels.").arg(fed).arg(total);
                                }
                                if (fed < total) {
                                    return qsTr("%1 of %2 coded channels fed by the assignments.").arg(fed).arg(total);
                                }
                                return qsTr("All %1 coded channels fed by the assignments.").arg(total);
                            }
                            wrapMode: Text.WordWrap
                            font.pixelSize: 11
                            color: levelsBlock.fedCount < levelsBlock.rowCount ? Theme.accent700 : Theme.neutral800
                        }

                        Text {
                            visible: !EncoderController.hasLevels
                            Layout.fillWidth: true
                            text: qsTr("Load a source, or start a live capture, and every coded channel gets a meter here.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.textMuted
                        }
                    }

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

                    // ---- 03 / Soundfield ---------------------------------
                    RailBlock {
                        ordinal: "03"
                        label: qsTr("SOUNDFIELD")
                        Layout.fillWidth: true
                        Layout.margins: Theme.space4

                        SoundfieldView {
                            visible: EncoderController.surround
                            Layout.fillWidth: true
                        }

                        // Dual mono has no soundstage to draw — two named
                        // programmes replace the plans.
                        ColumnLayout {
                            visible: EncoderController.dualMono && !EncoderController.atmosEnabled
                                     && EncoderController.hasLevels
                            Layout.fillWidth: true
                            spacing: Theme.space2

                            Repeater {
                                model: [qsTr("Programme 1"), qsTr("Programme 2")]
                                delegate: Rectangle {
                                    id: programmeCard
                                    required property string modelData
                                    Layout.fillWidth: true
                                    implicitHeight: 44
                                    color: Theme.neutral100
                                    border.color: Theme.divider
                                    border.width: 1

                                    ColumnLayout {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.leftMargin: Theme.space3
                                        spacing: 1
                                        Text {
                                            text: programmeCard.modelData
                                            font.pixelSize: 13
                                            font.weight: Font.DemiBold
                                            color: Theme.text
                                        }
                                        Text {
                                            text: qsTr("its own dialnorm and compression")
                                            font.pixelSize: 10
                                            color: Theme.textMuted
                                        }
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("No room to draw — dual mono has no soundstage. The listener's receiver plays one programme or the other.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.textMuted
                            }
                        }

                        Text {
                            visible: !EncoderController.surround
                                     && !(EncoderController.dualMono && EncoderController.hasLevels)
                            Layout.fillWidth: true
                            text: qsTr("Two or more full-bandwidth channels make a soundfield worth drawing.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.textMuted
                        }
                    }

                    Item { Layout.fillHeight: true }
                }
            }

            Rectangle { Layout.preferredWidth: 2; Layout.fillHeight: true; color: Theme.divider }

            // =============================================================
            // Right panel — the stream.
            // =============================================================
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                // ---- failure banner ---------------------------------------
                Rectangle {
                    Layout.fillWidth: true
                    visible: window.bannerRun !== null
                    color: Theme.accent100
                    implicitHeight: bannerColumn.implicitHeight + Theme.space3 * 2

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 2
                        color: Theme.accent
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.space3
                        spacing: Theme.space3

                        Text {
                            text: "⚠"
                            font.pixelSize: 18
                            color: Theme.accent700
                        }
                        ColumnLayout {
                            id: bannerColumn
                            Layout.fillWidth: true
                            spacing: 2

                            Text {
                                Layout.fillWidth: true
                                text: window.bannerRun !== null
                                      ? qsTr("Run %1 stopped — %2")
                                        .arg(window.bannerRun.id).arg(window.bannerRun.filename)
                                      : ""
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                            Text {
                                Layout.fillWidth: true
                                text: window.bannerRun !== null ? window.bannerRun.detail : ""
                                font.pixelSize: 12
                                color: Theme.neutral800
                                wrapMode: Text.WordWrap
                            }
                        }
                        Button {
                            text: qsTr("Dismiss")
                            flat: true
                            onClicked: {
                                window.dismissedRunId = window.bannerRun.id;
                                window.bannerRunId = -1;
                            }
                        }
                    }
                }

                // ---- back-to-guided strip ---------------------------------
                Rectangle {
                    Layout.fillWidth: true
                    visible: window.fromGuided && window.tier !== "guided"
                    color: Theme.neutral100
                    implicitHeight: 40

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.space4
                        anchors.rightMargin: Theme.space3
                        spacing: Theme.space3

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("You came here from the guided steps. Anything you change is kept when you go back.")
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            color: Theme.neutral800
                        }
                        Button {
                            objectName: "backToGuidedButton"
                            text: qsTr("Back to guided")
                            onClicked: {
                                window.fromGuided = false;
                                window.tier = "guided";
                            }
                        }
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: Theme.divider
                    visible: window.fromGuided && window.tier !== "guided"
                }

                // ---- plan strip -------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Layout.rightMargin: 24
                    Layout.topMargin: 14
                    Layout.bottomMargin: 12
                    spacing: Theme.space4

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            text: qsTr("THE STREAM")
                            font.pixelSize: 10
                            font.letterSpacing: 1.5
                            color: Theme.textMuted
                        }
                        Text {
                            Layout.fillWidth: true
                            text: window.planLine
                            font.pixelSize: 26
                            font.family: Theme.headingFamily
                            font.weight: Font.ExtraBold
                            elide: Text.ElideRight
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: window.planSubLine
                            font.pixelSize: 12
                            font.family: Theme.monoFamily
                            elide: Text.ElideRight
                            color: Theme.neutral700
                        }
                    }

                    ColumnLayout {
                        Layout.alignment: Qt.AlignTop
                        spacing: 2

                        Text {
                            text: qsTr("TOOLS")
                            font.pixelSize: 10
                            font.letterSpacing: 1.5
                            horizontalAlignment: Text.AlignRight
                            Layout.alignment: Qt.AlignRight
                            color: Theme.textMuted
                        }
                        Rectangle {
                            Layout.alignment: Qt.AlignRight
                            implicitWidth: toolsChip.implicitWidth + 14
                            implicitHeight: toolsChip.implicitHeight + 8
                            color: Theme.neutral200

                            Text {
                                id: toolsChip
                                anchors.centerIn: parent
                                text: {
                                    if (EncoderController.atmosEnabled) {
                                        return "joc+oamd";
                                    }
                                    const token = EncoderController.toolsToken;
                                    return token.length > 0 && token !== "none" ? token : "—";
                                }
                                font.pixelSize: 13
                                font.family: Theme.monoFamily
                                color: Theme.text
                            }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

                // ---- tab bar ----------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Layout.preferredHeight: 40
                    visible: window.tier !== "guided"
                    spacing: 28

                    Repeater {
                        model: window.visibleTabs

                        delegate: Item {
                            id: tabItem
                            required property var modelData
                            readonly property bool active: window.currentTab === modelData.key

                            objectName: "tab-" + modelData.key
                            implicitWidth: tabRow.implicitWidth
                            implicitHeight: 40

                            RowLayout {
                                id: tabRow
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Text {
                                    text: tabItem.modelData.label.toUpperCase()
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                    font.letterSpacing: 0.5
                                    color: Theme.text
                                    opacity: tabItem.active ? 1.0 : 0.55
                                }
                                Rectangle {
                                    visible: tabItem.modelData.badge.length > 0
                                    implicitWidth: Math.max(16, badgeText.implicitWidth + 8)
                                    implicitHeight: 14
                                    color: Theme.accent
                                    Text {
                                        id: badgeText
                                        anchors.centerIn: parent
                                        text: tabItem.modelData.badge
                                        font.pixelSize: 10
                                        font.family: Theme.monoFamily
                                        color: Theme.bg
                                    }
                                }
                            }
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 3
                                color: tabItem.active ? Theme.accent : "transparent"
                            }
                            MouseArea {
                                anchors.fill: parent
                                onClicked: window.currentTab = tabItem.modelData.key
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 2
                    color: Theme.divider
                    visible: window.tier !== "guided"
                }

                // ---- tab content ------------------------------------------
                ScrollView {
                    objectName: "tabScroll"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    contentWidth: availableWidth

                    StackLayout {
                        width: parent ? parent.width : 0
                        // Guided shows its wizard page (the last one) instead
                        // of whichever tab is current.
                        currentIndex: window.tier === "guided"
                                      ? window.tabOrder.length
                                      : window.tabOrder.indexOf(window.currentTab)

                        // =====================================================
                        // Format
                        // =====================================================
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.space4

                            // ---- presets + codec + rate + container --------
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Layout.topMargin: Theme.space4
                                spacing: Theme.space3

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space2

                                    Text {
                                        text: qsTr("PRESETS")
                                        font.pixelSize: 10
                                        font.letterSpacing: 1
                                        color: Theme.textMuted
                                    }
                                    Text {
                                        text: qsTr("starting points, not the model")
                                        font.pixelSize: 10
                                        font.family: Theme.monoFamily
                                        color: Theme.neutral500
                                    }
                                    Item { Layout.fillWidth: true }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space2

                                    Repeater {
                                        model: ["5.1", "7.1", "5.1.4", "7.1.4", "5.2", "7.2.4"]
                                        delegate: Button {
                                            required property string modelData
                                            objectName: "preset-" + modelData
                                            Layout.fillWidth: true
                                            text: modelData
                                            enabled: !EncoderController.busy && !EncoderController.atmosEnabled
                                            onClicked: EncoderController.applyChannelPreset(modelData)
                                        }
                                    }
                                }

                                GridLayout {
                                    Layout.fillWidth: true
                                    columns: 3
                                    columnSpacing: 20
                                    rowSpacing: 4

                                    // Whether anything is currently FORCING the
                                    // codec: extras and object mode both need
                                    // Dolby Digital Plus, and while they do the
                                    // field is a readout, not a control. A plain
                                    // bed genuinely encodes as either, so there
                                    // the choice is real and stays offered.
                                    readonly property bool codecForced: {
                                        if (EncoderController.atmosEnabled) return true;
                                        const extras = EncoderController.extrasModel;
                                        for (let i = 0; i < extras.length; i++) {
                                            if (extras[i].checked) return true;
                                        }
                                        return false;
                                    }
                                    id: formatGrid

                                    Text {
                                        text: EncoderController.atmosEnabled
                                              ? qsTr("Codec — fixed by object mode")
                                              : formatGrid.codecForced
                                                ? qsTr("Codec — follows the channels")
                                                : qsTr("Codec")
                                        font.pixelSize: 11
                                        color: Theme.textMuted
                                    }
                                    Text {
                                        text: qsTr("Bit rate")
                                        font.pixelSize: 11
                                        color: Theme.textMuted
                                    }
                                    Text {
                                        text: qsTr("Container")
                                        font.pixelSize: 11
                                        color: Theme.textMuted
                                    }

                                    // The codec follows the channels: any extra
                                    // promotes the stream to DD+ and locks this
                                    // to the derived value. With nothing forcing
                                    // it, a plain bed is a real either/or.
                                    ComboBox {
                                        Layout.fillWidth: true
                                        enabled: !formatGrid.codecForced && !EncoderController.busy
                                        model: EncoderController.codecNames
                                        currentIndex: EncoderController.codecIndex
                                        onActivated: EncoderController.codecIndex = currentIndex
                                    }
                                    ComboBox {
                                        id: bitrateBox
                                        Layout.fillWidth: true
                                        enabled: !EncoderController.busy
                                        model: EncoderController.bitrates
                                        displayText: qsTr("%1 kbps").arg(EncoderController.bitrateKbps)
                                        delegate: ItemDelegate {
                                            required property var modelData
                                            width: bitrateBox.width
                                            text: qsTr("%1 kbps").arg(modelData)
                                            onClicked: {
                                                EncoderController.bitrateKbps = modelData;
                                                bitrateBox.popup.close();
                                            }
                                        }
                                    }
                                    ComboBox {
                                        Layout.fillWidth: true
                                        enabled: !EncoderController.busy
                                        model: EncoderController.containerNames
                                        currentIndex: EncoderController.containerIndex
                                        onActivated: EncoderController.containerIndex = currentIndex
                                    }
                                }

                                VbrPanel {
                                    Layout.fillWidth: true
                                    showExplanations: window.showExplanations
                                }
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

                            // ---- the two-tier channel picker ----------------
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                spacing: Theme.space3

                                RowLayout {
                                    Layout.fillWidth: true

                                    Text {
                                        text: qsTr("CHANNELS — THE TWO-TIER PICKER")
                                        font.pixelSize: 10
                                        font.letterSpacing: 1
                                        color: Theme.textMuted
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: qsTr("%1 of %2 positions used · %3")
                                              .arg(EncoderController.channelBudgetUsed)
                                              .arg(EncoderController.channelBudgetMax)
                                              .arg(EncoderController.channelShapeName)
                                        font.pixelSize: 11
                                        font.family: Theme.monoFamily
                                        color: Theme.neutral700
                                    }
                                }

                                Text {
                                    text: qsTr("Bed — pick one")
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    color: Theme.text
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space2

                                    Repeater {
                                        model: EncoderController.bedChoices
                                        delegate: Rectangle {
                                            id: bedButton
                                            required property var modelData
                                            required property int index
                                            readonly property bool active: EncoderController.bedIndex === index
                                            readonly property bool dual: modelData.id === "1+1"
                                            readonly property bool locked: EncoderController.atmosEnabled
                                                                           || EncoderController.busy

                                            objectName: "bed-" + modelData.id
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 40
                                            color: active ? Theme.text : "transparent"
                                            border.color: active ? Theme.text
                                                                 : (dual ? Theme.neutral500 : Theme.divider)
                                            border.width: 1
                                            opacity: locked && !active ? 0.25 : 1.0

                                            ColumnLayout {
                                                anchors.centerIn: parent
                                                spacing: 0
                                                Text {
                                                    Layout.alignment: Qt.AlignHCenter
                                                    text: bedButton.dual
                                                          ? qsTr("1+1 · dual") : bedButton.modelData.id
                                                    font.pixelSize: 12
                                                    font.family: Theme.monoFamily
                                                    font.weight: Font.DemiBold
                                                    color: bedButton.active ? Theme.bg : Theme.text
                                                }
                                                Text {
                                                    Layout.alignment: Qt.AlignHCenter
                                                    text: bedButton.dual
                                                          ? qsTr("2 progs") : bedButton.modelData.channels
                                                    font.pixelSize: 9
                                                    font.family: Theme.monoFamily
                                                    color: bedButton.active ? Theme.bg : Theme.neutral600
                                                    elide: Text.ElideRight
                                                }
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                enabled: !bedButton.locked
                                                onClicked: EncoderController.bedIndex = bedButton.index
                                            }
                                        }
                                    }
                                }
                                Text {
                                    objectName: "noteBedAlways"
                                    visible: window.showExplanations
                                    Layout.fillWidth: true
                                    text: qsTr("One bed, always. Extras add to it — the format cannot carry a ceiling channel, or any other, without a bed underneath.")
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                    color: Theme.textMuted
                                }

                                // ---- low frequency: a count, not a flag ----
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space3

                                    Text {
                                        text: qsTr("Low frequency")
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                        color: Theme.text
                                    }
                                    Text {
                                        visible: EncoderController.bedLfeLocked
                                        text: EncoderController.dualMono
                                              ? qsTr("not part of dual mono") : qsTr("fixed by object mode")
                                        font.pixelSize: 11
                                        color: Theme.textMuted
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space2

                                    readonly property bool lfe2On: {
                                        const extras = EncoderController.extrasModel;
                                        for (let i = 0; i < extras.length; i++) {
                                            if (extras[i].id === "lfe2") return extras[i].checked;
                                        }
                                        return false;
                                    }
                                    readonly property int lfeCount: !EncoderController.bedLfe
                                                                    ? 0 : (lfe2On ? 2 : 1)
                                    id: lfeRow

                                    function setCount(n) {
                                        if (EncoderController.bedLfeLocked || EncoderController.busy) {
                                            return;
                                        }
                                        if (lfeRow.lfe2On !== (n > 1)) {
                                            EncoderController.toggleExtra("lfe2");
                                        }
                                        EncoderController.bedLfe = n > 0;
                                    }

                                    Repeater {
                                        model: [
                                            { n: 0, label: qsTr("None") },
                                            { n: 1, label: qsTr("One · LFE") },
                                            { n: 2, label: qsTr("Two · LFE + LFE2") },
                                        ]
                                        delegate: Rectangle {
                                            id: lfeButton
                                            required property var modelData
                                            readonly property bool active: lfeRow.lfeCount === modelData.n
                                            readonly property bool locked: EncoderController.bedLfeLocked
                                                                           || EncoderController.busy
                                                                           || (modelData.n === 2 && EncoderController.extrasLocked)

                                            objectName: "lfeCount-" + modelData.n
                                            Layout.preferredWidth: 130
                                            Layout.preferredHeight: 32
                                            color: active ? Theme.text : "transparent"
                                            border.color: active ? Theme.text : Theme.divider
                                            border.width: 1
                                            opacity: locked && !active ? 0.3 : 1.0

                                            Text {
                                                anchors.centerIn: parent
                                                text: lfeButton.modelData.label
                                                font.pixelSize: 11
                                                font.family: Theme.monoFamily
                                                color: lfeButton.active ? Theme.bg : Theme.text
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                enabled: !lfeButton.locked
                                                onClicked: {
                                                    const n = lfeButton.modelData.n;
                                                    // A second LFE is an extra, so it
                                                    // promotes the codec like one.
                                                    window.withCodecWarning(n === 2 && !lfeRow.lfe2On,
                                                        () => lfeRow.setCount(n));
                                                }
                                            }
                                        }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                                Text {
                                    visible: window.showExplanations
                                    Layout.fillWidth: true
                                    text: qsTr("Two means two independent low-frequency channels carrying different signal — not one signal sent to two subwoofers. This is what makes a 7.2.4 rather than a 7.1.4.")
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                    color: Theme.textMuted
                                }

                                // ---- extras --------------------------------
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text {
                                        text: qsTr("Extras — added to the bed")
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                        color: Theme.text
                                    }
                                    Item { Layout.fillWidth: true }
                                    Text {
                                        text: qsTr("pairs toggle together")
                                        font.pixelSize: 10
                                        font.family: Theme.monoFamily
                                        color: Theme.neutral500
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Repeater {
                                        model: EncoderController.extrasModel

                                        delegate: ColumnLayout {
                                            id: extraRow
                                            required property var modelData
                                            visible: modelData.id !== "lfe2"
                                            Layout.fillWidth: true
                                            spacing: 0

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Layout.topMargin: 6
                                                Layout.bottomMargin: 6
                                                spacing: Theme.space3
                                                opacity: extraRow.modelData.enabled ? 1.0 : 0.4

                                                CheckBox {
                                                    objectName: "extra-" + extraRow.modelData.id
                                                    checked: extraRow.modelData.checked
                                                    enabled: extraRow.modelData.enabled && !EncoderController.busy
                                                    onToggled: {
                                                        const id = extraRow.modelData.id;
                                                        const promotes = !extraRow.modelData.checked;
                                                        window.withCodecWarning(promotes,
                                                            () => EncoderController.toggleExtra(id));
                                                    }
                                                }
                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 0
                                                    Text {
                                                        text: extraRow.modelData.label
                                                        font.pixelSize: 13
                                                        font.weight: Font.DemiBold
                                                        color: Theme.text
                                                    }
                                                    Text {
                                                        text: qsTr("%1 channels").arg(extraRow.modelData.channels)
                                                        font.pixelSize: 11
                                                        color: Theme.textMuted
                                                    }
                                                }
                                                Text {
                                                    text: {
                                                        if (extraRow.modelData.reason.length > 0) {
                                                            return extraRow.modelData.reason;
                                                        }
                                                        if (!extraRow.modelData.checked
                                                            && EncoderController.codecIndex === 0
                                                            && !EncoderController.atmosEnabled
                                                            && !EncoderController.dualMono) {
                                                            return qsTr("moves to Dolby Digital Plus");
                                                        }
                                                        return "";
                                                    }
                                                    font.pixelSize: 11
                                                    color: Theme.textMuted
                                                }
                                            }
                                            Rectangle {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 1
                                                color: Theme.neutral200
                                            }
                                        }
                                    }
                                }

                                Text {
                                    visible: window.showExplanations
                                    Layout.fillWidth: true
                                    text: {
                                        if (EncoderController.dualMono && !EncoderController.atmosEnabled) {
                                            return qsTr("Dual mono is not a layout — it is two programmes. Extras, the LFE and objects do not apply, and the assignments below choose which sound is which programme.");
                                        }
                                        if (EncoderController.atmosEnabled) {
                                            return qsTr("Object mode fixes the bed at 5.1. The positions above describe the bed, not the objects.");
                                        }
                                        if (EncoderController.codecIndex === 1) {
                                            return qsTr("Anything past a bed and its LFE needs Dolby Digital Plus, so the codec has followed the channels — up to sixteen rendered locations, including a second, independent LFE.");
                                        }
                                        return qsTr("A bed with or without an LFE is Dolby Digital, capped at 5.1. Adding any extra — rear, ceiling or a second LFE — moves the stream to Dolby Digital Plus.");
                                    }
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                    color: Theme.textMuted
                                }
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

                            // ---- routing -------------------------------------
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                spacing: Theme.space3

                                Text {
                                    text: qsTr("ROUTING")
                                    font.pixelSize: 10
                                    font.letterSpacing: 1
                                    color: Theme.textMuted
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 56
                                        color: Theme.neutral100

                                        ColumnLayout {
                                            anchors.centerIn: parent
                                            spacing: 1
                                            Text {
                                                Layout.alignment: Qt.AlignHCenter
                                                text: qsTr("SOURCE")
                                                font.pixelSize: 10
                                                font.letterSpacing: 1
                                                color: Theme.textMuted
                                            }
                                            Text {
                                                Layout.alignment: Qt.AlignHCenter
                                                text: {
                                                    const sources = EncoderController.sourceModel;
                                                    if (sources.length === 0) return qsTr("nothing");
                                                    let channels = 0;
                                                    for (let i = 0; i < sources.length; i++) channels += sources[i].channels;
                                                    if (EncoderController.dualMono && !EncoderController.atmosEnabled) {
                                                        return qsTr("2 mono programmes");
                                                    }
                                                    return sources.length === 1
                                                           ? qsTr("1 source · %1 ch").arg(channels)
                                                           : qsTr("%1 sources · %2 ch").arg(sources.length).arg(channels);
                                                }
                                                font.pixelSize: 19
                                                font.family: Theme.headingFamily
                                                font.weight: Font.ExtraBold
                                                color: Theme.text
                                            }
                                        }
                                    }
                                    Text {
                                        Layout.leftMargin: Theme.space3
                                        Layout.rightMargin: Theme.space3
                                        text: "→"
                                        font.pixelSize: 20
                                        color: Theme.neutral500
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 56
                                        color: Theme.neutral100

                                        ColumnLayout {
                                            anchors.centerIn: parent
                                            spacing: 1
                                            Text {
                                                Layout.alignment: Qt.AlignHCenter
                                                text: qsTr("CODED")
                                                font.pixelSize: 10
                                                font.letterSpacing: 1
                                                color: Theme.textMuted
                                            }
                                            Text {
                                                Layout.alignment: Qt.AlignHCenter
                                                text: {
                                                    if (EncoderController.dualMono && !EncoderController.atmosEnabled) {
                                                        return qsTr("2 programmes");
                                                    }
                                                    if (EncoderController.atmosEnabled) {
                                                        return qsTr("%1 objects + 5.1 bed").arg(EncoderController.objectCount);
                                                    }
                                                    return EncoderController.channelShapeName;
                                                }
                                                font.pixelSize: 19
                                                font.family: Theme.headingFamily
                                                font.weight: Font.ExtraBold
                                                color: Theme.text
                                            }
                                        }
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: EncoderController.routingSummary
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 12
                                    color: Theme.neutral800
                                }

                                // The channel map: one tag per coded position,
                                // filled when a source feeds it, outlined when
                                // it is carried silent.
                                Flow {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    visible: !EncoderController.dualMono || EncoderController.atmosEnabled

                                    Repeater {
                                        model: EncoderController.plannedChannels

                                        delegate: Rectangle {
                                            required property var modelData
                                            visible: modelData.replaced !== true
                                            width: chipText.implicitWidth + 12
                                            height: 20
                                            color: modelData.fed !== false ? Theme.neutral800 : "transparent"
                                            border.color: modelData.fed !== false ? Theme.neutral800 : Theme.neutral400
                                            border.width: 1

                                            Text {
                                                id: chipText
                                                anchors.centerIn: parent
                                                text: parent.modelData.token
                                                font.pixelSize: 10
                                                font.family: Theme.monoFamily
                                                color: parent.modelData.fed !== false ? Theme.bg : Theme.neutral600
                                            }
                                        }
                                    }
                                }
                                Text {
                                    visible: !EncoderController.dualMono || EncoderController.atmosEnabled
                                    text: qsTr("Filled = fed by a source. Outlined = carried silent.")
                                    font.pixelSize: 10
                                    font.family: Theme.monoFamily
                                    color: Theme.neutral500
                                }
                            }

                            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

                            // ---- assignments ---------------------------------
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                spacing: Theme.space3

                                Text {
                                    text: qsTr("ASSIGNMENTS — EVERY SOURCE CHANNEL GOES SOMEWHERE, OR NOWHERE ON PURPOSE")
                                    font.pixelSize: 10
                                    font.letterSpacing: 1
                                    color: Theme.textMuted
                                }

                                AssignmentPanel {
                                    Layout.fillWidth: true
                                    showExplanations: window.showExplanations
                                }

                                Text {
                                    visible: window.showExplanations
                                    Layout.fillWidth: true
                                    text: qsTr("A stereo file cannot be one object — an object is a single point in the room. Send each channel to its own object, or put the pair on bed channels.")
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                    color: Theme.textMuted
                                }
                            }

                            // ---- loudness (Advanced only — Expert has it on
                            // the Metadata tab instead, so it appears once) ----
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 2
                                color: Theme.divider
                                visible: window.tier === "advanced"
                            }
                            ColumnLayout {
                                visible: window.tier === "advanced"
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                spacing: Theme.space3

                                Text {
                                    text: qsTr("LOUDNESS")
                                    font.pixelSize: 10
                                    font.letterSpacing: 1
                                    color: Theme.textMuted
                                }
                                LoudnessGroup { Layout.fillWidth: true }
                                Text {
                                    text: qsTr("Coding tools and broadcast metadata →")
                                    font.pixelSize: 12
                                    color: Theme.accent700
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: window.tier = "expert"
                                    }
                                }
                            }

                            // ---- passthrough ---------------------------------
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 2
                                color: Theme.divider
                                visible: window.tier !== "guided"
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Layout.bottomMargin: Theme.space4
                                spacing: Theme.space3

                                Text {
                                    text: qsTr("PASSTHROUGH TO A RECEIVER")
                                    font.pixelSize: 10
                                    font.letterSpacing: 1
                                    color: Theme.textMuted
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space2

                                    ComboBox {
                                        id: outputBox
                                        Layout.fillWidth: true
                                        enabled: !EncoderController.busy
                                        model: EncoderController.outputDevices
                                    }
                                    Button {
                                        text: qsTr("Refresh")
                                        enabled: !EncoderController.busy
                                        onClicked: EncoderController.refreshOutputDevices()
                                    }
                                    Button {
                                        text: EncoderController.playing ? qsTr("Playing…") : qsTr("Play")
                                        enabled: EncoderController.canPlay && !EncoderController.busy
                                                 && !EncoderController.playing
                                        onClicked: EncoderController.playToReceiver(outputBox.currentIndex)
                                    }
                                }
                                Text {
                                    visible: window.tier === "expert" && window.showExplanations
                                    Layout.fillWidth: true
                                    text: qsTr("Sends the encoded stream as IEC 61937 bursts in exclusive mode, so the receiver decodes it. The packer emits AC-3 bursts only (data type 1), so an E-AC-3 stream is refused here rather than sent as something it is not. Only S/PDIF and HDMI endpoints can bitstream at all.")
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                    color: Theme.textMuted
                                }
                            }
                        }

                        // =====================================================
                        // Coding tools (Expert only)
                        // =====================================================
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            Card {
                                Layout.fillWidth: true
                                Layout.margins: 24
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

                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("ac3cli tools token:  %1").arg(EncoderController.toolsToken)
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.fontSmall
                                    font.family: Theme.monoFamily
                                }
                            }

                            Item { Layout.fillHeight: true }
                        }

                        // =====================================================
                        // Metadata (Expert only)
                        // =====================================================
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignTop
                            spacing: 40

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignTop
                                Layout.leftMargin: 24
                                Layout.topMargin: Theme.space4
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
                                Layout.rightMargin: 24
                                Layout.topMargin: Theme.space4
                                spacing: Theme.gap

                                Card {
                                    title: qsTr("Heavy compression")

                                    CheckBox {
                                        text: qsTr("Heavy compression")
                                        enabled: !EncoderController.busy
                                        checked: EncoderController.heavy
                                        onToggled: EncoderController.heavy = checked
                                    }

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

                        // =====================================================
                        // Objects
                        // =====================================================
                        ColumnLayout {
                            id: objectsTab
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            property string driveMode: "author"
                            property real playheadTime: 0
                            property bool previewing: false
                            // objectKeyframes()/evaluateObjectPath() are
                            // Q_INVOKABLEs, not properties; reading this
                            // counter inside those bindings gives them a
                            // dependency to re-evaluate on.
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

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.margins: 24
                                spacing: Theme.space3

                                // ---- header: switch + summary + rate warning
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.gap

                                    Switch {
                                        id: atmosSwitch
                                        objectName: "atmosSwitch"
                                        text: qsTr("Encode as Dolby Atmos objects")
                                        enabled: !EncoderController.busy
                                        checked: EncoderController.atmosEnabled
                                        onToggled: EncoderController.atmosEnabled = checked
                                    }

                                    Text {
                                        Layout.fillWidth: true
                                        // preferredWidth 1 lets the row SHRINK this
                                        // text below its implicit width and elide,
                                        // instead of pushing the row past the panel.
                                        Layout.preferredWidth: 1
                                        text: EncoderController.atmosEnabled
                                              ? qsTr("%1 objects from the assignments · E-AC-3 over a 5.1 bed · positions ride as OAMD")
                                                .arg(EncoderController.objectCount)
                                              : qsTr("Off — the stream is a plain channel bed. Turning this on fixes the codec to E-AC-3 over 5.1.")
                                        elide: Text.ElideRight
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.fontSmall
                                    }

                                    RowLayout {
                                        visible: EncoderController.atmosEnabled
                                                 && EncoderController.bitrateKbps < 384
                                        spacing: Theme.space2

                                        Rectangle {
                                            implicitWidth: rateWarn.implicitWidth + 14
                                            implicitHeight: rateWarn.implicitHeight + 8
                                            color: Theme.accent100
                                            Text {
                                                id: rateWarn
                                                anchors.centerIn: parent
                                                text: qsTr("Objects over a 5.1 bed want 384 kbps or better")
                                                color: Theme.accent700
                                                font.pixelSize: Theme.fontSmall
                                            }
                                        }
                                        Button {
                                            text: qsTr("Set it")
                                            enabled: !EncoderController.busy
                                            onClicked: EncoderController.bitrateKbps = 384
                                        }
                                    }
                                }

                                // ---- empty state ---------------------------
                                ColumnLayout {
                                    visible: EncoderController.atmosEnabled
                                             && EncoderController.objectCount === 0
                                    Layout.fillWidth: true
                                    spacing: Theme.space2

                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("Nothing is an object yet. Objects come from the assignments — send a sound to \"an object\" and it appears here with a place in the room.")
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 13
                                        color: Theme.text
                                    }
                                    Button {
                                        text: qsTr("Open assignments")
                                        onClicked: window.goAssign()
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: EncoderController.atmosEnabled
                                             && EncoderController.objectCount > 0
                                    spacing: Theme.space6

                                    // ---- room: plan + elevation -------------
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
                                                font.family: Theme.monoFamily
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
                                                // A stable int model: the drag
                                                // stream must not rebuild these
                                                // delegates, only move them.
                                                model: EncoderController.objectCount

                                                Rectangle {
                                                    id: marker
                                                    required property int index
                                                    readonly property var obj: {
                                                        const list = EncoderController.objectModel;
                                                        return index < list.length ? list[index] : null;
                                                    }
                                                    readonly property bool isSelected:
                                                        index === EncoderController.selectedObjectIndex
                                                    readonly property var livePos:
                                                        objectsTab.previewing && obj !== null
                                                        ? EncoderController.evaluateObjectPath(
                                                              index, objectsTab.playheadTime)
                                                        : null

                                                    visible: obj !== null
                                                    width: isSelected ? 18 : 14
                                                    height: isSelected ? 18 : 14
                                                    color: isSelected ? Theme.accent : Theme.neutral800
                                                    border.color: Theme.text
                                                    border.width: isSelected ? 2 : 0
                                                    x: (livePos ? livePos.x : (obj ? obj.x : 0.5)) * room.width - width / 2
                                                    y: (livePos ? livePos.y : (obj ? obj.y : 0.5)) * room.height - height / 2
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
                                                            text: qsTr("obj %1").arg(marker.index + 1)
                                                            color: Theme.text
                                                            font.pixelSize: 10
                                                            font.family: Theme.monoFamily
                                                        }
                                                    }

                                                    MouseArea {
                                                        anchors.fill: parent
                                                        onClicked: EncoderController.selectedObjectIndex = marker.index
                                                    }
                                                }
                                            }
                                        }

                                        // ---- elevation: drag for height ------
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.topMargin: Theme.space2
                                            Text {
                                                text: qsTr("ROOM — ELEVATION")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: qsTr("drag for height")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                                font.family: Theme.monoFamily
                                            }
                                        }
                                        Rectangle {
                                            id: elevation
                                            Layout.preferredWidth: 340
                                            Layout.preferredHeight: 150
                                            color: Theme.neutral100
                                            border.color: Theme.divider
                                            border.width: 1

                                            // z +1 (ceiling) at the top line,
                                            // 0 (ear level) at 62%, −1 (floor)
                                            // at the bottom.
                                            function zToY(z) {
                                                return 14 + (1 - (z + 1) / 2) * (height - 24);
                                            }
                                            function yToZ(y) {
                                                return Math.max(-1, Math.min(1, 1 - 2 * ((y - 14) / (height - 24))));
                                            }

                                            Rectangle {
                                                x: 0; width: parent.width
                                                y: 14; height: 1
                                                color: Theme.neutral300
                                            }
                                            Text {
                                                x: 4; y: 2
                                                text: qsTr("ceiling")
                                                color: Theme.neutral500
                                                font.pixelSize: 9
                                            }
                                            Rectangle {
                                                x: 0; width: parent.width
                                                y: elevation.zToY(0); height: 1
                                                color: Theme.neutral300
                                            }
                                            Text {
                                                x: 4; y: elevation.zToY(0) - 12
                                                text: qsTr("ear level")
                                                color: Theme.neutral500
                                                font.pixelSize: 9
                                            }
                                            Text {
                                                x: 4; y: parent.height - 13
                                                text: qsTr("floor")
                                                color: Theme.neutral500
                                                font.pixelSize: 9
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                enabled: !EncoderController.busy
                                                         && objectsTab.driveMode === "author"
                                                         && objectsTab.selectedObj !== null
                                                onPositionChanged: (mouse) => place(mouse)
                                                onPressed: (mouse) => place(mouse)
                                                function place(mouse) {
                                                    const x = Math.max(0, Math.min(1, mouse.x / elevation.width));
                                                    EncoderController.setObjectPosition(
                                                        objectsTab.selectedObj.index, x,
                                                        objectsTab.selectedObj.y,
                                                        elevation.yToZ(mouse.y));
                                                }
                                            }

                                            // The selected object's marker with
                                            // a drop line to ear level.
                                            Rectangle {
                                                visible: objectsTab.selectedObj !== null
                                                readonly property real markerX:
                                                    (objectsTab.selectedObj ? objectsTab.selectedObj.x : 0.5) * elevation.width
                                                readonly property real markerY:
                                                    elevation.zToY(objectsTab.selectedObj ? objectsTab.selectedObj.z : 0)
                                                x: markerX - 1
                                                y: Math.min(markerY, elevation.zToY(0))
                                                width: 2
                                                height: Math.abs(elevation.zToY(0) - markerY)
                                                color: Theme.accent300
                                            }
                                            Rectangle {
                                                visible: objectsTab.selectedObj !== null
                                                width: 14
                                                height: 14
                                                color: Theme.accent
                                                border.color: Theme.text
                                                border.width: 2
                                                x: (objectsTab.selectedObj ? objectsTab.selectedObj.x : 0.5) * elevation.width - width / 2
                                                y: elevation.zToY(objectsTab.selectedObj ? objectsTab.selectedObj.z : 0) - height / 2
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
                                                        font.family: Theme.monoFamily
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // ---- object list + LFE send --------------
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

                                        GridLayout {
                                            Layout.fillWidth: true
                                            columns: 8
                                            columnSpacing: Theme.space2
                                            rowSpacing: 2

                                            Repeater {
                                                model: [
                                                    qsTr("Object"), qsTr("Sound"), qsTr("x"), qsTr("y"),
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
                                                model: EncoderController.objectCount

                                                Rectangle {
                                                    id: objRow
                                                    required property int index
                                                    readonly property var obj: {
                                                        const list = EncoderController.objectModel;
                                                        return index < list.length ? list[index] : null;
                                                    }
                                                    Layout.columnSpan: 8
                                                    Layout.fillWidth: true
                                                    implicitHeight: rowLayout.implicitHeight + 6
                                                    visible: obj !== null
                                                    color: index === EncoderController.selectedObjectIndex
                                                           ? Theme.accent100 : "transparent"

                                                    RowLayout {
                                                        id: rowLayout
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        width: parent.width
                                                        spacing: Theme.space2

                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: objRow.index + 1
                                                            font.family: Theme.monoFamily
                                                            font.pixelSize: Theme.fontSmall
                                                            color: Theme.text
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: objRow.obj ? objRow.obj.sourceLabel : ""
                                                            font.pixelSize: Theme.fontSmall
                                                            elide: Text.ElideMiddle
                                                            color: Theme.text
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: objRow.obj ? objRow.obj.x.toFixed(2) : ""
                                                            font.family: Theme.monoFamily
                                                            font.pixelSize: Theme.fontSmall
                                                            color: Theme.text
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: objRow.obj ? objRow.obj.y.toFixed(2) : ""
                                                            font.family: Theme.monoFamily
                                                            font.pixelSize: Theme.fontSmall
                                                            color: Theme.text
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: objRow.obj ? objRow.obj.z.toFixed(2) : ""
                                                            font.family: Theme.monoFamily
                                                            font.pixelSize: Theme.fontSmall
                                                            color: Theme.text
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: objRow.obj && objRow.obj.hasPath ? qsTr("path") : qsTr("static")
                                                            font.pixelSize: Theme.fontSmall
                                                            color: objRow.obj && objRow.obj.hasPath ? Theme.text : Theme.textMuted
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: objRow.obj ? objRow.obj.lfeSend.toFixed(2) : ""
                                                            font.family: Theme.monoFamily
                                                            font.pixelSize: Theme.fontSmall
                                                            color: Theme.text
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            text: objRow.obj ? objRow.obj.keyCount : ""
                                                            font.family: Theme.monoFamily
                                                            font.pixelSize: Theme.fontSmall
                                                            color: Theme.text
                                                        }
                                                    }

                                                    MouseArea {
                                                        anchors.fill: parent
                                                        onClicked: EncoderController.selectedObjectIndex = objRow.index
                                                    }
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: Theme.space2

                                            Button {
                                                text: qsTr("Add an object")
                                                flat: true
                                                onClicked: window.goAssign()
                                            }
                                            Button {
                                                text: qsTr("Change what feeds them →")
                                                flat: true
                                                onClicked: window.goAssign()
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: qsTr("%1 of 16 objects · each one is a sound with a place")
                                                      .arg(EncoderController.objectCount)
                                                font.pixelSize: 10
                                                font.family: Theme.monoFamily
                                                color: Theme.neutral600
                                            }
                                        }

                                        // ---- LFE send ---------------------------
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            Layout.topMargin: Theme.space3
                                            spacing: Theme.space2

                                            RowLayout {
                                                Layout.fillWidth: true
                                                Text {
                                                    text: qsTr("LFE send — object %1")
                                                          .arg((objectsTab.selectedObj
                                                                ? objectsTab.selectedObj.index : 0) + 1)
                                                    color: Theme.neutral600
                                                    font.pixelSize: 10
                                                }
                                                Item { Layout.fillWidth: true }
                                                Text {
                                                    text: (objectsTab.selectedObj
                                                           ? objectsTab.selectedObj.lfeSend : 0).toFixed(2)
                                                    color: Theme.text
                                                    font.pixelSize: 11
                                                    font.family: Theme.monoFamily
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
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Text { text: "0.00"; font.pixelSize: 9; font.family: Theme.monoFamily; color: Theme.neutral500 }
                                                Item { Layout.fillWidth: true }
                                                Text { text: "1.00"; font.pixelSize: 9; font.family: Theme.monoFamily; color: Theme.neutral500 }
                                            }
                                        }

                                        Text {
                                            visible: window.showExplanations
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
                                             && EncoderController.objectCount > 0
                                    height: 2
                                    color: Theme.divider
                                }

                                // ---- motion timeline ------------------------
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    visible: EncoderController.atmosEnabled
                                             && EncoderController.objectCount > 0
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
                                            font.family: Theme.monoFamily
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
                                                        font.family: Theme.monoFamily
                                                    }
                                                }
                                            }

                                            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

                                            Repeater {
                                                model: EncoderController.objectCount

                                                RowLayout {
                                                    id: laneRow
                                                    required property int index
                                                    Layout.fillWidth: true
                                                    spacing: 0

                                                    Text {
                                                        Layout.preferredWidth: 70
                                                        Layout.leftMargin: 8
                                                        text: qsTr("obj %1").arg(laneRow.index + 1)
                                                        color: laneRow.index === EncoderController.selectedObjectIndex
                                                               ? Theme.text : Theme.neutral700
                                                        font.pixelSize: 10
                                                        font.family: Theme.monoFamily
                                                    }

                                                    Rectangle {
                                                        id: lane
                                                        Layout.fillWidth: true
                                                        Layout.preferredHeight: 24
                                                        readonly property bool isSelected:
                                                            laneRow.index === EncoderController.selectedObjectIndex
                                                        readonly property var keys:
                                                            (objectsTab.objectsRevision,
                                                             EncoderController.objectKeyframes(laneRow.index))
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

                        // =====================================================
                        // Live session
                        // =====================================================
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                Layout.topMargin: Theme.space4
                                visible: EncoderController.liveReconnecting
                                color: Theme.accent100
                                implicitHeight: reconnectMsg.implicitHeight + Theme.space3 * 2

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 2
                                    color: Theme.accent
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: Theme.space3
                                    spacing: Theme.space3

                                    Text {
                                        id: reconnectMsg
                                        Layout.fillWidth: true
                                        Layout.preferredWidth: 1
                                        text: qsTr("Renegotiating with the receiver. It is re-locking to the new bitstream format — expect a second of silence. This is normal AVR behaviour on a format change.")
                                        color: Theme.accent800
                                        font.pixelSize: Theme.fontSmall
                                        wrapMode: Text.WordWrap
                                    }
                                    Button {
                                        objectName: "reconnectSkip"
                                        text: qsTr("Skip")
                                        flat: true
                                        onClicked: EncoderController.settleReconnect()
                                    }
                                }
                            }

                            Card {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
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
                                            font.family: Theme.monoFamily
                                        }
                                    }
                                    ColumnLayout {
                                        spacing: 2
                                        Text { text: qsTr("FRAMES"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            text: EncoderController.liveFramesEncoded
                                            color: Theme.text
                                            font.pixelSize: 15
                                            font.family: Theme.monoFamily
                                        }
                                    }
                                    ColumnLayout {
                                        spacing: 2
                                        Text { text: qsTr("DROPPED"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            text: EncoderController.liveFramesDropped
                                            color: EncoderController.liveFramesDropped > 0 ? Theme.accent700 : Theme.text
                                            font.pixelSize: 15
                                            font.family: Theme.monoFamily
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
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                title: qsTr("Chain")

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 0

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Text { text: qsTr("CAPTURE"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            Layout.fillWidth: true
                                            text: deviceBox.currentText.length > 0
                                                  ? deviceBox.currentText : qsTr("Capture device")
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
                                        Text {
                                            text: qsTr("meters and soundfield follow this")
                                            color: Theme.textMuted
                                            font.pixelSize: 10
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
                                        Text { text: qsTr("RECEIVER LEG — IEC 61937"); color: Theme.neutral600; font.pixelSize: 10 }
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
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                visible: EncoderController.liveGap
                                color: Theme.accent100
                                implicitHeight: gapMsg.implicitHeight + Theme.space3 * 2

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 2
                                    color: Theme.accent
                                }
                                Text {
                                    id: gapMsg
                                    anchors.fill: parent
                                    anchors.margins: Theme.space3
                                    text: qsTr("Everything past what the receiver leg carries — the extra channels, every object move — is visible on the meters and the soundfield but not audible on the amplifier, until Dolby Digital Plus passthrough lands.")
                                    color: Theme.accent800
                                    font.pixelSize: Theme.fontSmall
                                    wrapMode: Text.WordWrap
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                spacing: Theme.space6

                                Card {
                                    visible: EncoderController.atmosEnabled
                                    Layout.preferredWidth: 340
                                    title: qsTr("Live room")

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text: qsTr("drag to move — you hear it immediately")
                                            color: Theme.neutral600
                                            font.pixelSize: 10
                                            font.family: Theme.monoFamily
                                        }
                                        Item { Layout.fillWidth: true }
                                        Text {
                                            text: qsTr("latency %1 ms").arg(EncoderController.liveLatencyMs.toFixed(0))
                                            color: Theme.neutral600
                                            font.pixelSize: 10
                                            font.family: Theme.monoFamily
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
                                            model: EncoderController.objectCount
                                            Rectangle {
                                                required property int index
                                                readonly property var obj: {
                                                    const list = EncoderController.objectModel;
                                                    return index < list.length ? list[index] : null;
                                                }
                                                readonly property bool isSelected:
                                                    index === EncoderController.selectedObjectIndex
                                                visible: obj !== null
                                                width: isSelected ? 18 : 14
                                                height: isSelected ? 18 : 14
                                                color: isSelected ? Theme.accent : Theme.neutral800
                                                x: (obj ? obj.x : 0.5) * liveRoom.width - width / 2
                                                y: (obj ? obj.y : 0.5) * liveRoom.height - height / 2

                                                MouseArea {
                                                    anchors.fill: parent
                                                    onClicked: EncoderController.selectedObjectIndex = index
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
                                        title: qsTr("Layout — switching re-locks the receiver")

                                        Text {
                                            Layout.fillWidth: true
                                            text: EncoderController.atmosEnabled
                                                  ? qsTr("Atmos objects over a 5.1 bed — fixed while object mode is on")
                                                  : qsTr("Now encoding %1").arg(EncoderController.channelShapeName)
                                            color: Theme.text
                                            font.pixelSize: Theme.fontNormal
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            visible: !EncoderController.atmosEnabled
                                            spacing: Theme.space2

                                            Repeater {
                                                model: ["5.1", "7.1", "5.1.4", "7.1.4"]
                                                delegate: Rectangle {
                                                    id: liveLayoutButton
                                                    required property string modelData
                                                    readonly property bool active:
                                                        EncoderController.channelShapeName === modelData
                                                    readonly property bool beyondReceiver: modelData !== "5.1"
                                                    readonly property bool locked:
                                                        EncoderController.liveReconnecting
                                                        || EncoderController.liveWritingToDisk
                                                        || !EncoderController.liveActive

                                                    objectName: "liveLayout-" + modelData
                                                    Layout.fillWidth: true
                                                    Layout.preferredHeight: 36
                                                    color: active ? Theme.text : "transparent"
                                                    border.color: active ? Theme.text : Theme.divider
                                                    border.width: 1
                                                    opacity: locked && !active ? 0.4
                                                             : beyondReceiver && !active ? 0.7 : 1.0

                                                    RowLayout {
                                                        anchors.centerIn: parent
                                                        spacing: 5

                                                        Rectangle {
                                                            visible: liveLayoutButton.beyondReceiver
                                                            width: 6
                                                            height: 6
                                                            color: Theme.accent
                                                        }
                                                        Text {
                                                            text: liveLayoutButton.modelData
                                                            font.pixelSize: 12
                                                            font.family: Theme.monoFamily
                                                            color: liveLayoutButton.active ? Theme.bg : Theme.text
                                                        }
                                                    }
                                                    MouseArea {
                                                        anchors.fill: parent
                                                        enabled: !liveLayoutButton.locked && !liveLayoutButton.active
                                                        onClicked: EncoderController.switchLiveLayout(liveLayoutButton.modelData)
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            visible: !EncoderController.atmosEnabled
                                            Layout.fillWidth: true
                                            text: qsTr("Dotted layouts encode and meter, but the receiver leg stays Dolby Digital 5.1 until DD+ passthrough lands.")
                                            color: Theme.textMuted
                                            font.pixelSize: 10
                                            font.family: Theme.monoFamily
                                            wrapMode: Text.WordWrap
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: EncoderController.liveWritingToDisk
                                                  ? qsTr("The take is being written to disk, so the layout is fixed for this run — a restart would clobber the first half of the file.")
                                                  : qsTr("A layout change is a deliberate act, not a silent one: the stream stops, the receiver renegotiates, and about a second of audio is lost. The receiver's own display changes with it.")
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
                                                color: EncoderController.liveReconnecting ? Theme.accent700 : Theme.text
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
                                                color: EncoderController.liveUnderruns > 0 ? Theme.accent700 : Theme.text
                                                font.pixelSize: Theme.fontNormal
                                                font.family: Theme.monoFamily
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

                        // =====================================================
                        // Guided wizard — one more page, not a fourth tab.
                        // =====================================================
                        GuidedWizard {
                            Layout.fillWidth: true
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

                // ---- runs --------------------------------------------------
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
                                        font.family: Theme.monoFamily
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
                                font.family: Theme.monoFamily
                                font.pixelSize: 12
                                color: Theme.textMuted
                            }
                        }
                    }
                }
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

                // ---- command bar -------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 20
                    Layout.rightMargin: 20
                    Layout.topMargin: 12
                    Layout.bottomMargin: 12
                    spacing: 16

                    Rectangle {
                        objectName: "commandBar"
                        Layout.fillWidth: true
                        visible: appSettings.showCli
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
                                font.family: Theme.monoFamily
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
                    Item {
                        Layout.fillWidth: true
                        visible: !appSettings.showCli
                    }

                    Button {
                        objectName: "encodeButton"
                        text: EncoderController.busy
                              ? qsTr("Encoding…")
                              : qsTr("Encode to .%1").arg(EncoderController.outputSuffix())
                        enabled: EncoderController.sourceReady && !EncoderController.busy
                        highlighted: true
                        implicitHeight: 44
                        implicitWidth: Math.max(190, contentItem.implicitWidth + 40)
                        onClicked: window.startEncodeFlow()
                    }
                }
            }
        }
    }
}
