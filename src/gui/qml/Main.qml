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
        // Restore is a fresh-start feature. A controller that already holds
        // sources (a second window over the same singleton - the test
        // harness's shape, but also any future multi-window arrangement)
        // must not have a saved session STACKED on top of what is loaded.
        if (EncoderController.sourceModel.length > 0) {
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

    // --smoke-shot's scroll control: a plain property, deliberately, so
    // main.cpp's existing prop=value mechanism (apply_properties) can set it
    // with no new C++ code at all - see that file's own top comment for the
    // vocabulary this joins ("scrollY=650"). Pixels into the tab area's own
    // scroll range, applied to tabScroll's underlying Flickable the moment
    // it changes - ScrollView wraps a non-Flickable child (the tab
    // StackLayout here) in one automatically, and that wrapper IS what
    // contentItem resolves to in that case, so this needs no id on the
    // Flickable itself, only on the ScrollView.
    property real smokeScrollY: 0
    onSmokeScrollYChanged: {
        if (tabScrollView.contentItem) {
            tabScrollView.contentItem.contentY = smokeScrollY;
        }
    }

    // ---- the panel banner: one of the three feedback homes -----------------
    property int bannerRunId: -1
    property int dismissedRunId: -1
    // A pre-run refusal (encodeRefused) - shown in the same banner even
    // though no run entry exists for it. Cleared when a run actually starts
    // or the banner is dismissed.
    property string refusalText: ""
    // Bumped on every (coalesced) objectsChanged - a dependency hook for
    // bindings that read object-derived invokables/properties whose own
    // NOTIFY does not cover object drags (the plan strip's fed count).
    property int objectsRevision: 0
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

    // The rail's live branch is a per-device LIST (captureDeviceRows) now,
    // not a single ComboBox with its own `currentIndex` - these are what
    // every other live-session call site (recording, monitoring, cliLine,
    // the chain's capture cell) reads instead of a `deviceBox` id. Row 0 is
    // always the master; -1/"" with nothing selected reproduces exactly
    // what an empty ComboBox already meant to every one of those call sites.
    readonly property int liveMasterCaptureIndex: EncoderController.captureDeviceRows.length > 0
                                                  ? EncoderController.captureDeviceRows[0].deviceIndex
                                                  : -1
    readonly property string liveMasterCaptureName: EncoderController.captureDeviceRows.length > 0
                                                     ? EncoderController.captureDeviceRows[0].name
                                                     : ""

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
        // The tab exists whenever the live source is in play (the mockup's
        // sessionAvailable: pro && live), sessions running or not - it is
        // where a session is understood, not a modal that only appears once
        // one is already underway.
        if (tier !== "guided" && (inputMode === "live" || EncoderController.liveActive)) {
            tabs.push({ key: "session", label: qsTr("Live session"),
                        badge: EncoderController.liveActive ? qsTr("live") : "" });
        }
        return tabs;
    }
    onTierChanged: {
        appSettings.lastTier = tier;
    }
    // Whatever removed the current tab from the bar (a tier change, the live
    // source going away) sends focus back to Format - one rule instead of a
    // hand-written revert per cause.
    onVisibleTabsChanged: {
        if (!visibleTabs.some(tab => tab.key === currentTab)) {
            currentTab = "format";
        }
    }

    Connections {
        target: EncoderController
        function onLiveActiveChanged() {
            if (EncoderController.liveActive) {
                window.inputMode = "live";
                // Only a REAL session - a take on disk or a receiver leg -
                // steals focus. The rail's Monitor (auto-started by merely
                // picking a device) used to yank the user out of Format
                // mid-configuration, which no monitoring checkbox earns.
                if (EncoderController.liveWritingToDisk
                        || EncoderController.liveWantedPassthrough) {
                    window.currentTab = "session";
                }
            }
        }
        function onRecordingChanged() {
            if (EncoderController.recording) {
                window.inputMode = "live";
            }
        }
        function onObjectsChanged() {
            window.objectsRevision++;
        }
        function onEncodeRefused(reason) {
            // The third feedback home: a refusal that never opened a run
            // entry still lands in the banner, not just a status line the
            // run strip may have scrolled away.
            window.refusalText = reason;
        }
        function onBusyChanged() {
            if (EncoderController.busy) {
                window.refusalText = "";
            }
        }
    }

    // ---- the plan headline and the CLI line --------------------------------
    // Derived, never typed. Both read properties carrying NOTIFY planChanged,
    // so they stay live even though outputSuffix() is a plain invokable.
    readonly property string planRateText: {
        if (EncoderController.vbrAvailable && EncoderController.vbrEnabled) {
            let rate = qsTr("quality %1").arg(EncoderController.vbrQuality);
            if (EncoderController.vbrMinEnabled) {
                rate += qsTr(" · ≥%1").arg(EncoderController.vbrMinKbps);
            }
            if (EncoderController.vbrMaxEnabled) {
                rate += qsTr(" · ≤%1").arg(EncoderController.vbrMaxKbps);
            }
            return rate;
        }
        return qsTr("%1 kbps").arg(EncoderController.bitrateKbps);
    }
    // The suffix-free core, reused by the Live session chain's encode cell —
    // a running session may write no file at all, so a ".ec3" there would
    // describe something that does not exist.
    readonly property string planLineCore: {
        const codec = EncoderController.codecNames[EncoderController.codecIndex] || "";
        if (EncoderController.atmosEnabled) {
            const objects = EncoderController.objectCount;
            const shape = objects > 0 ? qsTr("5.1 bed + %1 objects").arg(objects)
                                      : qsTr("5.1 bed");
            return qsTr("%1 · %2 · %3").arg(codec).arg(shape).arg(window.planRateText);
        }
        return qsTr("%1 · %2 · %3")
            .arg(codec).arg(EncoderController.channelShapeName).arg(window.planRateText);
    }
    readonly property string planLine: qsTr("%1 · .%2")
        .arg(window.planLineCore).arg(EncoderController.outputSuffix())
    readonly property string planSubLine: {
        if (EncoderController.dualMono && !EncoderController.atmosEnabled) {
            return qsTr("acmod 0 · two independent programmes in one stream · no soundfield, no downmix");
        }
        // Object mode's fed count moves when objects are dragged, which
        // planChanged never announces - depending on objectsRevision here is
        // what keeps the sub-line honest mid-gesture.
        void window.objectsRevision;
        return EncoderController.layoutDetail;
    }

    // Quote a path or device name when it carries a space — the README's own
    // command-bar rule.
    function cliQuote(text) {
        return text.indexOf(" ") >= 0 ? "\"" + text + "\"" : text;
    }

    // What a source's channels DO, in a phrase — "feeds the bed",
    // "2 objects · 4 to the bed" — derived from the same assignment rows the
    // table edits, so the rail and the table can never disagree.
    function sourceRole(sourceIndex) {
        const rows = EncoderController.assignmentRows;
        let bed = 0, obj = 0, prog = 0, silent = 0, open = 0, total = 0;
        for (const row of rows) {
            if (row.source !== sourceIndex) continue;
            total++;
            const token = row.destToken;
            if (token === "obj") obj++;
            else if (token === "p1" || token === "p2") prog++;
            else if (token === "none") { if (row.touched) silent++; else open++; }
            else bed++;
        }
        if (total === 0) {
            return "";
        }
        if (open === total) {
            // Nothing assigned yet: exactly one source routes automatically;
            // several with nothing set genuinely go nowhere and say so.
            if (EncoderController.sourceModel.length === 1) {
                return EncoderController.atmosEnabled ? qsTr("each channel an object")
                                                      : qsTr("feeds the bed");
            }
            return qsTr("unassigned");
        }
        const parts = [];
        if (bed > 0) {
            parts.push(bed === total ? qsTr("feeds the bed") : qsTr("%1 to the bed").arg(bed));
        }
        if (obj > 0) {
            parts.push(obj === 1 ? qsTr("1 object") : qsTr("%1 objects").arg(obj));
        }
        if (prog > 0) {
            parts.push(qsTr("programme feed"));
        }
        if (silent > 0) {
            parts.push(qsTr("%1 silent").arg(silent));
        }
        if (open > 0) {
            parts.push(qsTr("%1 unassigned").arg(open));
        }
        return parts.join(" · ");
    }

    // ac3cli's actual grammar — real, pasteable syntax, not the handoff's
    // "--bed/--extras" sketch (which does not match ac3cli's positional
    // subcommands). Everything the positionals cannot say rides as trailing
    // tokens: extra sources (src=), the assignment (map=), the metadata, and
    // AC-3's bare `couple`. A live source renders the `live` subcommand; a
    // Matroska container is honestly TWO commands, because pasting one would
    // write a raw elementary stream into a file named .mkv - S/PDIF is the
    // same shape, one more ac3cli subcommand (spdif) over the same stream.
    readonly property string cliLine: {
        const eac3Stream = EncoderController.atmosEnabled || EncoderController.codecIndex === 1;
        if (window.inputMode === "live") {
            const liveCmd = ["ac3cli", "live", "out." + (eac3Stream ? "ec3" : "ac3"),
                             String(Math.max(window.liveMasterCaptureIndex, 0)), "10",
                             String(EncoderController.bitrateKbps),
                             liveMonitorCheck.checked ? "-1" : "-2",
                             liveReceiverBox.currentIndex > 0
                                 ? String(liveReceiverBox.currentIndex - 1) : "-2",
                             EncoderController.atmosEnabled ? "atmos" : "channels"];
            // The rail's second device, when one is selected - the same
            // capture2= token ac3cli's own `live` command takes, so the
            // command bar stays honest about a two-device session.
            if (EncoderController.captureDeviceRows.length > 1) {
                liveCmd.push("capture2=" + EncoderController.captureDeviceRows[1].deviceIndex);
            }
            return liveCmd.join(" ");
        }
        const source = EncoderController.sourcePath.length > 0
                       ? window.cliQuote(window.baseName(EncoderController.sourcePath))
                       : "<source>";
        const mkv = EncoderController.containerIndex === 1;
        const spdif = EncoderController.containerIndex === 2;
        const streamOut = "out." + (eac3Stream ? "ec3" : "ac3");
        const rate = String(EncoderController.bitrateKbps);
        const meta = EncoderController.metaTokens;
        const trailing = [];
        for (const row of EncoderController.sourceModel) {
            if (!row.primary) {
                trailing.push("src=" + window.cliQuote(window.baseName(row.path)));
            }
        }
        if (EncoderController.mapToken.length > 0) {
            trailing.push(EncoderController.mapToken);
        }
        // Per-source start offsets - only the ones actually set, source
        // order, so an all-zero session's line is unchanged from before
        // this token existed. atmos-encode has no src=/map=-style trailing
        // grammar at all (see below), so this only ever applies here.
        for (const row of EncoderController.sourceModel) {
            if (row.offsetSeconds > 0) {
                trailing.push("offset=" + row.index + ":" + row.offsetSeconds);
            }
        }

        let line;
        if (EncoderController.atmosEnabled) {
            // atmos-encode has no src=/map= grammar (every source channel is
            // its own object; there is no multi-file concept to route) - it
            // takes the object count, an optional exported keyframes file,
            // and honours the loudness tokens.
            const parts = ["ac3cli", "atmos-encode", source, streamOut, rate];
            if (EncoderController.objectCount > 0) {
                parts.push(String(EncoderController.objectCount));
                if (window.exportedPathsPath.length > 0) {
                    parts.push(window.cliQuote(window.baseName(window.exportedPathsPath)));
                }
            }
            for (const token of meta.split(" ")) {
                if (token.indexOf("dialnorm") === 0) {
                    parts.push(token);
                }
            }
            line = parts.join(" ");
        } else if (EncoderController.codecIndex === 0) {
            const parts = ["ac3cli", "encode", source, streamOut, rate,
                           EncoderController.channelLocationsText];
            if (EncoderController.coupling) {
                parts.push("couple");
            }
            line = parts.concat(trailing).join(" ");
            if (meta.length > 0) {
                line += " " + meta;
            }
        } else {
            const parts = ["ac3cli", "eac3-encode", source, streamOut, rate,
                           EncoderController.toolsToken.length > 0
                               ? EncoderController.toolsToken : "none",
                           EncoderController.channelLocationsText];
            if (EncoderController.vbrAvailable && EncoderController.vbrEnabled) {
                parts.push(EncoderController.vbrToken);
            }
            line = parts.concat(trailing).join(" ");
            if (meta.length > 0) {
                line += " " + meta;
            }
        }
        if (mkv) {
            line += " && ac3cli mkv " + streamOut + " out.mkv";
        } else if (spdif) {
            line += " && ac3cli spdif " + streamOut + " out.wav";
        }
        return line;
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
        onAccepted: EncoderController.startRecording(window.liveMasterCaptureIndex, selectedFile)
    }

    FileDialog {
        id: liveSessionDialog
        title: qsTr("Save the live take")
        fileMode: FileDialog.SaveFile
        onAccepted: EncoderController.startLiveSession(
                        window.liveMasterCaptureIndex, liveMonitorCheck.checked,
                        liveReceiverBox.currentIndex - 1, true, selectedFile)
    }

    // The Objects tab's "Export paths…" - writes every dynamic object's
    // authored motion (or static position) to a keyframes file in
    // ac3cli's atmos-path/atmos-encode grammar, so the exact line the
    // command bar shows (see window.cliLine) is honestly reproducible.
    FileDialog {
        id: exportPathsDialog
        title: qsTr("Export object paths")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("Text file (*.txt)"), qsTr("All files (*)")]
        defaultSuffix: "txt"
        currentFolder: window.outputFolderUrl()
        selectedFile: window.outputFolderUrl() + "/" + window.exportedPathsName()
        onAccepted: {
            window.exportedPathsPath = selectedFile;
            EncoderController.exportObjectPaths(selectedFile);
        }
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
    // Suggested name for "Export paths…" - <primary source>-paths.txt, the
    // exact filename cliLine then quotes back once one has actually been
    // exported (see exportedPathsPath).
    function exportedPathsName() {
        const stem = EncoderController.sourcePath.length > 0
                     ? baseName(EncoderController.sourcePath).replace(/\.[^.]*$/, "")
                     : "objects";
        return stem + "-paths.txt";
    }
    // Set once "Export paths…" actually writes a file this session - empty
    // until then, so cliLine has nothing honest to reference before a file
    // exists on disk. Cleared whenever the primary source changes, since a
    // path exported against a different file is no longer the one an
    // atmos-encode command line naming it would mean.
    property string exportedPathsPath: ""
    Connections {
        target: EncoderController
        function onSourceChanged() { window.exportedPathsPath = ""; }
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
    // Guided's contract: measured loudness + film-standard DRC. App-wide
    // defaults stay spec-neutral (dialnorm 31, DRC none, measure off) - this
    // is what GUIDED layers on top, and only while it is actually driving.
    //
    // Gated on tier being "guided" RIGHT NOW, not merely "was visited this
    // session" - entering guided fires this same tier value before a source
    // or a room is even chosen (dualMono isn't known yet then, and it fires
    // on the very first tier assignment at startup too), so checking at the
    // point this function is actually called is what lets it read settled
    // facts. Never applies over an explicit edit: EncoderController.
    // loudnessTouched is set only by LoudnessGroup.qml's own interactive
    // handlers, never by the writes below, so this function can safely
    // re-run on every call without ever mistaking its own writes for a
    // user's.
    //
    // Called from two places: when Guided reaches its "What you are about
    // to make" summary (GuidedWizard.qml's onCurrentStepKeyChanged), so the
    // summary tells the truth before Encode is ever pressed, and again here
    // in startEncodeFlow as a belt-and-suspenders final check covering the
    // failure-banner retry and the plain Encode button being clicked while
    // guided still nominally owns the tier. Idempotent both times - every
    // write below is a no-op once already at its target value.
    //
    // Dual mono: measurement is refused outright at encode time (see
    // LoudnessGroup.qml's own tooltip and encodeChannels' hard refusal), so
    // the contract must never set measureDialnorm/measureDialnorm2 there -
    // only its DRC half applies, and to BOTH programmes, since there is no
    // reason guided should give one dual-mono programme its sensible
    // default and leave the other at none.
    function applyGuidedLoudnessContract() {
        if (tier !== "guided" || EncoderController.loudnessTouched) {
            return;
        }
        if (EncoderController.dualMono) {
            EncoderController.drcIndex = 1;   // film-standard
            EncoderController.drc2Index = 1;  // film-standard, Ch2's own
        } else {
            EncoderController.measureDialnorm = true;
            EncoderController.drcIndex = 1;   // film-standard
        }
    }

    // What the Encode button and guided's "Encode now" both run — one flow,
    // so the two can never drift apart.
    function startEncodeFlow() {
        applyGuidedLoudnessContract();
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
        EncoderController.startRecording(window.liveMasterCaptureIndex,
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
                                                // "6 ch · 0:08 · feeds the bed · 44.1→48 k" -
                                                // the mockup's row says what the source DOES,
                                                // not whether it was added second; the
                                                // resample label (see addSourceFile's own
                                                // comment) only appears once a rate
                                                // mismatch actually triggered a conversion.
                                                text: {
                                                    const role = window.sourceRole(sourceRow.modelData.index);
                                                    let head = qsTr("%1 ch · %2")
                                                        .arg(sourceRow.modelData.channels)
                                                        .arg(sourceRow.modelData.duration);
                                                    if (role.length > 0) {
                                                        head += " · " + role;
                                                    }
                                                    if (sourceRow.modelData.resampleLabel.length > 0) {
                                                        head += " · " + sourceRow.modelData.resampleLabel;
                                                    }
                                                    return head;
                                                }
                                                font.pixelSize: 11
                                                color: Theme.textMuted
                                            }
                                        }
                                        // The level pip: whole-programme peak/RMS for
                                        // THIS source, pre-routing (see sourceLevels'
                                        // own doc comment) - a miniature version of
                                        // ChannelMeter's own track, looked up by index
                                        // rather than folded into sourceModel itself so
                                        // this Repeater's row identity never rebuilds on
                                        // a meter tick.
                                        Rectangle {
                                            id: sourcePipTrack
                                            Layout.preferredWidth: 40
                                            Layout.preferredHeight: 8
                                            color: Theme.neutral200
                                            clip: true

                                            readonly property var pip: {
                                                const levels = EncoderController.sourceLevels;
                                                return sourceRow.modelData.index < levels.length
                                                       ? levels[sourceRow.modelData.index] : ({});
                                            }
                                            readonly property real peakDb:
                                                pip.peakDb !== undefined ? pip.peakDb : -120

                                            Rectangle {
                                                x: 0
                                                y: 0
                                                height: parent.height
                                                width: sourcePipTrack.width * (
                                                    sourcePipTrack.pip.rmsDb !== undefined
                                                        ? EncoderController.meterFraction(
                                                              sourcePipTrack.pip.rmsDb)
                                                        : 0)
                                                color: sourcePipTrack.peakDb > -6.0
                                                       ? Theme.accent : Theme.neutral800
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
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.bottomMargin: 8
                                        spacing: Theme.space2
                                        Text {
                                            text: qsTr("Start offset")
                                            font.pixelSize: 10
                                            color: Theme.textMuted
                                        }
                                        Item { Layout.fillWidth: true }
                                        // Tenths of a second internally (SpinBox is
                                        // integer-only) - the same textFromValue/
                                        // valueFromText idiom the LFE mix control
                                        // uses to show a fractional-feeling control.
                                        SpinBox {
                                            objectName: "sourceOffsetSpin" + sourceRow.modelData.index
                                            from: 0
                                            to: 36000
                                            stepSize: 1
                                            editable: true
                                            enabled: !EncoderController.busy
                                            value: Math.round(sourceRow.modelData.offsetSeconds * 10)
                                            textFromValue: (value) => (value / 10).toFixed(1) + " s"
                                            valueFromText: (text) => Math.round(parseFloat(text) * 10) || 0
                                            onValueModified: EncoderController.setSourceOffset(
                                                                 sourceRow.modelData.index, value / 10)
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
                                            // The programme's own length, not just the
                                            // longest source's own duration - a later-
                                            // starting source can still end last (the
                                            // same max(offset + duration) the Objects
                                            // tab's timeline derives its length from).
                                            seconds = Math.max(seconds,
                                                sources[i].offsetSeconds + sources[i].seconds);
                                        }
                                        const mm = Math.floor(seconds / 60);
                                        const ss = String(Math.floor(seconds % 60)).padStart(2, "0");
                                        return [
                                            { label: qsTr("RATE"), value: sources.length > 0 ? EncoderController.groupDigits(sources[0].rate) : "—" },
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
                        // Loader-gated rather than a plain visible: toggle:
                        // this branch's per-device Repeater constructs a real
                        // native Button per selected device, and with a real
                        // capture endpoint on the machine (auto-selected as
                        // the master - see refreshCaptureDevices()) that
                        // Repeater is never empty, so an eagerly-constructed
                        // ColumnLayout here would build those Buttons on
                        // EVERY window this suite creates, file branch or
                        // not - dozens of them over a full run. Gating
                        // construction on inputMode actually being "live"
                        // keeps that cost where the file branch's own
                        // per-source Repeater already keeps it: built only
                        // when there is something to show.
                        Loader {
                            active: window.inputMode === "live"
                            visible: active
                            Layout.fillWidth: true
                            sourceComponent: liveBranchComponent
                        }
                    }

                    Component {
                        id: liveBranchComponent
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            // Mirrors the file branch's per-source list just
                            // above: one row per SELECTED device (not every
                            // device captureDevices() lists - see
                            // EncoderController.captureDeviceRows' own doc
                            // comment). Row 0 is always the master, whose
                            // delivery paces the session exactly as a
                            // single-device session always has; row 1, when
                            // present, is the slave, clock-conformed to it.
                            Repeater {
                                id: captureDeviceList
                                objectName: "captureDeviceList"
                                model: EncoderController.captureDeviceRows
                                delegate: ColumnLayout {
                                    id: deviceRow
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 0

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 8
                                        Layout.bottomMargin: 8

                                        Text {
                                            objectName: "liveDeviceName" + deviceRow.modelData.slotIndex
                                            Layout.fillWidth: true
                                            text: deviceRow.modelData.isMaster
                                                  ? deviceRow.modelData.name
                                                  : qsTr("%1 — slave").arg(deviceRow.modelData.name)
                                            elide: Text.ElideMiddle
                                            color: Theme.text
                                            font.pixelSize: 12
                                            font.family: Theme.monoFamily
                                            font.weight: Font.DemiBold
                                        }
                                        // A plain Text+MouseArea rather than a
                                        // native Button: this row is rebuilt
                                        // per device on a Repeater over real,
                                        // often non-empty data (a real capture
                                        // endpoint auto-selects as master - see
                                        // refreshCaptureDevices()), and this
                                        // machine's offscreen Qt Quick Controls
                                        // native-style path (see the recurring
                                        // "OpenThemeData() failed" warning)
                                        // hangs when a native Button lands
                                        // here under some real-object-mode
                                        // states - a lightweight control sidesteps
                                        // it entirely rather than working around
                                        // a platform-styling issue this app has
                                        // no other lever over.
                                        Text {
                                            objectName: "liveDeviceRemove" + deviceRow.modelData.slotIndex
                                            text: qsTr("Remove")
                                            color: removeArea.containsMouse ? Theme.text : Theme.textMuted
                                            font.pixelSize: 12
                                            font.family: Theme.monoFamily
                                            opacity: EncoderController.busy ? 0.4 : 1.0

                                            MouseArea {
                                                id: removeArea
                                                anchors.fill: parent
                                                anchors.margins: -4
                                                hoverEnabled: true
                                                enabled: !EncoderController.busy
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: EncoderController.removeCaptureDevice(
                                                               deviceRow.modelData.slotIndex)
                                            }
                                        }
                                    }
                                    Text {
                                        objectName: "liveDeviceMeta" + deviceRow.modelData.slotIndex
                                        text: deviceRow.modelData.rateText
                                        color: Theme.textMuted
                                        font.pixelSize: 11
                                        font.family: Theme.monoFamily
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 8
                                        height: 1
                                        color: Theme.neutral200
                                    }
                                }
                            }

                            Text {
                                visible: captureDeviceList.count === 0
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                Layout.bottomMargin: 8
                                text: qsTr("No capture devices were found.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.accent700
                            }

                            RowLayout {
                                visible: EncoderController.captureSupported
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                Layout.bottomMargin: 8
                                spacing: Theme.space2

                                ComboBox {
                                    id: addDeviceBox
                                    Layout.fillWidth: true
                                    enabled: !EncoderController.captureDeviceCapReached
                                             && !EncoderController.busy
                                    model: EncoderController.captureDevices
                                }
                                Button {
                                    objectName: "addCaptureDeviceButton"
                                    text: qsTr("Add input…")
                                    enabled: !EncoderController.captureDeviceCapReached
                                             && !EncoderController.busy
                                    onClicked: EncoderController.addCaptureDevice(
                                                   addDeviceBox.currentIndex)
                                }
                            }
                            Text {
                                objectName: "captureDeviceCapNote"
                                visible: EncoderController.captureDeviceCapReached
                                Layout.fillWidth: true
                                text: qsTr("Two devices per session — remove one to add another.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.textMuted
                            }
                            Text {
                                objectName: "captureDeviceTotals"
                                visible: EncoderController.captureDeviceCount > 0
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                text: EncoderController.captureDeviceTotals
                                font.pixelSize: 11
                                font.family: Theme.monoFamily
                                color: Theme.textMuted
                            }
                            Rectangle {
                                visible: EncoderController.captureDeviceCount > 0
                                Layout.fillWidth: true
                                Layout.topMargin: 8
                                Layout.bottomMargin: 8
                                height: 1
                                color: Theme.neutral300
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
                                                window.liveMasterCaptureIndex, true, -1, false, "");
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
                                Rectangle {
                                    visible: EncoderController.liveActive || EncoderController.recording
                                    width: 8
                                    height: 8
                                    color: Theme.accent
                                }
                                Text {
                                    visible: EncoderController.liveActive || EncoderController.recording
                                    text: EncoderController.recording
                                          ? qsTr("recording %1 s").arg(EncoderController.recordedSeconds.toFixed(1))
                                          : qsTr("monitoring %1 s").arg(EncoderController.liveRunningSeconds.toFixed(1))
                                    font.pixelSize: 12
                                    font.family: Theme.monoFamily
                                    color: Theme.accent700
                                }
                                Item { Layout.fillWidth: true }
                            }

                            Text {
                                visible: window.showExplanations
                                Layout.fillWidth: true
                                text: qsTr("Monitoring is free — nothing is written and no filename is asked for. The levels below are real. Open Live session to set up and start a real take.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.textMuted
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
                        // Rendered mode counts only the rows it shows: a bed
                        // channel a dependent replaces is not a speaker, and
                        // including it in either side of "N of M" made the
                        // footer disagree with the meters above it.
                        readonly property int fedCount: {
                            const meta = EncoderController.channelMeta;
                            let fed = 0;
                            for (let i = 0; i < meta.length; i++) {
                                if (window.meterMode === "rendered" && meta[i].replaced === true) {
                                    continue;
                                }
                                if (meta[i].fed !== false) fed++;
                            }
                            return fed;
                        }
                        readonly property int rowCount: {
                            const meta = EncoderController.channelMeta;
                            if (window.meterMode === "coded") {
                                return meta.length;
                            }
                            let rows = 0;
                            for (let i = 0; i < meta.length; i++) {
                                if (meta[i].replaced !== true) rows++;
                            }
                            return rows;
                        }

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
                                        channelIndex: meterRow.index
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
                            // Mono draws too - one dot at centre is a true
                            // statement about where the sound sits. Only dual
                            // mono has genuinely nothing to draw.
                            visible: EncoderController.hasLevels
                                     && !(EncoderController.dualMono && !EncoderController.atmosEnabled)
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

                                    RowLayout {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.left: parent.left
                                        anchors.leftMargin: Theme.space3
                                        spacing: Theme.space3

                                        Rectangle { width: 10; height: 10; color: Theme.text }
                                        ColumnLayout {
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
                            visible: !EncoderController.hasLevels
                            Layout.fillWidth: true
                            text: qsTr("Load a source, or start a live capture, and the plan's positions are drawn here at their real angles.")
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
                // One home for both a run that stopped and a refusal that
                // never opened a run. The headline says WHAT stopped it (the
                // cause's first sentence), not just which file was involved,
                // and the mockup's two recovery actions are real buttons.
                Rectangle {
                    id: failureBanner
                    Layout.fillWidth: true
                    visible: window.bannerRun !== null || window.refusalText.length > 0
                    color: Theme.accent100
                    implicitHeight: bannerColumn.implicitHeight + Theme.space3 * 2

                    function firstSentence(text) {
                        const at = text.indexOf(". ");
                        return at >= 0 ? text.substring(0, at + 1) : text;
                    }
                    readonly property string causeText: {
                        if (window.bannerRun === null) {
                            return window.refusalText;
                        }
                        const frames = window.bannerRun.framesText || "";
                        const cause = firstSentence(window.bannerRun.detail || "");
                        return frames.length > 0
                               ? qsTr("Run %1 stopped after %2 — %3")
                                     .arg(window.bannerRun.id).arg(frames).arg(cause)
                               : qsTr("Run %1 stopped — %2")
                                     .arg(window.bannerRun.id).arg(cause);
                    }
                    // The full story only when it says more than the headline.
                    readonly property string detailText: {
                        if (window.bannerRun === null) {
                            return "";
                        }
                        const detail = window.bannerRun.detail || "";
                        return firstSentence(detail) === detail ? "" : detail;
                    }
                    readonly property bool deviceClass: {
                        const text = (window.bannerRun !== null
                                      ? window.bannerRun.detail : window.refusalText) || "";
                        return text.indexOf("device") >= 0 || text.indexOf("bitstream") >= 0
                               || text.indexOf("IEC 61937") >= 0;
                    }
                    function dismiss() {
                        if (window.bannerRun !== null) {
                            window.dismissedRunId = window.bannerRun.id;
                        }
                        window.bannerRunId = -1;
                        window.refusalText = "";
                    }

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
                                text: failureBanner.causeText
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                color: Theme.text
                                wrapMode: Text.WordWrap
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: text.length > 0
                                text: failureBanner.detailText
                                font.pixelSize: 12
                                color: Theme.neutral800
                                wrapMode: Text.WordWrap
                            }
                        }
                        Button {
                            objectName: "bannerChooseDevice"
                            text: qsTr("Choose another device")
                            flat: true
                            visible: failureBanner.deviceClass
                            onClicked: {
                                // The relevant combo: the rail's capture list
                                // for a live/record failure, the Format tab's
                                // passthrough panel otherwise.
                                if (window.inputMode === "live") {
                                    EncoderController.refreshCaptureDevices();
                                } else {
                                    window.currentTab = "format";
                                    EncoderController.refreshOutputDevices();
                                }
                            }
                        }
                        Button {
                            objectName: "bannerRetryFile"
                            text: qsTr("Retry as file")
                            flat: true
                            visible: EncoderController.sourceReady && !EncoderController.busy
                            onClicked: {
                                failureBanner.dismiss();
                                window.startEncodeFlow();
                            }
                        }
                        Button {
                            text: qsTr("Dismiss")
                            flat: true
                            onClicked: failureBanner.dismiss()
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
                    id: tabScrollView
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
                                            // During a live session a preset is still a real
                                            // act - the mockup's own interaction table has it
                                            // renegotiate the running stream, which is what
                                            // switchLiveLayout does (and refuses, with a
                                            // status line, while a take is on disk).
                                            enabled: (!EncoderController.busy
                                                      || EncoderController.liveActive)
                                                     && !EncoderController.atmosEnabled
                                            onClicked: {
                                                if (EncoderController.liveActive) {
                                                    EncoderController.switchLiveLayout(modelData);
                                                } else {
                                                    EncoderController.applyChannelPreset(modelData);
                                                }
                                            }
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
                                        // Under VBR the fixed rate is not a target any
                                        // more - it keeps feeding the coupling/SPX band-
                                        // edge defaults (its other job), and saying so is
                                        // what stops two rate controls competing.
                                        text: EncoderController.vbrAvailable && EncoderController.vbrEnabled
                                              ? qsTr("Bit rate — band-edge reference, not a target")
                                              : qsTr("Bit rate")
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

                                    // Row 3: empty under Codec/Container - GridLayout fills
                                    // cells row-major, so these three items together place
                                    // the advisory under Bit rate specifically. Object mode
                                    // has its own 384 kbps advisory already (Advanced tab
                                    // below), so this one steps aside for it rather than
                                    // showing two competing warnings.
                                    Item {}
                                    Text {
                                        objectName: "bitrateFloorAdvisory"
                                        Layout.fillWidth: true
                                        visible: !EncoderController.atmosEnabled
                                                 && EncoderController.bitrateKbps
                                                    < EncoderController.fullBandwidthCodedChannelCount
                                                      * EncoderController.kbpsPerChannelFloor
                                        text: qsTr("%1 coded channels at %2 kbps will audibly starve — encoders refuse outright below the frame minimum.")
                                              .arg(EncoderController.fullBandwidthCodedChannelCount)
                                              .arg(EncoderController.bitrateKbps)
                                        color: Theme.textMuted
                                        font.pixelSize: 10
                                        wrapMode: Text.WordWrap
                                    }
                                    Item {}
                                }

                                VbrPanel {
                                    Layout.fillWidth: true
                                    showExplanations: window.showExplanations
                                    // The mockup never renders Rate mode under a live
                                    // source - a live session always runs CBR, and the
                                    // rail's own warning covers the transition case.
                                    visible: window.inputMode !== "live"
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
                                        text: qsTr("%1 of %2 positions used · %3 coded channels")
                                              .arg(EncoderController.channelBudgetUsed)
                                              .arg(EncoderController.channelBudgetMax)
                                              .arg(EncoderController.codedChannelCount)
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
                                            // 1+1 draws DASHED - "categorically different"
                                            // (a bed of two programmes, not a speaker
                                            // shape) - via the Canvas below, since a
                                            // Rectangle border cannot dash.
                                            border.color: active ? Theme.text
                                                                 : (dual ? "transparent" : Theme.divider)
                                            border.width: 1
                                            opacity: locked && !active ? 0.25 : 1.0

                                            Canvas {
                                                anchors.fill: parent
                                                visible: bedButton.dual && !bedButton.active
                                                property color dashColor: Theme.neutral500
                                                onDashColorChanged: requestPaint()
                                                onVisibleChanged: if (visible) requestPaint()
                                                onPaint: {
                                                    const ctx = getContext("2d");
                                                    ctx.clearRect(0, 0, width, height);
                                                    ctx.strokeStyle = String(dashColor);
                                                    ctx.lineWidth = 1;
                                                    ctx.setLineDash([4, 3]);
                                                    ctx.strokeRect(0.5, 0.5, width - 1, height - 1);
                                                }
                                            }

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
                                                        // The channel tokens themselves ("Lw Rw"),
                                                        // the same names the channel map prints -
                                                        // not a count.
                                                        text: extraRow.modelData.tokens
                                                        font.pixelSize: 11
                                                        font.family: Theme.monoFamily
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

                                // The mockup's accent dual-mono note - what 1+1 is FOR,
                                // shown whenever it is the bed, independent of the
                                // explanations preference (it is a state banner, not a
                                // tutorial).
                                Rectangle {
                                    visible: EncoderController.dualMono && !EncoderController.atmosEnabled
                                    Layout.fillWidth: true
                                    color: Theme.accent100
                                    implicitHeight: dualNoteText.implicitHeight + 24

                                    Rectangle {
                                        anchors.left: parent.left
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        width: 2
                                        color: Theme.accent
                                    }
                                    Text {
                                        id: dualNoteText
                                        anchors.fill: parent
                                        anchors.leftMargin: 14
                                        anchors.rightMargin: 14
                                        anchors.topMargin: 12
                                        text: qsTr("Dual mono carries two unrelated soundtracks — a second language, a commentary track — chosen by the listener, not mixed together. There is no stereo pair, no surround, no LFE and no downmix, and each programme carries its own dialnorm and compression.")
                                        wrapMode: Text.WordWrap
                                        font.pixelSize: 12
                                        color: Theme.text
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
                                    text: qsTr("ROUTING — WHAT HAPPENS TO THIS SOURCE")
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
                                                    // The coded/speaker split, not the shape
                                                    // name (that is the plan strip's job) -
                                                    // this cell is where a dependent
                                                    // substream's replaced channels stop
                                                    // being invisible bookkeeping.
                                                    return qsTr("%1 coded · %2 spk")
                                                        .arg(EncoderController.codedChannelCount)
                                                        .arg(EncoderController.renderedChannelCount);
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
                                        objectName: "playToReceiver"
                                        text: EncoderController.playing ? qsTr("Playing…") : qsTr("Play")
                                        // Greyed for an endpoint that cannot bitstream
                                        // THIS stream, instead of failing after the
                                        // click - the labels already say why.
                                        enabled: EncoderController.canPlay && !EncoderController.busy
                                                 && !EncoderController.playing
                                                 && EncoderController.outputDevices.length > 0
                                                 && EncoderController.outputDeviceCanBitstream(outputBox.currentIndex)
                                        onClicked: EncoderController.playToReceiver(outputBox.currentIndex)
                                    }
                                }
                                Text {
                                    visible: window.tier !== "guided" && window.showExplanations
                                    Layout.fillWidth: true
                                    text: qsTr("Sends the encoded stream as IEC 61937 bursts in exclusive mode, so the receiver decodes it. AC-3 rides data-type-1 bursts and E-AC-3 data-type-21 bursts at four-times rate — each endpoint's label says which it accepts, and Play stays greyed for a stream the selected endpoint cannot take. Only S/PDIF and HDMI endpoints can bitstream at all.")
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

                                // Ch2's own heavy compression (§7.7.2.2) - dual mono only. Lives
                                // beside programme 1's card rather than inside LoudnessGroup.qml's
                                // Programme 2 block: heavy compression itself is an Expert-tier-only
                                // control (the card above), so its programme-2 twin belongs at the
                                // same tier rather than newly appearing in Basic/Advanced, where
                                // programme 1's heavy compression is not offered either.
                                Card {
                                    title: qsTr("Heavy compression — programme 2")
                                    visible: EncoderController.dualMono

                                    CheckBox {
                                        text: qsTr("Heavy compression")
                                        enabled: !EncoderController.busy
                                        checked: EncoderController.heavy2
                                        onToggled: EncoderController.heavy2 = checked
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: EncoderController.heavy2
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
                                                    value: Math.round(EncoderController.ceiling2Db * 10)
                                                    textFromValue: (value) => (value / 10).toFixed(1) + " dBFS"
                                                    valueFromText: (text) => Math.round(parseFloat(text) * 10)
                                                    onValueModified: EncoderController.ceiling2Db = value / 10
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
                                                    value: Math.round(EncoderController.dialogue2Db)
                                                    textFromValue: (value) => value + " dBFS"
                                                    valueFromText: (text) => parseInt(text)
                                                    onValueModified: EncoderController.dialogue2Db = value
                                                }

                                                Item { Layout.fillWidth: true }
                                            }

                                            Text {
                                                Layout.fillWidth: true
                                                text: qsTr("Ch2's own peak ceiling (§7.7.2.2) - dual mono has no downmix to bound, so this is measured on programme 2's own signal, independently of the card above.")
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
                            objectName: "objectsTab"
                            Layout.fillWidth: true
                            spacing: Theme.gap

                            property string driveMode: "author"
                            property real playheadTime: 0
                            // Mirrors the controller's own audio clock -
                            // Preview plays every path back through the
                            // Atmos encoder and the monitor sink for real
                            // (see EncoderController::startMotionPreview),
                            // so there is no separate visual-only pacing
                            // here anymore; this just follows it.
                            readonly property bool previewing: EncoderController.motionPreviewActive
                            Connections {
                                target: EncoderController
                                function onMotionPreviewTimeChanged() {
                                    if (EncoderController.motionPreviewActive) {
                                        objectsTab.playheadTime = EncoderController.motionPreviewTime;
                                    }
                                }
                            }
                            // One source of truth for the ruler, the lanes,
                            // the keys and the playhead - max(offset +
                            // duration) over every loaded source, never
                            // stored. Falls back to the prototype's old
                            // fixed 8 s when nothing derivable is loaded
                            // yet (a live session, or nothing at all).
                            readonly property real timelineLength: {
                                let end = 0;
                                const sources = EncoderController.sourceModel;
                                for (let i = 0; i < sources.length; i++) {
                                    const at = sources[i].offsetSeconds + sources[i].seconds;
                                    if (at > end) end = at;
                                }
                                return end > 0 ? end : 8;
                            }
                            // Horizontal zoom: 1 = the whole derived length
                            // fits the lane width; higher zooms into a
                            // narrower window (visibleSeconds below).
                            property real zoomFactor: 1
                            readonly property real maxZoom: 40
                            readonly property real visibleSeconds: timelineLength / zoomFactor
                            // Seconds at the LEFT edge of the visible
                            // window - the pan position.
                            property real panOffset: 0
                            function clampPan(p) {
                                return Math.max(0, Math.min(Math.max(0, timelineLength - visibleSeconds), p));
                            }
                            // Changes zoom while keeping centerTime under
                            // the same relative position in the window it
                            // held before - the wheel-to-zoom and +/-
                            // buttons both go through this.
                            function setZoom(newZoom, centerTime) {
                                const clamped = Math.max(1, Math.min(maxZoom, newZoom));
                                if (clamped === zoomFactor) return;
                                const newVisible = timelineLength / clamped;
                                const fraction = visibleSeconds > 0 ? (centerTime - panOffset) / visibleSeconds : 0.5;
                                zoomFactor = clamped;
                                panOffset = clampPan(centerTime - fraction * newVisible);
                            }
                            // Ruler tick promotion and drag-snap granularity
                            // move together, keyed off how many pixels one
                            // second currently spans - "0.1 s / 1 s / 10 s"
                            // for the ruler, "1 s coarse / 0.1 s fine / a
                            // 32 ms floor" for snapping (one 1536-sample
                            // OAMD frame at 48 kHz - finer is fake
                            // precision).
                            function zoomTier(pixelsPerSecond) {
                                if (pixelsPerSecond >= 400) return "fine";
                                if (pixelsPerSecond >= 60) return "medium";
                                return "coarse";
                            }
                            function tickInterval(pixelsPerSecond) {
                                const tier = zoomTier(pixelsPerSecond);
                                return tier === "fine" ? 0.1 : (tier === "medium" ? 1.0 : 10.0);
                            }
                            function snapIncrement(pixelsPerSecond) {
                                const tier = zoomTier(pixelsPerSecond);
                                return tier === "fine" ? 0.032 : (tier === "medium" ? 0.1 : 1.0);
                            }
                            function snapTime(t) {
                                const inc = snapIncrement(timelineWrap.pixelsPerSecond);
                                return Math.max(0, Math.min(timelineLength, Math.round(t / inc) * inc));
                            }
                            // The key the timeline has selected for editing,
                            // as its time on the SELECTED object's path; -1
                            // when none. Cleared whenever the selection moves.
                            property real selectedKeyTime: -1
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
                            // Where the selected object IS right now: along
                            // its path while the preview plays, else its
                            // static config - so the elevation view and the
                            // x/y/z readouts animate exactly what the plan
                            // markers already do.
                            readonly property var selectedLive: {
                                if (!previewing || selectedObj === null) {
                                    return null;
                                }
                                void objectsRevision;
                                return EncoderController.evaluateObjectPath(
                                    selectedObj.index, playheadTime);
                            }
                            readonly property real selX: selectedLive ? selectedLive.x
                                                        : (selectedObj ? selectedObj.x : 0.5)
                            readonly property real selY: selectedLive ? selectedLive.y
                                                        : (selectedObj ? selectedObj.y : 0.5)
                            readonly property real selZ: selectedLive ? selectedLive.z
                                                        : (selectedObj ? selectedObj.z : 0)

                            function formatTime(t) {
                                const minutes = Math.floor(t / 60);
                                const seconds = t - minutes * 60;
                                return minutes + ":" + seconds.toFixed(2).padStart(5, "0");
                            }

                            // selectedObjectIndex has no signal of its own
                            // (it notifies through objectsChanged), so the
                            // key selection clears here when it moves.
                            property int lastSelectedIndex: -1
                            Connections {
                                target: EncoderController
                                function onObjectsChanged() {
                                    objectsTab.objectsRevision++;
                                    if (EncoderController.selectedObjectIndex
                                            !== objectsTab.lastSelectedIndex) {
                                        objectsTab.lastSelectedIndex =
                                            EncoderController.selectedObjectIndex;
                                        objectsTab.selectedKeyTime = -1;
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.margins: 24
                                spacing: Theme.space3

                                // ---- header: switch beside the title, the
                                // summary beneath it - the mockup's layout.
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.gap

                                    Switch {
                                        id: atmosSwitch
                                        objectName: "atmosSwitch"
                                        Layout.alignment: Qt.AlignTop
                                        enabled: !EncoderController.busy
                                        checked: EncoderController.atmosEnabled
                                        onToggled: EncoderController.atmosEnabled = checked
                                    }

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        // preferredWidth 1 lets the row SHRINK the
                                        // texts below their implicit width and elide,
                                        // instead of pushing the row past the panel.
                                        Layout.preferredWidth: 1
                                        spacing: 2

                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("Encode as Dolby Atmos objects")
                                            font.pixelSize: 15
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                            color: Theme.text
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: EncoderController.atmosEnabled
                                                  ? qsTr("%1 objects from the assignments · E-AC-3 over a 5.1 bed · positions ride as OAMD")
                                                    .arg(EncoderController.objectCount)
                                                  : qsTr("Off — the stream is a plain channel bed. Turning this on fixes the codec to E-AC-3 over 5.1.")
                                            elide: Text.ElideRight
                                            color: Theme.textMuted
                                            font.pixelSize: Theme.fontSmall
                                        }
                                    }

                                    // The rate-floor warning earns its dress -
                                    // triangle, accent rule - and shows only once
                                    // objects actually exist to starve.
                                    RowLayout {
                                        visible: EncoderController.atmosEnabled
                                                 && EncoderController.objectCount > 0
                                                 && EncoderController.bitrateKbps < 384
                                        spacing: Theme.space2

                                        Rectangle {
                                            implicitWidth: rateWarnRow.implicitWidth + 18
                                            implicitHeight: rateWarnRow.implicitHeight + 8
                                            color: Theme.accent100

                                            Rectangle {
                                                anchors.left: parent.left
                                                anchors.top: parent.top
                                                anchors.bottom: parent.bottom
                                                width: 2
                                                color: Theme.accent
                                            }
                                            RowLayout {
                                                id: rateWarnRow
                                                anchors.centerIn: parent
                                                spacing: 6
                                                Text {
                                                    text: "⚠"
                                                    color: Theme.accent700
                                                    font.pixelSize: Theme.fontSmall
                                                }
                                                Text {
                                                    text: qsTr("Objects over a 5.1 bed want 384 kbps or better")
                                                    color: Theme.accent700
                                                    font.pixelSize: Theme.fontSmall
                                                }
                                            }
                                        }
                                        Button {
                                            text: qsTr("Set it")
                                            enabled: !EncoderController.busy
                                            onClicked: EncoderController.bitrateKbps = 384
                                        }
                                    }
                                }

                                // ---- sounds available -----------------------
                                // The mockup's strip: what could become an
                                // object, and the way to bring more in. The
                                // chips read the same source list the rail
                                // shows; Change jumps to the one table that
                                // decides what each sound does.
                                ColumnLayout {
                                    visible: EncoderController.atmosEnabled
                                    Layout.fillWidth: true
                                    spacing: Theme.space2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.space2

                                        Text {
                                            text: qsTr("SOUNDS AVAILABLE")
                                            color: Theme.neutral600
                                            font.pixelSize: 10
                                            font.letterSpacing: 1
                                        }
                                        Item { Layout.fillWidth: true }
                                        Button {
                                            text: qsTr("Import audio…")
                                            flat: true
                                            enabled: !EncoderController.busy
                                            onClicked: EncoderController.sourceModel.length > 0
                                                       ? addSourceDialog.open() : openDialog.open()
                                        }
                                        Button {
                                            text: qsTr("Add live input")
                                            flat: true
                                            enabled: !EncoderController.busy
                                            onClicked: {
                                                // One input at a time today - the rail's
                                                // live branch becomes THE input; mixing
                                                // files with capture is future work.
                                                window.inputMode = "live";
                                            }
                                        }
                                    }

                                    Flow {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        visible: EncoderController.sourceModel.length > 0

                                        Repeater {
                                            model: EncoderController.sourceModel

                                            delegate: Rectangle {
                                                id: soundChip
                                                required property var modelData
                                                width: chipRow.implicitWidth + 16
                                                height: 24
                                                color: Theme.neutral100
                                                border.color: Theme.divider
                                                border.width: 1

                                                RowLayout {
                                                    id: chipRow
                                                    anchors.centerIn: parent
                                                    spacing: 6

                                                    Text {
                                                        text: soundChip.modelData.label
                                                        font.pixelSize: 11
                                                        font.family: Theme.monoFamily
                                                        color: Theme.text
                                                    }
                                                    Text {
                                                        text: qsTr("%1 ch · in use").arg(soundChip.modelData.channels)
                                                        font.pixelSize: 10
                                                        color: Theme.textMuted
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            height: 24
                                            verticalAlignment: Text.AlignVCenter
                                            text: qsTr("Change →")
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
                                            color: Theme.accent700

                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: window.goAssign()
                                            }
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
                                                        // Select on press, then DRAG the
                                                        // marker itself - "drag to place"
                                                        // must not require starting the
                                                        // gesture beside the dot.
                                                        onPressed: EncoderController.selectedObjectIndex = marker.index
                                                        onPositionChanged: (mouse) => {
                                                            if (!(mouse.buttons & Qt.LeftButton)
                                                                    || EncoderController.busy
                                                                    || objectsTab.driveMode !== "author") {
                                                                return;
                                                            }
                                                            const p = mapToItem(room, mouse.x, mouse.y);
                                                            EncoderController.setObjectPosition(
                                                                marker.index,
                                                                Math.max(0, Math.min(1, p.x / room.width)),
                                                                Math.max(0, Math.min(1, p.y / room.height)),
                                                                marker.obj ? marker.obj.z : 0);
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            visible: objectsTab.selectedObj !== null
                                                     && objectsTab.selectedObj.hasPath
                                            Layout.fillWidth: true
                                            text: qsTr("This object follows its authored path — dragging edits its idle position, not the path. Scrub the timeline and Add key to author motion.")
                                            wrapMode: Text.WordWrap
                                            font.pixelSize: 10
                                            color: Theme.neutral600
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

                                            // A SIDE view: the horizontal axis is the
                                            // room's depth (y - front wall at the left,
                                            // rear at the right), never x. z +1
                                            // (ceiling) at the top line, 0 (ear level)
                                            // at 66%, -1 (floor) at the bottom - the
                                            // mockup's own proportions.
                                            readonly property real earY: height * 0.66
                                            function zToY(z) {
                                                return z >= 0 ? earY - z * (earY - 14)
                                                              : earY + (-z) * ((height - 10) - earY);
                                            }
                                            function yToZ(y) {
                                                if (y <= earY) {
                                                    return Math.min(1, (earY - y) / (earY - 14));
                                                }
                                                return Math.max(-1, -(y - earY) / ((height - 10) - earY));
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
                                                y: elevation.earY; height: 1
                                                color: Theme.neutral300
                                            }
                                            Text {
                                                x: 4; y: elevation.earY - 12
                                                text: qsTr("ear level")
                                                color: Theme.neutral500
                                                font.pixelSize: 9
                                            }
                                            Text {
                                                x: 4; y: parent.height - 13
                                                text: qsTr("front")
                                                color: Theme.neutral500
                                                font.pixelSize: 9
                                                horizontalAlignment: Text.AlignLeft
                                            }
                                            Text {
                                                x: parent.width - implicitWidth - 4
                                                y: parent.height - 13
                                                text: qsTr("rear")
                                                color: Theme.neutral500
                                                font.pixelSize: 9
                                            }

                                            // Context: the bed's speakers, so the
                                            // object's height reads against something -
                                            // ear-level fronts and surrounds on the ear
                                            // line, the ceiling pair above.
                                            Repeater {
                                                model: [
                                                    { fy: 0.08, ceiling: false }, { fy: 0.5, ceiling: false },
                                                    { fy: 0.88, ceiling: false },
                                                    { fy: 0.3, ceiling: true }, { fy: 0.7, ceiling: true }
                                                ]
                                                delegate: Rectangle {
                                                    required property var modelData
                                                    width: 6
                                                    height: 6
                                                    color: Theme.neutral400
                                                    x: modelData.fy * elevation.width - 3
                                                    y: (modelData.ceiling ? 14 : elevation.earY) - 3
                                                }
                                            }

                                            MouseArea {
                                                anchors.fill: parent
                                                enabled: !EncoderController.busy
                                                         && objectsTab.driveMode === "author"
                                                         && objectsTab.selectedObj !== null
                                                onPositionChanged: (mouse) => place(mouse)
                                                onPressed: (mouse) => place(mouse)
                                                function place(mouse) {
                                                    const y = Math.max(0, Math.min(1, mouse.x / elevation.width));
                                                    EncoderController.setObjectPosition(
                                                        objectsTab.selectedObj.index,
                                                        objectsTab.selectedObj.x, y,
                                                        elevation.yToZ(mouse.y));
                                                }
                                            }

                                            // The selected object's marker with a drop
                                            // line to the FLOOR - height reads as height
                                            // above the ground, not distance from the
                                            // ear line.
                                            Rectangle {
                                                visible: objectsTab.selectedObj !== null
                                                readonly property real markerX: objectsTab.selY * elevation.width
                                                readonly property real markerY: elevation.zToY(objectsTab.selZ)
                                                x: markerX - 1
                                                y: markerY
                                                width: 2
                                                height: Math.max(0, (elevation.height - 10) - markerY)
                                                color: Theme.accent300
                                            }
                                            Rectangle {
                                                id: elevationMarker
                                                visible: objectsTab.selectedObj !== null
                                                width: 14
                                                height: 14
                                                color: Theme.accent
                                                border.color: Theme.text
                                                border.width: 2
                                                x: objectsTab.selY * elevation.width - width / 2
                                                y: elevation.zToY(objectsTab.selZ) - height / 2

                                                Rectangle {
                                                    anchors.left: parent.right
                                                    anchors.leftMargin: 4
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: elevationChip.implicitWidth + 6
                                                    height: elevationChip.implicitHeight + 2
                                                    color: Theme.bg

                                                    Text {
                                                        id: elevationChip
                                                        anchors.centerIn: parent
                                                        text: qsTr("obj %1 · z %2")
                                                              .arg(EncoderController.selectedObjectIndex + 1)
                                                              .arg(objectsTab.selZ.toFixed(2))
                                                        color: Theme.text
                                                        font.pixelSize: 10
                                                        font.family: Theme.monoFamily
                                                    }
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: Theme.space3

                                            Repeater {
                                                // A static model - the values live in the
                                                // delegate bindings, so a 30 Hz preview
                                                // tick re-evaluates three Texts instead
                                                // of rebuilding three delegates.
                                                model: ["x", "y", "z"]

                                                ColumnLayout {
                                                    required property string modelData
                                                    Layout.fillWidth: true
                                                    spacing: 2

                                                    Text {
                                                        text: parent.modelData
                                                        color: Theme.neutral600
                                                        font.pixelSize: 9
                                                        font.capitalization: Font.AllUppercase
                                                    }
                                                    Text {
                                                        text: (parent.modelData === "x" ? objectsTab.selX
                                                               : parent.modelData === "y" ? objectsTab.selY
                                                                                          : objectsTab.selZ).toFixed(2)
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
                                                            // The shape's name when a preset
                                                            // authored it ("orbit"), the key
                                                            // count for hand-made paths.
                                                            text: {
                                                                if (!objRow.obj || !objRow.obj.hasPath) {
                                                                    return qsTr("static");
                                                                }
                                                                return objRow.obj.pathLabel.length > 0
                                                                       ? objRow.obj.pathLabel
                                                                       : qsTr("%1 keys").arg(objRow.obj.keyCount);
                                                            }
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
                                                // The bed's LFE is the sixteenth object,
                                                // and every bed-pinned channel spends a
                                                // dynamic slot - the denominator says
                                                // what is genuinely left, not "16".
                                                text: {
                                                    const pinned = EncoderController.pinnedObjectCount;
                                                    const cap = 15 - pinned;
                                                    if (pinned > 0) {
                                                        return qsTr("%1 of %2 objects · %3 pinned to the bed")
                                                            .arg(EncoderController.objectCount).arg(cap).arg(pinned);
                                                    }
                                                    return qsTr("%1 of %2 objects · each one is a sound with a place")
                                                        .arg(EncoderController.objectCount).arg(cap);
                                                }
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
                                                  + " / " + objectsTab.formatTime(objectsTab.timelineLength)
                                            color: Theme.textMuted
                                            font.pixelSize: 11
                                            font.family: Theme.monoFamily
                                        }
                                        Text {
                                            text: qsTr("scrub · double-click for a key · drag to retime (snaps) · right-click removes · shift-drag a clip moves its keys too")
                                            color: Theme.neutral500
                                            font.pixelSize: 9
                                            font.family: Theme.monoFamily
                                            elide: Text.ElideRight
                                        }
                                        Item { Layout.fillWidth: true }
                                        Button {
                                            objectName: "zoomOutButton"
                                            text: "−"
                                            flat: true
                                            implicitWidth: 26
                                            enabled: objectsTab.zoomFactor > 1
                                            onClicked: objectsTab.setZoom(
                                                           objectsTab.zoomFactor / 1.5,
                                                           objectsTab.panOffset + objectsTab.visibleSeconds / 2)
                                        }
                                        Text {
                                            objectName: "zoomReadout"
                                            text: Math.round(objectsTab.zoomFactor * 100) + "%"
                                            color: Theme.neutral600
                                            font.pixelSize: 9
                                            font.family: Theme.monoFamily
                                        }
                                        Button {
                                            objectName: "zoomInButton"
                                            text: "+"
                                            flat: true
                                            implicitWidth: 26
                                            enabled: objectsTab.zoomFactor < objectsTab.maxZoom
                                            onClicked: objectsTab.setZoom(
                                                           objectsTab.zoomFactor * 1.5,
                                                           objectsTab.panOffset + objectsTab.visibleSeconds / 2)
                                        }
                                        Button {
                                            objectName: "zoomFitButton"
                                            text: qsTr("Fit")
                                            flat: true
                                            visible: objectsTab.zoomFactor > 1
                                            onClicked: {
                                                objectsTab.zoomFactor = 1;
                                                objectsTab.panOffset = 0;
                                            }
                                        }
                                        Button {
                                            objectName: "exportPathsButton"
                                            text: qsTr("Export paths…")
                                            flat: true
                                            enabled: !EncoderController.busy
                                            onClicked: exportPathsDialog.open()
                                        }
                                        Button {
                                            objectName: "addKeyButton"
                                            text: qsTr("Add key")
                                            enabled: !EncoderController.busy && objectsTab.selectedObj !== null
                                            onClicked: EncoderController.addObjectKeyframe(
                                                           objectsTab.selectedObj.index, objectsTab.playheadTime)
                                        }
                                        Button {
                                            objectName: "deleteKeyButton"
                                            text: qsTr("Delete key")
                                            enabled: !EncoderController.busy
                                                     && objectsTab.selectedObj !== null
                                                     && objectsTab.selectedKeyTime >= 0
                                            onClicked: {
                                                EncoderController.removeObjectKeyframe(
                                                    objectsTab.selectedObj.index, objectsTab.selectedKeyTime);
                                                objectsTab.selectedKeyTime = -1;
                                            }
                                        }
                                        Button {
                                            objectName: "previewButton"
                                            text: objectsTab.previewing ? qsTr("Stop") : qsTr("Preview")
                                            enabled: objectsTab.previewing || !EncoderController.busy
                                            onClicked: {
                                                if (objectsTab.previewing) {
                                                    EncoderController.stopMotionPreview();
                                                } else {
                                                    EncoderController.startMotionPreview();
                                                }
                                            }
                                        }
                                    }

                                    Item {
                                        id: timelineWrap
                                        Layout.fillWidth: true
                                        implicitHeight: timelineColumn.implicitHeight

                                        // ONE geometry for the ruler, the clip bands, the
                                        // keys and the playhead - the 8 px disagreement
                                        // between them was exactly the drift a shared
                                        // mapping ends. laneSpan/pixelsPerSecond feed
                                        // objectsTab's own zoom-tier/snap functions, so
                                        // "how many pixels is a second" is answered once.
                                        readonly property real laneLeft: 78
                                        readonly property real laneRight: 8
                                        readonly property real laneSpan: width - laneLeft - laneRight
                                        readonly property real pixelsPerSecond:
                                            objectsTab.visibleSeconds > 0 ? laneSpan / objectsTab.visibleSeconds : 0
                                        function xToTime(x) {
                                            return Math.max(0, Math.min(objectsTab.timelineLength,
                                                objectsTab.panOffset
                                                + (x - laneLeft) / laneSpan * objectsTab.visibleSeconds));
                                        }

                                        Rectangle {
                                            anchors.fill: parent
                                            color: "transparent"
                                            border.color: Theme.divider
                                            border.width: 1
                                        }

                                        // Scrubbing: anywhere on the timeline positions
                                        // the playhead; the lanes' own handlers sit on
                                        // top for their gestures. Wheel zooms, centred on
                                        // whatever time is under the cursor.
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: !EncoderController.busy
                                            onPressed: (mouse) => scrub(mouse)
                                            onPositionChanged: (mouse) => {
                                                if (mouse.buttons & Qt.LeftButton) scrub(mouse);
                                            }
                                            onWheel: (wheel) => {
                                                const centerTime = timelineWrap.xToTime(wheel.x);
                                                const factor = wheel.angleDelta.y > 0 ? 1.2 : (1 / 1.2);
                                                objectsTab.setZoom(objectsTab.zoomFactor * factor, centerTime);
                                                wheel.accepted = true;
                                            }
                                            function scrub(mouse) {
                                                if (objectsTab.previewing) {
                                                    EncoderController.stopMotionPreview();
                                                }
                                                objectsTab.playheadTime = timelineWrap.xToTime(mouse.x);
                                            }
                                        }

                                        ColumnLayout {
                                            id: timelineColumn
                                            width: parent.width
                                            spacing: 0

                                            Rectangle {
                                                Layout.fillWidth: true
                                                implicitHeight: 20
                                                color: Theme.neutral100
                                                clip: true

                                                Item {
                                                    id: rulerRow
                                                    anchors.fill: parent
                                                    anchors.leftMargin: timelineWrap.laneLeft
                                                    anchors.rightMargin: timelineWrap.laneRight

                                                    readonly property real tickInterval:
                                                        objectsTab.tickInterval(timelineWrap.pixelsPerSecond)
                                                    readonly property int firstTick:
                                                        Math.ceil(objectsTab.panOffset / tickInterval)
                                                    readonly property int lastTick:
                                                        Math.floor((objectsTab.panOffset + objectsTab.visibleSeconds)
                                                                    / tickInterval)

                                                    Repeater {
                                                        model: Math.max(0, rulerRow.lastTick - rulerRow.firstTick + 1)
                                                        Text {
                                                            required property int index
                                                            readonly property real t:
                                                                (rulerRow.firstTick + index) * rulerRow.tickInterval
                                                            y: (parent ? parent.height : 0) / 2 - implicitHeight / 2
                                                            x: (t - objectsTab.panOffset) / objectsTab.visibleSeconds
                                                               * rulerRow.width - implicitWidth / 2
                                                            text: (rulerRow.tickInterval < 1 ? t.toFixed(1) : t.toFixed(0)) + "s"
                                                            color: Theme.neutral600
                                                            font.pixelSize: 9
                                                            font.family: Theme.monoFamily
                                                        }
                                                    }
                                                }
                                            }

                                            // A thin draggable viewport indicator, pan's
                                            // affordance - a separate strip rather than
                                            // dragging the ruler itself, so it can never
                                            // fight the ruler's own tick layout or the
                                            // scrub area underneath the whole timeline.
                                            Rectangle {
                                                id: panStrip
                                                objectName: "panStrip"
                                                Layout.fillWidth: true
                                                visible: objectsTab.zoomFactor > 1
                                                implicitHeight: 8
                                                color: Theme.neutral100

                                                Rectangle {
                                                    id: panThumb
                                                    objectName: "panThumb"
                                                    readonly property real trackWidth:
                                                        panStrip.width - timelineWrap.laneLeft - timelineWrap.laneRight
                                                    height: parent.height
                                                    x: timelineWrap.laneLeft + (objectsTab.timelineLength > 0
                                                           ? objectsTab.panOffset / objectsTab.timelineLength * trackWidth
                                                           : 0)
                                                    width: Math.max(12, objectsTab.timelineLength > 0
                                                           ? objectsTab.visibleSeconds / objectsTab.timelineLength * trackWidth
                                                           : trackWidth)
                                                    color: Theme.accent400
                                                    radius: 2
                                                }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    property real pressX: 0
                                                    property real pressPan: 0
                                                    onPressed: (mouse) => {
                                                        pressX = mouse.x;
                                                        pressPan = objectsTab.panOffset;
                                                    }
                                                    onPositionChanged: (mouse) => {
                                                        if (!(mouse.buttons & Qt.LeftButton)) return;
                                                        if (panThumb.trackWidth <= 0) return;
                                                        const deltaSeconds = (mouse.x - pressX) / panThumb.trackWidth
                                                                              * objectsTab.timelineLength;
                                                        objectsTab.panOffset = objectsTab.clampPan(pressPan + deltaSeconds);
                                                    }
                                                }
                                            }

                                            Rectangle { Layout.fillWidth: true; height: 1; color: Theme.divider }

                                            // ---- clip bands: each source's active span,
                                            // draggable to change its start offset. Shift-
                                            // drag also brings that source's objects' own
                                            // keyframes along by the same delta.
                                            Repeater {
                                                model: EncoderController.sourceModel

                                                RowLayout {
                                                    id: clipRow
                                                    required property var modelData
                                                    Layout.fillWidth: true
                                                    spacing: 0

                                                    Text {
                                                        Layout.preferredWidth: 70
                                                        Layout.leftMargin: 8
                                                        text: clipRow.modelData.label
                                                        elide: Text.ElideRight
                                                        color: Theme.neutral700
                                                        font.pixelSize: 9
                                                        font.family: Theme.monoFamily
                                                    }

                                                    Item {
                                                        id: clipLane
                                                        Layout.fillWidth: true
                                                        Layout.rightMargin: timelineWrap.laneRight
                                                        Layout.preferredHeight: 14
                                                        clip: true

                                                        function timeToX(t) {
                                                            return (t - objectsTab.panOffset) / objectsTab.visibleSeconds
                                                                   * clipLane.width;
                                                        }
                                                        function xToTime(x) {
                                                            return objectsTab.panOffset
                                                                   + x / clipLane.width * objectsTab.visibleSeconds;
                                                        }

                                                        Rectangle {
                                                            id: clipBand
                                                            objectName: "clipBand" + clipRow.modelData.index
                                                            property bool dragging: false
                                                            property real dragOffset: clipRow.modelData.offsetSeconds
                                                            property bool shiftHeld: false
                                                            height: parent.height
                                                            x: clipLane.timeToX(
                                                                   dragging ? dragOffset : clipRow.modelData.offsetSeconds)
                                                            width: Math.max(4, objectsTab.visibleSeconds > 0
                                                                   ? clipRow.modelData.seconds / objectsTab.visibleSeconds
                                                                     * clipLane.width
                                                                   : 0)
                                                            color: Theme.accent200
                                                            border.color: Theme.accent400
                                                            border.width: 1
                                                            radius: 2

                                                            MouseArea {
                                                                anchors.fill: parent
                                                                enabled: !EncoderController.busy
                                                                onPressed: (mouse) => {
                                                                    clipBand.dragging = true;
                                                                    clipBand.dragOffset = clipRow.modelData.offsetSeconds;
                                                                    clipBand.shiftHeld =
                                                                        (mouse.modifiers & Qt.ShiftModifier) !== 0;
                                                                }
                                                                onPositionChanged: (mouse) => {
                                                                    if (!clipBand.dragging) return;
                                                                    if (mouse.modifiers & Qt.ShiftModifier) {
                                                                        clipBand.shiftHeld = true;
                                                                    }
                                                                    const laneX = clipBand.x + mouse.x;
                                                                    clipBand.dragOffset = objectsTab.snapTime(
                                                                        Math.max(0, clipLane.xToTime(laneX)));
                                                                }
                                                                onReleased: (mouse) => {
                                                                    if (!clipBand.dragging) return;
                                                                    clipBand.dragging = false;
                                                                    const delta = clipBand.dragOffset
                                                                                  - clipRow.modelData.offsetSeconds;
                                                                    if (Math.abs(delta) < 0.001) return;
                                                                    EncoderController.setSourceOffset(
                                                                        clipRow.modelData.index, clipBand.dragOffset);
                                                                    if (clipBand.shiftHeld) {
                                                                        // Move keys with source: every
                                                                        // object this source owns shifts
                                                                        // by the same delta, clamped at 0
                                                                        // (shiftObjectKeyframes' own job).
                                                                        const objects = EncoderController.objectModel;
                                                                        for (let i = 0; i < objects.length; i++) {
                                                                            if (objects[i].sourceIndex === clipRow.modelData.index) {
                                                                                EncoderController.shiftObjectKeyframes(
                                                                                    objects[i].index, delta);
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                visible: EncoderController.sourceModel.length > 0
                                                Layout.fillWidth: true
                                                height: 1
                                                color: Theme.divider
                                            }

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
                                                        font.weight: laneRow.index === EncoderController.selectedObjectIndex
                                                                     ? Font.DemiBold : Font.Normal
                                                    }

                                                    Rectangle {
                                                        id: lane
                                                        Layout.fillWidth: true
                                                        Layout.rightMargin: timelineWrap.laneRight
                                                        Layout.preferredHeight: 24
                                                        clip: true
                                                        readonly property bool isSelected:
                                                            laneRow.index === EncoderController.selectedObjectIndex
                                                        readonly property var keys:
                                                            (objectsTab.objectsRevision,
                                                             EncoderController.objectKeyframes(laneRow.index))
                                                        color: isSelected ? Theme.accent100 : "transparent"

                                                        function timeToX(t) {
                                                            return (t - objectsTab.panOffset) / objectsTab.visibleSeconds
                                                                   * lane.width;
                                                        }
                                                        function xToTime(x) {
                                                            return objectsTab.panOffset
                                                                   + x / lane.width * objectsTab.visibleSeconds;
                                                        }

                                                        // Select on press, scrub on drag; a
                                                        // double-click authors a key at that
                                                        // instant from the object's current
                                                        // spot, snapped to the current tier.
                                                        MouseArea {
                                                            anchors.fill: parent
                                                            enabled: !EncoderController.busy
                                                            onPressed: (mouse) => {
                                                                EncoderController.selectedObjectIndex = laneRow.index;
                                                                if (objectsTab.previewing) {
                                                                    EncoderController.stopMotionPreview();
                                                                }
                                                                objectsTab.playheadTime = Math.max(0,
                                                                    Math.min(objectsTab.timelineLength, lane.xToTime(mouse.x)));
                                                            }
                                                            onPositionChanged: (mouse) => {
                                                                if (mouse.buttons & Qt.LeftButton) {
                                                                    objectsTab.playheadTime = Math.max(0,
                                                                        Math.min(objectsTab.timelineLength, lane.xToTime(mouse.x)));
                                                                }
                                                            }
                                                            onDoubleClicked: (mouse) => {
                                                                EncoderController.selectedObjectIndex = laneRow.index;
                                                                EncoderController.addObjectKeyframe(
                                                                    laneRow.index,
                                                                    objectsTab.snapTime(lane.xToTime(mouse.x)));
                                                            }
                                                        }

                                                        Rectangle {
                                                            anchors.left: parent.left
                                                            anchors.top: parent.top
                                                            anchors.bottom: parent.bottom
                                                            width: 1
                                                            color: Theme.divider
                                                        }

                                                        Rectangle {
                                                            visible: lane.keys.length > 1
                                                            x: lane.keys.length > 1 ? lane.timeToX(lane.keys[0].time) : 0
                                                            width: lane.keys.length > 1
                                                                   ? Math.max(0, lane.timeToX(lane.keys[lane.keys.length - 1].time)
                                                                                 - lane.timeToX(lane.keys[0].time))
                                                                   : 0
                                                            y: lane.height / 2
                                                            height: 1
                                                            color: lane.isSelected ? Theme.accent400 : Theme.neutral400
                                                        }

                                                        Repeater {
                                                            model: lane.keys
                                                            Item {
                                                                id: keyMark
                                                                required property var modelData
                                                                readonly property bool keySelected:
                                                                    lane.isSelected
                                                                    && Math.abs(objectsTab.selectedKeyTime - modelData.time) < 0.005
                                                                // While a drag is in flight the
                                                                // VISUAL position is local state -
                                                                // committing per-move would rebuild
                                                                // this delegate mid-gesture.
                                                                property bool dragging: false
                                                                property real dragTime: 0

                                                                width: 16
                                                                height: lane.height
                                                                x: lane.timeToX(dragging ? dragTime : modelData.time) - width / 2
                                                                y: 0
                                                                z: 1

                                                                Rectangle {
                                                                    anchors.centerIn: parent
                                                                    width: keyMark.keySelected ? 10 : 8
                                                                    height: keyMark.keySelected ? 10 : 8
                                                                    rotation: 45
                                                                    color: lane.isSelected ? Theme.accent : Theme.text
                                                                    border.color: keyMark.keySelected ? Theme.text : "transparent"
                                                                    border.width: keyMark.keySelected ? 2 : 0
                                                                }

                                                                MouseArea {
                                                                    anchors.fill: parent
                                                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                                                    enabled: !EncoderController.busy
                                                                    onPressed: (mouse) => {
                                                                        EncoderController.selectedObjectIndex = laneRow.index;
                                                                        objectsTab.selectedKeyTime = keyMark.modelData.time;
                                                                        if (mouse.button === Qt.LeftButton) {
                                                                            keyMark.dragging = true;
                                                                            keyMark.dragTime = keyMark.modelData.time;
                                                                        }
                                                                    }
                                                                    onPositionChanged: (mouse) => {
                                                                        if (!keyMark.dragging) return;
                                                                        const laneX = keyMark.x + mouse.x;
                                                                        keyMark.dragTime = objectsTab.snapTime(
                                                                            lane.xToTime(laneX));
                                                                    }
                                                                    onReleased: (mouse) => {
                                                                        if (!keyMark.dragging) return;
                                                                        keyMark.dragging = false;
                                                                        if (Math.abs(keyMark.dragTime - keyMark.modelData.time) >= 0.005) {
                                                                            EncoderController.moveObjectKeyframe(
                                                                                laneRow.index, keyMark.modelData.time,
                                                                                keyMark.dragTime);
                                                                            objectsTab.selectedKeyTime = keyMark.dragTime;
                                                                        }
                                                                    }
                                                                    onClicked: (mouse) => {
                                                                        if (mouse.button === Qt.RightButton) {
                                                                            objectsTab.selectedKeyTime = -1;
                                                                            EncoderController.removeObjectKeyframe(
                                                                                laneRow.index, keyMark.modelData.time);
                                                                        }
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Rectangle {
                                            id: playhead
                                            readonly property real rawX: timelineWrap.laneLeft
                                                + (objectsTab.playheadTime - objectsTab.panOffset)
                                                  / objectsTab.visibleSeconds * timelineWrap.laneSpan
                                            x: rawX
                                            y: 0
                                            width: 2
                                            height: timelineWrap.height
                                            color: Theme.accent
                                            visible: rawX >= timelineWrap.laneLeft
                                                     && rawX <= timelineWrap.laneLeft + timelineWrap.laneSpan
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
                                        textFormat: Text.StyledText
                                        // The actual endpoint, by name - "the receiver"
                                        // is a hedge when the session knows exactly who
                                        // is re-locking.
                                        text: EncoderController.liveReceiverName.length > 0
                                              ? qsTr("<b>Renegotiating with %1.</b> The receiver is re-locking to the new bitstream format — expect a second of silence. This is normal AVR behaviour on a format change.")
                                                .arg(EncoderController.liveReceiverName)
                                              : qsTr("Renegotiating with the receiver. It is re-locking to the new bitstream format — expect a second of silence. This is normal AVR behaviour on a format change.")
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

                                // ---- receiver: pre-flight AND hot-swap ---------
                                // The one control that means something in BOTH
                                // phases: before Start it is the pre-flight
                                // choice startLiveSession reads; once live, an
                                // explicit pick (onActivated, never a binding)
                                // instead hot-swaps the passthrough leg without
                                // restarting capture or encode - index 0 turns
                                // passthrough off, same vocabulary either way.
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.space2

                                    Text {
                                        text: qsTr("Receiver")
                                        color: Theme.neutral600
                                        font.pixelSize: 12
                                    }
                                    ComboBox {
                                        id: liveReceiverBox
                                        objectName: "liveReceiverBox"
                                        Layout.fillWidth: true
                                        model: [qsTr("No passthrough")].concat(EncoderController.outputDevices)
                                        onActivated: {
                                            if (EncoderController.liveActive) {
                                                EncoderController.switchLiveReceiver(currentIndex - 1);
                                            }
                                        }
                                    }
                                }

                                // ---- idle: pre-flight -------------------------
                                // Where a session starts, now that the rail's own
                                // live branch keeps only the signal-side acts
                                // (device pick, Refresh, Monitor, Record). Every
                                // choice a real take needs - monitor, whether to
                                // write, the safety copy - lives here, live and
                                // editable, not a disabled readout.
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    visible: !EncoderController.liveActive
                                    spacing: Theme.space2

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.space2

                                        CheckBox {
                                            id: liveMonitorCheck
                                            objectName: "liveMonitorCheck"
                                            text: qsTr("Monitor")
                                            checked: true
                                            font.pixelSize: 12
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.space2

                                        CheckBox {
                                            id: liveWriteCheck
                                            objectName: "liveWriteCheck"
                                            text: qsTr("Also write the take to disk")
                                            font.pixelSize: 12
                                        }
                                        CheckBox {
                                            id: liveWavSafetyCheck
                                            objectName: "liveWavSafetyCheck"
                                            text: qsTr("Raw-WAV safety copy")
                                            enabled: liveWriteCheck.checked
                                            checked: EncoderController.liveWavSafetyCopy
                                            font.pixelSize: 12
                                            onToggled: EncoderController.liveWavSafetyCopy = checked
                                        }
                                        Item { Layout.fillWidth: true }
                                        Button {
                                            objectName: "startSessionButton"
                                            text: qsTr("Start session")
                                            highlighted: true
                                            enabled: EncoderController.captureSupported
                                                     && !EncoderController.busy
                                            onClicked: {
                                                if (liveWriteCheck.checked) {
                                                    window.openSaveDialog(liveSessionDialog,
                                                                          EncoderController.suggestedOutputName());
                                                } else {
                                                    EncoderController.startLiveSession(
                                                        window.liveMasterCaptureIndex, liveMonitorCheck.checked,
                                                        liveReceiverBox.currentIndex - 1, false, "");
                                                }
                                            }
                                        }
                                    }
                                    Text {
                                        visible: window.showExplanations
                                        Layout.fillWidth: true
                                        text: qsTr("Pick a device on the rail first, then set up the take here — monitor, an optional receiver leg, and whether to write it to disk.")
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
                                }

                                // ---- running: the real transport ---------------
                                RowLayout {
                                    Layout.fillWidth: true
                                    visible: EncoderController.liveActive
                                    spacing: Theme.space6

                                    Button {
                                        objectName: "stopSessionButton"
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
                                                return String(m).padStart(2, "0") + ":"
                                                       + rem.toFixed(1).padStart(4, "0");
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
                                            text: EncoderController.groupDigits(EncoderController.liveFramesEncoded)
                                            color: Theme.text
                                            font.pixelSize: 15
                                            font.family: Theme.monoFamily
                                        }
                                    }
                                    ColumnLayout {
                                        spacing: 2
                                        Text { text: qsTr("DROPPED"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            text: EncoderController.groupDigits(EncoderController.liveFramesDropped)
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
                                        objectName: "chainCaptureCell"
                                        Layout.fillWidth: true
                                        Text { text: qsTr("CAPTURE"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            objectName: "chainCaptureName"
                                            Layout.fillWidth: true
                                            text: window.liveMasterCaptureName.length > 0
                                                  ? window.liveMasterCaptureName : qsTr("Capture device")
                                            color: Theme.text
                                            font.pixelSize: Theme.fontNormal
                                            wrapMode: Text.WordWrap
                                        }
                                        Text {
                                            visible: EncoderController.liveCaptureDetail.length > 0
                                            text: EncoderController.liveCaptureDetail
                                            color: Theme.textMuted
                                            font.pixelSize: 10
                                            font.family: Theme.monoFamily
                                        }
                                        // The slave's measured clock correction - honest,
                                        // updated with the same ~30 Hz cadence as every
                                        // other live stat, empty (and this row hidden)
                                        // outside a two-device session.
                                        Text {
                                            objectName: "chainCaptureDrift"
                                            visible: EncoderController.liveDriftText.length > 0
                                            text: EncoderController.liveDriftText
                                            color: Theme.textMuted
                                            font.pixelSize: 10
                                            font.family: Theme.monoFamily
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
                                            // Suffix-free: a session may write no file,
                                            // so ".ec3" here would describe nothing.
                                            text: window.planLineCore
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
                                        objectName: "chainReceiverCell"
                                        Layout.fillWidth: true
                                        Text { text: qsTr("RECEIVER LEG — IEC 61937"); color: Theme.neutral600; font.pixelSize: 10 }
                                        Text {
                                            objectName: "chainReceiverPlanText"
                                            Layout.fillWidth: true
                                            text: EncoderController.liveReceiverPlanText
                                            color: Theme.text
                                            font.pixelSize: Theme.fontNormal
                                            wrapMode: Text.WordWrap
                                        }
                                        Text {
                                            visible: EncoderController.livePassthrough
                                            // The capped downmix leg bitstreams plain
                                            // AC-3 even when the main plan needs E-AC-3
                                            // (that is the whole point of the leg) - so
                                            // this reads the ACTUAL format on the wire,
                                            // not the main plan's.
                                            text: (EncoderController.atmosEnabled || EncoderController.codecIndex === 1)
                                                  && !EncoderController.liveDownmixLeg
                                                  ? qsTr("exclusive · E-AC-3 bursts (data type 21)")
                                                  : qsTr("exclusive · AC-3 bursts (data type 1)")
                                            color: Theme.textMuted
                                            font.pixelSize: 10
                                            font.family: Theme.monoFamily
                                        }
                                    }
                                }
                            }

                            // The GAP banner: the receiver leg carries less than the
                            // encode. Three reasons now reach here, all making liveGap
                            // true - object mode with an E-AC-3-capable receiver (the
                            // leg is always just the 5.1 bed, independent of the
                            // capped downmix leg below), object mode with an AC-3-only
                            // receiver (the leg is the capped downmix of that SAME
                            // bed), and a wide channel layout with an AC-3-only
                            // receiver (the leg is a 5.1 downmix of the full layout) -
                            // named from the two plans, not a stock sentence about a
                            // passthrough that has already landed.
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
                                    objectName: "liveGapMessage"
                                    anchors.fill: parent
                                    anchors.margins: Theme.space3
                                    text: {
                                        if (EncoderController.atmosEnabled) {
                                            return EncoderController.liveDownmixLeg
                                                ? qsTr("The encode is a 5.1 bed with %1 objects; the receiver leg is a Dolby Digital 5.1 downmix of that bed. Every object move is visible on the meters and the soundfield, but %2 can only bitstream Dolby Digital — the amplifier plays the downmix, not the motion.")
                                                      .arg(EncoderController.objectCount)
                                                      .arg(EncoderController.liveReceiverName)
                                                : qsTr("The encode is a 5.1 bed with %1 objects; the receiver leg is that Dolby Digital Plus 5.1 bed. Every object move is visible on the meters and the soundfield, but a consumer decoder gates object decoding — the amplifier plays the bed, not the motion.")
                                                      .arg(EncoderController.objectCount);
                                        }
                                        return qsTr("The encode is %1; the receiver leg is a Dolby Digital 5.1 downmix — everything past it is visible on the meters, not audible on the amplifier.")
                                              .arg(EncoderController.channelShapeName);
                                    }
                                    color: Theme.accent800
                                    font.pixelSize: Theme.fontSmall
                                    wrapMode: Text.WordWrap
                                }
                            }

                            // A passthrough that was asked for and did NOT open is a
                            // different story - the leg carries nothing, and the text
                            // already on liveReceiverPlanText says why.
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.leftMargin: 24
                                Layout.rightMargin: 24
                                visible: EncoderController.liveActive
                                         && EncoderController.liveWantedPassthrough
                                         && !EncoderController.livePassthrough
                                color: Theme.accent100
                                implicitHeight: noPassMsg.implicitHeight + Theme.space3 * 2

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 2
                                    color: Theme.accent
                                }
                                Text {
                                    id: noPassMsg
                                    anchors.fill: parent
                                    anchors.margins: Theme.space3
                                    text: qsTr("No passthrough opened — %1 The session still encodes, meters and monitors; only the receiver leg is missing.")
                                          .arg(EncoderController.liveReceiverPlanText)
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

                                    // The mockup's "Objects in this session" panel:
                                    // which sounds are live objects, which one a drag
                                    // would move, and - now that the session
                                    // pre-allocates a fixed slot budget rather than
                                    // exactly the device's channel count (see
                                    // startLiveSession) - room to add one or
                                    // reassign one below.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text: qsTr("OBJECTS IN THIS SESSION")
                                            color: Theme.neutral600
                                            font.pixelSize: 10
                                            font.letterSpacing: 1
                                        }
                                        Item { Layout.fillWidth: true }
                                        Text {
                                            objectName: "liveObjectSlotsCounter"
                                            text: EncoderController.liveActive
                                                  ? qsTr("%1 of %2 slots live")
                                                    .arg(EncoderController.liveObjectSlotsBound)
                                                    .arg(EncoderController.objectCount)
                                                  : qsTr("%1 objects live").arg(EncoderController.objectCount)
                                            color: Theme.accent700
                                            font.pixelSize: 10
                                            font.family: Theme.monoFamily
                                        }
                                    }
                                    Flow {
                                        Layout.fillWidth: true
                                        spacing: 4

                                        Repeater {
                                            model: EncoderController.objectModel

                                            delegate: Rectangle {
                                                id: sessionObjChip
                                                required property var modelData
                                                readonly property bool isSelected:
                                                    modelData.index === EncoderController.selectedObjectIndex
                                                width: sessionObjText.implicitWidth + 12
                                                height: 20
                                                color: isSelected ? Theme.accent100 : Theme.neutral100
                                                border.color: isSelected ? Theme.accent : Theme.divider
                                                border.width: 1

                                                Text {
                                                    id: sessionObjText
                                                    anchors.centerIn: parent
                                                    text: sessionObjChip.isSelected
                                                          ? qsTr("obj %1 · %2 · dragging")
                                                            .arg(sessionObjChip.modelData.index + 1)
                                                            .arg(sessionObjChip.modelData.sourceLabel)
                                                          : qsTr("obj %1 · %2")
                                                            .arg(sessionObjChip.modelData.index + 1)
                                                            .arg(sessionObjChip.modelData.sourceLabel)
                                                    font.pixelSize: 9
                                                    font.family: Theme.monoFamily
                                                    color: Theme.text
                                                }
                                                MouseArea {
                                                    anchors.fill: parent
                                                    onClicked: EncoderController.selectedObjectIndex =
                                                                   sessionObjChip.modelData.index
                                                }
                                            }
                                        }
                                    }

                                    // Add or reassign, live: both just change which
                                    // input plane feeds which object mid-loop (see
                                    // addLiveObject/reassignLiveObjectSlot's own
                                    // comments) - the JOC object COUNT stays fixed
                                    // for the whole session, only the channel
                                    // binding moves.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: EncoderController.liveActive
                                        spacing: Theme.space2

                                        ComboBox {
                                            id: liveObjectChannelPicker
                                            objectName: "liveObjectChannelPicker"
                                            Layout.preferredWidth: 96
                                            // One label per flat capture-channel index -
                                            // "Ch 1".."Ch N" for the master, then "Dev2
                                            // Ch 1".."Dev2 Ch N" for a selected slave - so
                                            // a two-device session's picker names which
                                            // device a channel actually comes from.
                                            model: EncoderController.liveCaptureChannelLabels
                                        }
                                        Button {
                                            objectName: "addLiveObjectButton"
                                            text: qsTr("Add")
                                            flat: true
                                            enabled: EncoderController.liveObjectSlotsBound
                                                     < EncoderController.objectCount
                                            onClicked: EncoderController.addLiveObject(
                                                           liveObjectChannelPicker.currentIndex)
                                        }
                                        Button {
                                            objectName: "reassignLiveObjectButton"
                                            text: qsTr("Reassign selected")
                                            flat: true
                                            onClicked: EncoderController.reassignLiveObjectSlot(
                                                           EncoderController.selectedObjectIndex,
                                                           liveObjectChannelPicker.currentIndex)
                                        }
                                        Button {
                                            objectName: "silenceLiveObjectButton"
                                            text: qsTr("Silence selected")
                                            flat: true
                                            onClicked: EncoderController.reassignLiveObjectSlot(
                                                           EncoderController.selectedObjectIndex, -1)
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text: qsTr("drag to move — you hear it immediately")
                                            color: Theme.neutral600
                                            font.pixelSize: 10
                                            font.family: Theme.monoFamily
                                        }
                                        Item { Layout.fillWidth: true }
                                    }

                                    Rectangle {
                                        id: liveRoom
                                        Layout.preferredWidth: 320
                                        Layout.preferredHeight: 320
                                        color: Theme.neutral100
                                        border.color: Theme.divider
                                        border.width: 1

                                        // The crosshair and the walls' names - the same
                                        // furniture the Objects tab's room carries.
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
                                                id: liveMarker
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
                                                z: isSelected ? 1 : 0

                                                Rectangle {
                                                    visible: liveMarker.isSelected
                                                    anchors.left: parent.right
                                                    anchors.leftMargin: 4
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    width: liveChipText.implicitWidth + 6
                                                    height: liveChipText.implicitHeight + 2
                                                    color: Theme.bg

                                                    Text {
                                                        id: liveChipText
                                                        anchors.centerIn: parent
                                                        text: qsTr("obj %1").arg(liveMarker.index + 1)
                                                        color: Theme.text
                                                        font.pixelSize: 10
                                                        font.family: Theme.monoFamily
                                                    }
                                                }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    onPressed: EncoderController.selectedObjectIndex = liveMarker.index
                                                    onPositionChanged: (mouse) => {
                                                        if (!(mouse.buttons & Qt.LeftButton)
                                                                || !EncoderController.liveActive) {
                                                            return;
                                                        }
                                                        const p = mapToItem(liveRoom, mouse.x, mouse.y);
                                                        EncoderController.setObjectPosition(
                                                            liveMarker.index,
                                                            Math.max(0, Math.min(1, p.x / liveRoom.width)),
                                                            Math.max(0, Math.min(1, p.y / liveRoom.height)),
                                                            liveMarker.obj ? liveMarker.obj.z : 0);
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    // The mockup's readout grid: where the dragged
                                    // object sits, and the honest (estimated) delay
                                    // before it is heard.
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.space3

                                        Repeater {
                                            model: ["x", "y", "z"]

                                            ColumnLayout {
                                                required property string modelData
                                                Layout.fillWidth: true
                                                spacing: 2

                                                Text {
                                                    text: parent.modelData
                                                    color: Theme.neutral600
                                                    font.pixelSize: 9
                                                    font.capitalization: Font.AllUppercase
                                                }
                                                Text {
                                                    text: {
                                                        const list = EncoderController.objectModel;
                                                        const sel = EncoderController.selectedObjectIndex;
                                                        let obj = null;
                                                        for (let i = 0; i < list.length; ++i) {
                                                            if (list[i].index === sel) { obj = list[i]; break; }
                                                        }
                                                        if (!obj) return "—";
                                                        return (parent.modelData === "x" ? obj.x
                                                                : parent.modelData === "y" ? obj.y
                                                                                           : obj.z).toFixed(2);
                                                    }
                                                    color: Theme.text
                                                    font.pixelSize: 13
                                                    font.family: Theme.monoFamily
                                                }
                                            }
                                        }
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 2

                                            Text {
                                                text: qsTr("LATENCY")
                                                color: Theme.neutral600
                                                font.pixelSize: 9
                                            }
                                            Text {
                                                objectName: "liveLatencyReadout"
                                                text: EncoderController.liveLatencyMeasured
                                                      ? qsTr("~%1 ms measured").arg(EncoderController.liveLatencyMs.toFixed(0))
                                                      : qsTr("~%1 ms est.").arg(EncoderController.liveLatencyMs.toFixed(0))
                                                color: Theme.text
                                                font.pixelSize: 13
                                                font.family: Theme.monoFamily
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
                                                    // Derived from the ACTUAL receiver: a
                                                    // layout is only "beyond" a leg that
                                                    // cannot take the E-AC-3 it needs. An
                                                    // E-AC-3-capable receiver bitstreams
                                                    // every one of these.
                                                    readonly property bool beyondReceiver:
                                                        EncoderController.livePassthrough
                                                        && !EncoderController.liveReceiverEac3
                                                        && modelData !== "5.1"
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

                                                        Text {
                                                            text: liveLayoutButton.modelData
                                                            font.pixelSize: 12
                                                            font.family: Theme.monoFamily
                                                            color: liveLayoutButton.active ? Theme.bg : Theme.text
                                                        }
                                                        Rectangle {
                                                            visible: liveLayoutButton.beyondReceiver
                                                            width: 6
                                                            height: 6
                                                            color: Theme.accent
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

                                        RowLayout {
                                            visible: !EncoderController.atmosEnabled
                                                     && EncoderController.livePassthrough
                                                     && !EncoderController.liveReceiverEac3
                                            Layout.fillWidth: true
                                            spacing: 6

                                            Rectangle {
                                                width: 6
                                                height: 6
                                                color: Theme.accent
                                            }
                                            Text {
                                                objectName: "liveLayoutLegendCapped"
                                                Layout.fillWidth: true
                                                // The capped downmix leg means a dotted
                                                // layout is never a dead end now - %1
                                                // still hears sound, just the 5.1 downmix
                                                // rather than the layout itself.
                                                text: qsTr("Dotted layouts encode and meter fully — %1 bitstreams Dolby Digital only, so this receiver hears a 5.1 downmix of them.")
                                                      .arg(EncoderController.liveReceiverName)
                                                color: Theme.textMuted
                                                font.pixelSize: 10
                                                font.family: Theme.monoFamily
                                                wrapMode: Text.WordWrap
                                            }
                                        }
                                        Text {
                                            objectName: "liveLayoutLegendFull"
                                            visible: !EncoderController.atmosEnabled
                                                     && EncoderController.livePassthrough
                                                     && EncoderController.liveReceiverEac3
                                            Layout.fillWidth: true
                                            text: qsTr("%1 takes Dolby Digital Plus — every layout here bitstreams as encoded.")
                                                  .arg(EncoderController.liveReceiverName)
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

                                        // The mockup's receiver-display rows: what the
                                        // front panel would say, in its own voice.
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.preferredWidth: 80
                                                text: qsTr("Format")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                objectName: "receiverReportFormat"
                                                text: !EncoderController.livePassthrough ? "—"
                                                      : EncoderController.liveDownmixLeg
                                                        ? qsTr("DOLBY DIGITAL")
                                                      : (EncoderController.atmosEnabled
                                                         || EncoderController.codecIndex === 1)
                                                        ? qsTr("DOLBY DIGITAL PLUS") : qsTr("DOLBY DIGITAL")
                                                color: Theme.text
                                                font.pixelSize: Theme.fontNormal
                                                font.family: Theme.monoFamily
                                                font.letterSpacing: 1
                                            }
                                        }
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Text {
                                                Layout.preferredWidth: 80
                                                text: qsTr("Input")
                                                color: Theme.neutral600
                                                font.pixelSize: 10
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                objectName: "receiverReportInput"
                                                text: !EncoderController.livePassthrough ? "—"
                                                      : (EncoderController.atmosEnabled
                                                         || EncoderController.liveDownmixLeg)
                                                        ? qsTr("5.1") : EncoderController.channelShapeName
                                                color: Theme.text
                                                font.pixelSize: Theme.fontNormal
                                                font.family: Theme.monoFamily
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
                                    readonly property bool cancelled: modelData.status === "cancelled"

                                    Layout.leftMargin: 16
                                    spacing: 8

                                    Rectangle {
                                        width: 8
                                        height: 8
                                        // A failure is not "still encoding" - the two used
                                        // to share the accent, which made a failed chip
                                        // read as busy at a glance.
                                        color: encoding ? Theme.accent
                                             : failed ? Theme.accent700
                                                      : Theme.neutral400
                                    }
                                    Text {
                                        font.family: Theme.monoFamily
                                        font.pixelSize: 12
                                        color: Theme.text
                                        // Terminal states say WHICH one they are - the
                                        // mockup's "14 · failed · 212 frames" - instead of
                                        // dressing a failure in a success's clothes.
                                        text: encoding
                                              ? qsTr("%1 · %2 · %3 · %4%")
                                                .arg(modelData.id).arg(modelData.filename)
                                                .arg(modelData.rateText)
                                                .arg(Math.round(EncoderController.progress * 100))
                                              : (failed || cancelled)
                                              ? qsTr("%1 · %2 · %3%4")
                                                .arg(modelData.id).arg(modelData.filename)
                                                .arg(modelData.status)
                                                .arg((modelData.framesText || "").length > 0
                                                     ? " · " + modelData.framesText : "")
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
                                        objectName: "runShowInFolder"
                                        visible: modelData.status === "done"
                                                 && (modelData.path || "").length > 0
                                        text: qsTr("Show in folder")
                                        flat: true
                                        onClicked: {
                                            const path = modelData.path;
                                            const cut = Math.max(path.lastIndexOf("/"),
                                                                 path.lastIndexOf("\\"));
                                            if (cut > 0) {
                                                Qt.openUrlExternally("file:///"
                                                                     + path.substring(0, cut));
                                            }
                                        }
                                    }
                                    Button {
                                        visible: failed
                                                 || (cancelled
                                                     && (modelData.detail || "").length > 0)
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
