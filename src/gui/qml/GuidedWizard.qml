import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window

import Ac3Forge

// Guided — the handoff's five steps (1 Audio · 2 Speakers · 3 Quality ·
// 4 Movement · 5 Where it goes), one question per step, constraints applied
// and explained. A wrapper over the SAME rules Advanced/Expert edit: there
// is no wizard draft, so anything set here is exactly what Expert would show
// for the same field — including the assignments, which live in step 1.
ColumnLayout {
    id: wizard

    objectName: "guidedWizard"
    spacing: 0

    // 'source' | 'setup' | 'quality' | 'motion' | 'output' — the prototype's
    // own step keys.
    property string currentStepKey: "source"
    // Applied here (not just in startEncodeFlow) so the "What you are about
    // to make" summary on the output step already tells the truth before
    // Encode is ever pressed - see applyGuidedLoudnessContract's own comment
    // for why tier-entry itself is too early to do this.
    onCurrentStepKeyChanged: {
        if (currentStepKey === "output" && appWindow) {
            appWindow.applyGuidedLoudnessContract();
        }
        // A new step starts reading from ITS top - scrolling down to finish
        // one step must never leave the next one opened mid-page.
        resetContentScroll();
    }

    // The enclosing ApplicationWindow, resolved once the tree exists — for
    // tier/tab jumps and for reading window state (input mode, plan line)
    // in bindings.
    property var appWindow: null
    Component.onCompleted: appWindow = requestWindow()

    // The step-content scroll's underlying Flickable, for Main.qml's
    // --smoke-shot scroll control (smokeScrollY) — the wizard owns its own
    // scroll now that its step bar and footer are pinned.
    readonly property var contentFlickable: wizardScroll.contentItem

    // Entering/leaving the room picker is a step change in everything but
    // key, so it resets the reading position the same way (the step-change
    // reset lives in onCurrentStepKeyChanged above).
    function resetContentScroll() {
        if (contentFlickable) {
            contentFlickable.contentY = 0;
        }
    }
    onRoomPickerChanged: resetContentScroll()

    readonly property var steps: [
        { key: "source", label: qsTr("Audio"),
          assistant: qsTr("Start with the audio. Everything after this follows from what you bring in."),
          next: qsTr("Next — your speakers") },
        { key: "setup", label: qsTr("Speakers"),
          assistant: qsTr("Tell me what you are playing it back on. I will set the channels to match."),
          next: qsTr("Next — quality") },
        { key: "quality", label: qsTr("Quality"),
          assistant: qsTr("A higher rate sounds better and makes a bigger file. Anything here is a valid Dolby stream."),
          next: qsTr("Next — movement") },
        { key: "motion", label: qsTr("Movement"),
          assistant: qsTr("Give each sound a path through the room, or leave everything where it sits."),
          next: qsTr("Next — where it goes") },
        { key: "output", label: qsTr("Where it goes"),
          assistant: qsTr("Say where it goes, then encode. Nothing is written until you press the button."),
          next: qsTr("Encode now") },
    ]
    readonly property int currentStepIndex: {
        for (let i = 0; i < steps.length; i++) {
            if (steps[i].key === currentStepKey) return i;
        }
        return 0;
    }
    readonly property var currentStep: steps[currentStepIndex]

    // Where the encode ends up — a file, or straight to the amplifier.
    property string dest: "file"

    // ---- item 30: the amp destination's own device pick --------------------
    // -1 means "let the auto-pick decide"; "change…" (below, step 5) sets a
    // real index to override it. Read back by Main.qml's startEncodeFlow via
    // ampDeviceIndex when dest is "amp", so the run this produces already
    // knows which receiver to play to - see EncoderController.
    // setPendingPlayDevice and item 27's run-chip Play action.
    property int ampDeviceOverride: -1
    // Whether the stream about to be encoded is E-AC-3 - the same rule
    // window.cliLine's own eac3Stream uses, computed here because nothing
    // has actually been encoded yet for outputIsEac3 to answer honestly.
    readonly property bool ampEac3: EncoderController.atmosEnabled
                                    || EncoderController.codecIndex === 1
    // The first output device that can bitstream ampEac3 - the same
    // "AC-3 + E-AC-3 ready" / "cannot bitstream" capability
    // outputDeviceSupportsFormat and the Format tab's own device labels
    // already use (docs/gui/format-and-channels.md), just asked against the
    // PROSPECTIVE plan instead of a past encode. -1 when nothing qualifies.
    readonly property int autoAmpDeviceIndex: {
        const devices = EncoderController.outputDevices;
        for (let i = 0; i < devices.length; i++) {
            if (EncoderController.outputDeviceSupportsFormat(i, wizard.ampEac3)) {
                return i;
            }
        }
        return -1;
    }
    // ampDeviceOverride when it still names a real device, else the
    // auto-pick - an override surviving a device list refresh that dropped
    // it would otherwise silently point nowhere.
    readonly property int ampDeviceIndex: (wizard.ampDeviceOverride >= 0
                                           && wizard.ampDeviceOverride
                                              < EncoderController.outputDevices.length)
                                          ? wizard.ampDeviceOverride
                                          : wizard.autoAmpDeviceIndex

    // ---- item 29: Good/Better/Best -> VBR quality when VBR is "in force" ---
    // True when a VBR default (Preferences' rate-mode default) or an
    // already-selected VBR rate mode applies to the CURRENT plan -
    // vbrAvailable gates both, since VBR means nothing for AC-3, object
    // mode or a live session regardless of either. When this is true the
    // step 3 rate cards below set a VBR quality target instead of a fixed
    // CBR bitrate.
    readonly property bool vbrQualityMode: EncoderController.vbrAvailable
        && (EncoderController.vbrEnabled
            || (wizard.appWindow !== null && wizard.appWindow.settings.defaultVbr))

    // The speakers step's sub-screen: buttons that edit what is IN THE ROOM,
    // with the bed falling out of the parts rather than being named. Open is
    // wizard-local UI state; everything it edits is controller state, so the
    // round trip through Advanced stays lossless.
    property bool roomPicker: false

    // ---- what is in the room, read back from the bed -----------------------
    readonly property string bedId: {
        const beds = EncoderController.bedChoices;
        return EncoderController.bedIndex >= 0 && EncoderController.bedIndex < beds.length
               ? beds[EncoderController.bedIndex].id : "";
    }
    readonly property bool roomFronts: bedId !== "1/0" && bedId !== "1+1"
    readonly property bool roomCentre: ["1/0", "3/0", "3/1", "3/2"].indexOf(bedId) >= 0
    readonly property string roomSurround: bedId === "2/2" || bedId === "3/2" ? "sides"
                                           : bedId === "2/1" || bedId === "3/1" ? "back"
                                           : "none"
    readonly property int roomSubs: {
        if (!EncoderController.bedLfe) return 0;
        const extras = EncoderController.extrasModel;
        for (let i = 0; i < extras.length; i++) {
            if (extras[i].id === "lfe2") return extras[i].checked ? 2 : 1;
        }
        return 1;
    }

    // The bed the room's parts add up to. No fronts collapses to the lone
    // centre; sides and back are mutually exclusive kinds of surround.
    function bedFrom(centre, surround, fronts) {
        if (!fronts) return "1/0";
        if (surround === "sides") return centre ? "3/2" : "2/2";
        if (surround === "back") return centre ? "3/1" : "2/1";
        return centre ? "3/0" : "2/0";
    }
    function setBedId(id) {
        const beds = EncoderController.bedChoices;
        for (let i = 0; i < beds.length; i++) {
            if (beds[i].id === id) {
                EncoderController.bedIndex = i;
                return;
            }
        }
    }
    function setSubCount(n) {
        const wantLfe2 = n > 1;
        const extras = EncoderController.extrasModel;
        for (let i = 0; i < extras.length; i++) {
            if (extras[i].id === "lfe2" && extras[i].checked !== wantLfe2) {
                EncoderController.toggleExtra("lfe2");
            }
        }
        EncoderController.bedLfe = n > 0;
    }

    // ---- trajectory presets (step 4) ---------------------------------------
    // Authors REAL keyframes through the same setObjectPathKeyframes the
    // Objects tab's timeline uses — a preset is a starting point for the
    // room view, not a separate motion system. Gains carry the same
    // inverse-root law the static fallback uses, so a preset never makes
    // the summed bed hotter than static placement would.
    property string traj: "hold"
    // ---- item 28: "what should move" -----------------------------------
    // Which of the two what-moves cards is the CURRENT choice - "all"
    // rewrote every loaded channel's assignment to an object (no bed left);
    // "picked" left an existing bed assignment alone and only turned
    // still-unassigned channels into objects. A card tap is an action
    // applied once, the same as traj above - a later hand-edit in the
    // assignment table can leave neither description exactly true any
    // more, which is fine; this only tracks which button was pressed last.
    property string whatMoves: ""
    // Sends every loaded (source, channel) to "obj" through the exact
    // setAssignment path the full assignment table itself uses, so the
    // result is indistinguishable from doing it by hand there - no bed
    // position survives.
    function setWhatMovesAll() {
        wizard.whatMoves = "all";
        const sources = EncoderController.sourceModel;
        for (let s = 0; s < sources.length; s++) {
            for (let c = 0; c < sources[s].channels; c++) {
                EncoderController.setAssignment(s, c, "obj");
            }
        }
    }
    // Leaves every row that already has a real destination (a bed position
    // from step 1, or an object from an earlier pass here) untouched, and
    // sends only "none"/untouched rows - the ones a newly loaded file's
    // channels start as - to "obj". A no-op when everything is already
    // assigned, which is exactly right: there is nothing new to pick up.
    function setWhatMovesPicked() {
        wizard.whatMoves = "picked";
        const rows = EncoderController.assignmentRows;
        for (let i = 0; i < rows.length; i++) {
            if (rows[i].destToken === "none" && rows[i].touched !== true) {
                EncoderController.setAssignment(rows[i].source, rows[i].channel, "obj");
            }
        }
    }
    // The step-4 preview's clock: loops the authored paths so the movement
    // card is watched, not imagined. Runs only while the step is on screen.
    property real previewT: 0
    // Resolved fresh through requestWindow() each time, rather than reading
    // the Component.onCompleted-cached appWindow: that walk runs once,
    // early, and is not guaranteed to already see a fully-parented tree at
    // that point - a lazy re-walk here is what actually gets the settled
    // answer.
    readonly property bool liveSession: {
        const win = requestWindow();
        return win !== null && win.inputMode === "live";
    }
    // One lap/cycle's own length - what a preset repeats every this-many
    // seconds, whether authored across a whole file or looped live.
    readonly property real cycleSeconds: 8
    // Mirrors the Objects tab's own timelineLength formula (Main.qml) -
    // max(offset + duration) over every loaded source, falling back to
    // cycleSeconds when nothing is derivable yet. A live session has no
    // fixed programme length to author across at all - see
    // authorTrajectories' own comment on why it uses cycleSeconds instead.
    function derivedDuration() {
        let end = 0;
        const sources = EncoderController.sourceModel;
        for (let i = 0; i < sources.length; i++) {
            const at = sources[i].offsetSeconds + sources[i].seconds;
            if (at > end) end = at;
        }
        return end > 0 ? end : cycleSeconds;
    }
    Timer {
        interval: 50
        repeat: true
        running: wizard.currentStepKey === "motion" && wizard.visible
                 && EncoderController.atmosEnabled && EncoderController.objectCount > 0
                 && wizard.traj !== "hold"
        onTriggered: {
            const loop = wizard.liveSession ? wizard.cycleSeconds : wizard.derivedDuration();
            wizard.previewT = (wizard.previewT + 0.05) % loop;
        }
    }
    function authorTrajectories(kind) {
        traj = kind;
        const n = EncoderController.objectCount;
        if (n <= 0) return;
        const scale = 1 / Math.sqrt(Math.max(1, n));
        const objects = EncoderController.objectModel;
        // A live session has no fixed programme length to author across -
        // the preset loops one fixed-length cycle instead, for as long as
        // the session runs. A file source authors keys across its own
        // derived length, in whole laps/cycles: the phase below wraps
        // every cycleSeconds, so the pattern repeats seamlessly rather
        // than the last lap being cut off mid-turn or stretched thin to
        // fit.
        const duration = wizard.liveSession ? wizard.cycleSeconds : wizard.derivedDuration();
        for (let i = 0; i < n; i++) {
            if (kind === "hold") {
                EncoderController.clearObjectPath(i);
                continue;
            }
            const obj = objects[i];
            const keyAt = (t) => {
                let x = obj.x;
                let y = obj.y;
                let z = obj.z;
                const phase = (t % wizard.cycleSeconds) / wizard.cycleSeconds;
                if (kind === "orbit") {
                    // One full lap per cycle, each object offset around the
                    // circle so they never bunch up.
                    const angle = 2 * Math.PI * phase + 2 * Math.PI * i / n;
                    x = 0.5 + 0.35 * Math.cos(angle);
                    y = 0.5 + 0.35 * Math.sin(angle);
                    z = 0;
                } else if (kind === "lift") {
                    // Floor-ish to the ceiling and back — height is the
                    // point, so x/y hold the object's own place.
                    z = Math.min(1, -0.2 + 1.2 * Math.sin(Math.PI * phase));
                }
                return { time: t, x: x, y: y, z: z,
                         gain: 0.7 * scale, lfeSend: obj.lfeSend * scale };
            };
            const keys = [];
            for (let t = 0; t < duration; t++) {
                keys.push(keyAt(t));
            }
            // Always closes exactly at the derived length, even when it is
            // not a whole number of seconds - the last lap never cuts off.
            keys.push(keyAt(duration));
            EncoderController.setObjectPathKeyframes(i, keys, kind);
        }
    }

    // The setup cards read the CURRENT channel state back, so an edit made
    // in Advanced round-trips into Guided instead of the card lying.
    readonly property string setupSignature: {
        const extras = EncoderController.extrasModel;
        const on = [];
        for (let i = 0; i < extras.length; i++) {
            if (extras[i].checked) on.push(extras[i].id);
        }
        on.sort();
        const beds = EncoderController.bedChoices;
        const bed = EncoderController.bedIndex >= 0 && EncoderController.bedIndex < beds.length
                    ? beds[EncoderController.bedIndex].id : "";
        return bed + "|" + (EncoderController.bedLfe ? 1 : 0) + "|" + on.join(",");
    }

    function requestWindow() {
        // The enclosing ApplicationWindow, for tier/tab jumps. Item.parent
        // does NOT reliably chain up to it - content declared inside
        // ApplicationWindow's default property is hosted under an
        // internal contentItem whose own .parent is not exposed, so a
        // manual walk (this used to be one) dead-ends at null before ever
        // reaching the window. The Window attached property is what
        // actually answers "which window is this item shown in", for any
        // item, regardless of that.
        return wizard.Window.window;
    }

    // ---- step bar -----------------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: Theme.space3
        Layout.bottomMargin: Theme.space3
        spacing: 0

        Repeater {
            model: wizard.steps

            delegate: RowLayout {
                id: stepEntry
                required property var modelData
                required property int index
                readonly property bool current: index === wizard.currentStepIndex
                readonly property bool completed: index < wizard.currentStepIndex

                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    objectName: "wizardStepDot-" + stepEntry.modelData.key
                    width: 22
                    height: 22
                    color: stepEntry.current ? Theme.accent
                           : stepEntry.completed ? Theme.text : "transparent"
                    border.color: stepEntry.current ? Theme.accent : Theme.divider
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: stepEntry.index + 1
                        font.pixelSize: 11
                        font.family: Theme.monoFamily
                        color: stepEntry.current || stepEntry.completed ? Theme.bg : Theme.text
                    }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: wizard.currentStepKey = stepEntry.modelData.key
                    }
                }
                Text {
                    text: stepEntry.modelData.label
                    font.pixelSize: 13
                    font.weight: stepEntry.current ? Font.DemiBold : Font.Normal
                    color: Theme.text
                    opacity: stepEntry.current ? 1.0 : 0.6
                }
                Rectangle {
                    visible: stepEntry.index < wizard.steps.length - 1
                    Layout.fillWidth: true
                    Layout.leftMargin: 6
                    Layout.rightMargin: 14
                    Layout.preferredHeight: 1
                    color: Theme.divider
                }
            }
        }
    }
    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

    // ---- step content, in its own scroll --------------------------------------
    // Only THIS scrolls. The step bar above and the assistant/Back/Next
    // footer below stay pinned, so the way forward is always on screen -
    // the footer used to live at the bottom of the whole page's scroll
    // range and could sit a full screen below short step content.
    ScrollView {
        id: wizardScroll
        objectName: "wizardScroll"
        Layout.fillWidth: true
        Layout.fillHeight: true
        contentWidth: availableWidth

        ColumnLayout {
            width: parent ? parent.width : 0
            spacing: 0

    // ---- step content ---------------------------------------------------------
    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: Theme.space4
        spacing: Theme.space4

        // ================= 1 · Audio =================
        ColumnLayout {
            visible: wizard.currentStepKey === "source"
            Layout.fillWidth: true
            spacing: Theme.space4

            Text {
                text: qsTr("Where is the audio coming from?")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.ExtraBold
                color: Theme.text
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("A file you already have, or whatever is playing on this machine right now. You can listen before committing to anything.")
                wrapMode: Text.WordWrap
                font.pixelSize: 14
                color: Theme.neutral700
            }

            // The two source cards.
            Rectangle {
                objectName: "wizardSourceFile"
                // The SELECTED branch highlights - the live card lights up
                // when live is the input, not never.
                readonly property bool active: EncoderController.sourceModel.length > 0
                                               && (!wizard.appWindow
                                                   || wizard.appWindow.inputMode === "file")
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                color: "transparent"
                border.color: active ? Theme.text : Theme.divider
                border.width: active ? 2 : 1

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    color: parent.active ? Theme.accent : "transparent"
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space4
                    anchors.rightMargin: Theme.space4
                    spacing: Theme.space3

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: qsTr("Files on this computer")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("One WAV of any width, or several — a 5.1 mix plus the separate sounds you want moving over it.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                    }
                    Text {
                        text: EncoderController.sourceModel.length > 0
                              ? qsTr("%1 file%2").arg(EncoderController.sourceModel.length)
                                .arg(EncoderController.sourceModel.length === 1 ? "" : qsTr("s"))
                              : qsTr("choose…")
                        font.pixelSize: 12
                        font.family: Theme.monoFamily
                        color: Theme.accent700
                    }
                }
                TapHandler {
                    onTapped: {
                        const win = wizard.requestWindow();
                        if (EncoderController.sourceModel.length === 0) {
                            if (win) win.inputMode = "file";
                        }
                        // Either way the dialogs live on the window.
                        if (win) {
                            win.inputMode = "file";
                        }
                        wizardOpenDialog.open();
                    }
                }
            }

            Rectangle {
                objectName: "wizardSourceLive"
                readonly property bool active: wizard.appWindow !== null
                                               && wizard.appWindow.inputMode === "live"
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                color: "transparent"
                border.color: active ? Theme.text : Theme.divider
                border.width: active ? 2 : 1

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    color: parent.active ? Theme.accent : "transparent"
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: Theme.space4
                    anchors.rightMargin: Theme.space4
                    spacing: Theme.space3

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            text: qsTr("Whatever is playing right now")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Captures this machine's own output. Nothing is recorded until you say so.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                    }
                }
                TapHandler {
                    onTapped: {
                        const win = wizard.requestWindow();
                        if (win) {
                            win.everHadSource = true;
                            win.inputMode = "live";
                        }
                    }
                }
            }

            // What each sound does — the assignments, in guided.
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
            RowLayout {
                Layout.fillWidth: true
                visible: EncoderController.sourceModel.length > 0
                spacing: Theme.space3

                Text {
                    text: qsTr("What each sound does")
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Set it here, or leave it — anything unset is flagged before you encode.")
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    color: Theme.textMuted
                }
            }

            AssignmentPanel {
                visible: EncoderController.sourceModel.length > 0
                Layout.fillWidth: true
                compact: true
            }

            RowLayout {
                Layout.fillWidth: true
                visible: EncoderController.sourceModel.length > 0
                spacing: Theme.space3

                Text {
                    Layout.fillWidth: true
                    text: qsTr("The meters on the left follow these choices — if they move, you are good.")
                    font.pixelSize: 12
                    color: Theme.textMuted
                }
                Text {
                    objectName: "wizardOpenAssignments"
                    text: qsTr("Open the full assignment table →")
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: Theme.accent700
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const win = wizard.requestWindow();
                            if (win) win.goAssign();
                        }
                    }
                }
            }
        }

        // ================= 2 · Speakers =================
        ColumnLayout {
            visible: wizard.currentStepKey === "setup" && !wizard.roomPicker
            Layout.fillWidth: true
            spacing: Theme.space4

            Text {
                text: qsTr("What are you playing it back on?")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.ExtraBold
                color: Theme.text
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Pick the room you actually have. This sets the channels — you never have to name them yourself.")
                wrapMode: Text.WordWrap
                font.pixelSize: 15
                color: Theme.neutral700
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.space3
                rowSpacing: Theme.space3

                Repeater {
                    model: [
                        { key: "stereo", preset: "stereo", signature: "2/0|0|",
                          title: qsTr("A TV or soundbar"),
                          body: qsTr("Two speakers, no subwoofer of its own."),
                          detail: qsTr("2.0 · L R") },
                        { key: "home", preset: "5.1", signature: "3/2|1|",
                          title: qsTr("Home cinema, five speakers and a sub"),
                          body: qsTr("The common one. Nothing on the ceiling."),
                          detail: qsTr("5.1 · L C R Ls Rs LFE") },
                        { key: "atmos", preset: "5.1.4", signature: "3/2|1|topf,topr",
                          title: qsTr("…plus speakers overhead"),
                          body: qsTr("Four in the ceiling, or upward-firing modules."),
                          detail: qsTr("5.1.4 · + 4 ceiling") },
                        { key: "full", preset: "7.1.4", signature: "3/2|1|rear,topf,topr",
                          title: qsTr("The full room"),
                          body: qsTr("Sides and rears at ear level, four overhead."),
                          detail: qsTr("7.1.4 · 12 speakers") },
                    ]
                    delegate: Rectangle {
                        id: setupCard
                        required property var modelData
                        readonly property bool active: wizard.setupSignature === modelData.signature

                        objectName: "wizardSetup-" + modelData.key
                        Layout.fillWidth: true
                        Layout.preferredHeight: 100
                        color: active ? Theme.accent100 : "transparent"
                        border.color: active ? Theme.accent : Theme.divider
                        border.width: active ? 2 : 1
                        opacity: EncoderController.atmosEnabled || EncoderController.busy ? 0.4 : 1.0

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.space3
                            spacing: 2

                            Text {
                                text: setupCard.modelData.title
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                color: Theme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                text: setupCard.modelData.body
                                wrapMode: Text.WordWrap
                                font.pixelSize: 12
                                color: Theme.neutral700
                            }
                            Item { Layout.fillHeight: true }
                            Text {
                                text: setupCard.modelData.detail
                                font.pixelSize: 11
                                font.family: Theme.monoFamily
                                color: Theme.neutral600
                            }
                        }
                        TapHandler {
                            enabled: !EncoderController.atmosEnabled && !EncoderController.busy
                            onTapped: EncoderController.applyChannelPreset(setupCard.modelData.preset)
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Currently building %1 · %2 speakers. The plan above the tabs always shows what you will get.")
                      .arg(EncoderController.channelShapeName)
                      .arg(EncoderController.renderedChannelCount)
                font.pixelSize: 12
                color: Theme.neutral700
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space6

                Text {
                    objectName: "wizardRoomPickerLink"
                    text: qsTr("Pick speakers myself →")
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: Theme.accent700
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: wizard.roomPicker = true
                    }
                }
                Text {
                    objectName: "wizardEverythingLink"
                    text: qsTr("Everything, in channel names →")
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: Theme.accent700
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const win = wizard.requestWindow();
                            if (win) win.goAssign();
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }
        }

        // ---- the room picker: the speakers step, part by part --------------
        ColumnLayout {
            visible: wizard.currentStepKey === "setup" && wizard.roomPicker
            Layout.fillWidth: true
            spacing: Theme.space4

            readonly property bool roomLocked: EncoderController.atmosEnabled
                                               || EncoderController.busy
                                               || EncoderController.dualMono

            id: roomPickerScreen

            Text {
                text: qsTr("Tap what is in your room.")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.ExtraBold
                color: Theme.text
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Front left and right are always there. Everything else is yours to add — pairs go in together, because half a pair does not exist.")
                wrapMode: Text.WordWrap
                font.pixelSize: 14
                color: Theme.neutral700
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.space4
                rowSpacing: Theme.space3
                enabled: !roomPickerScreen.roomLocked

                Text { text: qsTr("A pair of front speakers"); font.pixelSize: 13; font.weight: Font.DemiBold; color: Theme.text }
                SegmentedControl {
                    objectName: "roomFronts"
                    model: [ { value: "on", label: qsTr("Yes") }, { value: "off", label: qsTr("Centre only") } ]
                    currentValue: wizard.roomFronts ? "on" : "off"
                    onSelected: (value) => {
                        if (value === "off") {
                            // No fronts collapses the room to the lone
                            // centre; extras made sense relative to a wider
                            // bed, so they go with it.
                            const extras = EncoderController.extrasModel;
                            for (let i = 0; i < extras.length; i++) {
                                if (extras[i].checked) EncoderController.toggleExtra(extras[i].id);
                            }
                            wizard.setBedId("1/0");
                        } else {
                            wizard.setBedId(wizard.bedFrom(wizard.roomCentre, "none", true));
                        }
                    }
                }

                Text {
                    text: qsTr("A centre speaker")
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Theme.text
                    opacity: wizard.roomFronts ? 1.0 : 0.4
                }
                SegmentedControl {
                    objectName: "roomCentre"
                    // A centre-only room IS the centre; only fronts make this
                    // a real choice.
                    enabled: wizard.roomFronts
                    opacity: wizard.roomFronts ? 1.0 : 0.4
                    model: [ { value: "on", label: qsTr("Yes") }, { value: "off", label: qsTr("No") } ]
                    currentValue: wizard.roomCentre ? "on" : "off"
                    onSelected: (value) => wizard.setBedId(
                                    wizard.bedFrom(value === "on", wizard.roomSurround, true))
                }

                Text {
                    text: qsTr("Surround speakers")
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Theme.text
                    opacity: wizard.roomFronts ? 1.0 : 0.4
                }
                SegmentedControl {
                    objectName: "roomSurround"
                    enabled: wizard.roomFronts
                    opacity: wizard.roomFronts ? 1.0 : 0.4
                    model: [
                        { value: "none", label: qsTr("None") },
                        { value: "sides", label: qsTr("At your sides") },
                        { value: "back", label: qsTr("One at the back") },
                    ]
                    currentValue: wizard.roomSurround
                    onSelected: (value) => wizard.setBedId(
                                    wizard.bedFrom(wizard.roomCentre, value, true))
                }

                Text { text: qsTr("A subwoofer"); font.pixelSize: 13; font.weight: Font.DemiBold; color: Theme.text }
                SegmentedControl {
                    objectName: "roomSubs"
                    model: [
                        { value: "0", label: qsTr("No sub") },
                        { value: "1", label: qsTr("One sub") },
                        { value: "2", label: qsTr("Two subs") },
                    ]
                    currentValue: String(wizard.roomSubs)
                    onSelected: (value) => wizard.setSubCount(parseInt(value))
                }
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("Two subs means two independent low-frequency channels carrying different signal — and, like everything below, it needs Dolby Digital Plus, which the codec follows on its own.")
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.textMuted
            }

            // The extras, in room language. Same rows, same rules, same
            // reasons as the Format tab — only the words are the room's.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.space2

                Text {
                    text: qsTr("Anything overhead or extra")
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: qsTr("%1 of %2 · %3")
                          .arg(EncoderController.channelBudgetUsed)
                          .arg(EncoderController.channelBudgetMax)
                          .arg(EncoderController.channelShapeName)
                    font.pixelSize: 11
                    font.family: Theme.monoFamily
                    color: Theme.neutral700
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0
                enabled: !roomPickerScreen.roomLocked

                Repeater {
                    model: EncoderController.extrasModel

                    delegate: ColumnLayout {
                        id: roomExtraRow
                        required property var modelData
                        readonly property var roomLabels: ({
                            wide: qsTr("A wide pair outside the fronts"),
                            rear: qsTr("A pair behind you"),
                            topf: qsTr("Two above the front"),
                            topr: qsTr("Two above the back"),
                        })
                        visible: modelData.id !== "lfe2"
                        Layout.fillWidth: true
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 6
                            Layout.bottomMargin: 6
                            spacing: Theme.space3
                            opacity: roomExtraRow.modelData.enabled ? 1.0 : 0.4

                            CheckBox {
                                objectName: "roomExtra-" + roomExtraRow.modelData.id
                                checked: roomExtraRow.modelData.checked
                                enabled: roomExtraRow.modelData.enabled && !EncoderController.busy
                                onToggled: EncoderController.toggleExtra(roomExtraRow.modelData.id)
                            }
                            Text {
                                Layout.fillWidth: true
                                text: roomExtraRow.roomLabels[roomExtraRow.modelData.id]
                                      || roomExtraRow.modelData.label
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                color: Theme.text
                            }
                            Text {
                                // The Format tab's promotion hint applies here
                                // too: an unticked extra under plain AC-3 will
                                // move the stream to Dolby Digital Plus.
                                text: {
                                    if (roomExtraRow.modelData.reason.length > 0) {
                                        return roomExtraRow.modelData.reason;
                                    }
                                    if (!roomExtraRow.modelData.checked
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
                        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.neutral200 }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3

                Text {
                    text: qsTr("This room is a %1.").arg(EncoderController.channelShapeName)
                    font.pixelSize: 15
                    font.family: Theme.headingFamily
                    font.weight: Font.ExtraBold
                    color: Theme.text
                }
                Item { Layout.fillWidth: true }
                Button {
                    objectName: "roomBackToPresets"
                    text: qsTr("Back to the presets")
                    onClicked: wizard.roomPicker = false
                }
                Text {
                    text: qsTr("Everything, in channel names →")
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: Theme.accent700
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            const win = wizard.requestWindow();
                            if (win) win.goAssign();
                        }
                    }
                }
            }

            Text {
                visible: roomPickerScreen.roomLocked
                Layout.fillWidth: true
                text: EncoderController.atmosEnabled
                      ? qsTr("Movement is on, so the room is fixed at 5.1 — objects carry the height instead of ceiling speakers.")
                      : qsTr("Dual mono has no room to pick — it is two programmes, not a soundstage.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.accent700
            }
        }

        // ================= 3 · Quality =================
        ColumnLayout {
            visible: wizard.currentStepKey === "quality"
            Layout.fillWidth: true
            spacing: Theme.space4

            Text {
                text: qsTr("How good should it sound?")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.ExtraBold
                color: Theme.text
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("All three make a real Dolby stream any receiver will play. The difference is file size and how much detail survives.")
                wrapMode: Text.WordWrap
                font.pixelSize: 15
                color: Theme.neutral700
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3

                Repeater {
                    model: [
                        { rate: 192, vbrQuality: 40, title: qsTr("Good"),
                          body: qsTr("Fine for speech, streaming and anything stereo."),
                          detail: qsTr("192 kbps · 24 KB/s") },
                        { rate: 448, vbrQuality: 75, title: qsTr("Better"),
                          body: qsTr("What a DVD carries. A sensible default for 5.1."),
                          detail: qsTr("448 kbps · 56 KB/s") },
                        { rate: 768, vbrQuality: 90, title: qsTr("Best"),
                          body: qsTr("Worth it for wide rooms, ceiling channels and objects."),
                          detail: qsTr("768 kbps · 96 KB/s") },
                    ]
                    delegate: Rectangle {
                        id: rateCard
                        required property var modelData
                        // Under vbrQualityMode, active reads the quality
                        // target these cards actually set instead of the
                        // fixed bitrate - see the TapHandler below.
                        readonly property bool active: wizard.vbrQualityMode
                            ? (EncoderController.vbrEnabled
                               && EncoderController.vbrQuality === modelData.vbrQuality)
                            : (!EncoderController.vbrEnabled
                               && EncoderController.bitrateKbps === modelData.rate)

                        objectName: "wizardRate-" + modelData.rate
                        Layout.fillWidth: true
                        Layout.preferredHeight: 96
                        color: active ? Theme.accent100 : "transparent"
                        border.color: active ? Theme.accent : Theme.divider
                        border.width: active ? 2 : 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.space3
                            spacing: 2

                            Text {
                                text: rateCard.modelData.title
                                font.pixelSize: 18
                                font.family: Theme.headingFamily
                                font.weight: Font.ExtraBold
                                color: Theme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                text: rateCard.modelData.body
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.neutral700
                            }
                            Item { Layout.fillHeight: true }
                            Text {
                                text: wizard.vbrQualityMode
                                      ? qsTr("VBR quality %1").arg(rateCard.modelData.vbrQuality)
                                      : rateCard.modelData.detail
                                font.pixelSize: 11
                                font.family: Theme.monoFamily
                                color: Theme.neutral600
                            }
                        }
                        TapHandler {
                            enabled: !EncoderController.busy
                            onTapped: {
                                EncoderController.formatDefaultsTouched = true;
                                // Still the coupling/spx band-edge reference
                                // even under VBR (EncoderController.
                                // bitrateKbps' own doc comment) - a card tap
                                // sets it either way.
                                EncoderController.bitrateKbps = rateCard.modelData.rate;
                                if (wizard.vbrQualityMode) {
                                    EncoderController.vbrEnabled = true;
                                    EncoderController.vbrQuality = rateCard.modelData.vbrQuality;
                                }
                            }
                        }
                    }
                }
            }

            Text {
                visible: EncoderController.atmosEnabled && EncoderController.bitrateKbps < 384
                Layout.fillWidth: true
                text: qsTr("Objects over a 5.1 bed want 384 kbps or better — the metadata competes with the audio for the same frame.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.accent700
            }

            // The Main.qml advisory's plain-language cousin: a wide room (more
            // full-bandwidth channels than a plain 5.1 bed) can make even
            // "Good" too little for what it is being asked to carry.
            Text {
                objectName: "wizardBitrateFloorAdvisory"
                visible: !EncoderController.atmosEnabled
                         && EncoderController.bitrateKbps === 192
                         && EncoderController.fullBandwidthCodedChannelCount
                            * EncoderController.kbpsPerChannelFloor > 192
                Layout.fillWidth: true
                text: qsTr("A wide room asks more of Good than 192 kbps can give it — the encoder will refuse outright long before it sounds right.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.accent700
            }
        }

        // ================= 4 · Movement =================
        ColumnLayout {
            visible: wizard.currentStepKey === "motion"
            Layout.fillWidth: true
            spacing: Theme.space4

            Text {
                text: qsTr("Should anything move?")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.ExtraBold
                color: Theme.text
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Sounds can sit in fixed speakers, or travel through the room as objects. Pick a movement and watch it in the room below.")
                wrapMode: Text.WordWrap
                font.pixelSize: 15
                color: Theme.neutral700
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3

                Rectangle {
                    id: stayCard
                    objectName: "wizardMotion-off"
                    readonly property bool active: !EncoderController.atmosEnabled
                    Layout.fillWidth: true
                    Layout.preferredHeight: 110
                    color: active ? Theme.accent100 : "transparent"
                    border.color: active ? Theme.accent : Theme.divider
                    border.width: active ? 2 : 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.space3
                        spacing: 2

                        Text {
                            text: qsTr("Stay put")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Every sound keeps the speaker the assignments gave it. A plain channel bed — smallest, simplest, plays everywhere.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                    }
                    TapHandler {
                        enabled: !EncoderController.busy
                        onTapped: {
                            EncoderController.atmosEnabled = false;
                            wizard.traj = "hold";
                            wizard.whatMoves = "";
                        }
                    }
                }

                Rectangle {
                    id: moveCard
                    objectName: "wizardMotion-on"
                    readonly property bool active: EncoderController.atmosEnabled
                    Layout.fillWidth: true
                    Layout.preferredHeight: 110
                    color: active ? Theme.accent100 : "transparent"
                    border.color: active ? Theme.accent : Theme.divider
                    border.width: active ? 2 : 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.space3
                        spacing: 2

                        Text {
                            text: qsTr("Move around the room")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Sounds become Dolby Atmos objects with a place — and a path — in the room. Fixes the stream at Dolby Digital Plus over a 5.1 bed and raises the rate to at least 384 kbps.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                    }
                    TapHandler {
                        enabled: !EncoderController.busy
                        onTapped: {
                            if (!EncoderController.atmosEnabled) {
                                EncoderController.applyChannelPreset("5.1");
                                EncoderController.atmosEnabled = true;
                                if (EncoderController.bitrateKbps < 384) {
                                    EncoderController.bitrateKbps = 384;
                                }
                            }
                        }
                    }
                }
            }

            Text {
                visible: EncoderController.atmosEnabled
                Layout.fillWidth: true
                text: qsTr("Adding objects auto-selected the 5.1 bed and E-AC-3 — that is the format objects ride in, not a preference.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.textMuted
            }

            // ---- what should move (item 28) - object mode only -------------
            // Rewrites the assignment table (the same one step 1's "What each
            // sound does" and the full table edit) rather than inventing a
            // second notion of "which channels are objects" - dynamicObjectChannels()
            // already reads exactly this, so a choice made here is visible
            // and editable everywhere the assignment already is.
            ColumnLayout {
                visible: EncoderController.atmosEnabled
                Layout.fillWidth: true
                spacing: Theme.space2

                Text {
                    text: qsTr("WHAT SHOULD MOVE")
                    font.pixelSize: 10
                    font.letterSpacing: 1.5
                    color: Theme.textMuted
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space3

                    Rectangle {
                        id: allMoveCard
                        objectName: "wizardWhatMoves-all"
                        readonly property bool active: wizard.whatMoves === "all"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 88
                        color: active ? Theme.accent100 : "transparent"
                        border.color: active ? Theme.accent : Theme.divider
                        border.width: active ? 2 : 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.space3
                            spacing: 2

                            Text {
                                text: qsTr("Everything moves")
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                color: Theme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Every loaded channel becomes its own object — there is no bed left underneath them.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.neutral700
                            }
                        }
                        TapHandler {
                            enabled: !EncoderController.busy
                            onTapped: wizard.setWhatMovesAll()
                        }
                    }

                    Rectangle {
                        id: pickedMoveCard
                        objectName: "wizardWhatMoves-picked"
                        readonly property bool active: wizard.whatMoves === "picked"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 88
                        color: active ? Theme.accent100 : "transparent"
                        border.color: active ? Theme.accent : Theme.divider
                        border.width: active ? 2 : 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.margins: Theme.space3
                            spacing: 2

                            Text {
                                text: qsTr("Keep the bed, add movers")
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                color: Theme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Leaves an existing mix exactly where it is; only channels not yet assigned to anything — a file added since — become objects.")
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.neutral700
                            }
                        }
                        TapHandler {
                            enabled: !EncoderController.busy
                            onTapped: wizard.setWhatMovesPicked()
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("This edits the same per-channel destinations step 1's assignment table shows — change any one there afterwards and it sticks.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                }
            }

            // Trajectory presets — real keyframes through the same API the
            // Objects tab's timeline edits, so the room view shows exactly
            // what these authored and every key can be refined there.
            ColumnLayout {
                visible: EncoderController.atmosEnabled && EncoderController.objectCount > 0
                Layout.fillWidth: true
                spacing: Theme.space2

                Text {
                    text: qsTr("GIVE THEM A PATH")
                    font.pixelSize: 10
                    font.letterSpacing: 1.5
                    color: Theme.textMuted
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space2

                    Repeater {
                        // orbit/lift's body names how the preset actually
                        // scales: whole laps/cycles across the derived
                        // programme length for a file source, or a fixed
                        // loop for a live session with no such length -
                        // see authorTrajectories' own comment.
                        model: [
                            { key: "hold", title: qsTr("Stay put"),
                              body: qsTr("Every sound holds its speaker position.") },
                            { key: "orbit", title: qsTr("Circle the room"),
                              body: wizard.liveSession
                                    ? qsTr("One lap every eight seconds, looping for as long as the session runs.")
                                    : qsTr("One lap every eight seconds, for the length of the programme.") },
                            { key: "lift", title: qsTr("Lift overhead"),
                              body: wizard.liveSession
                                    ? qsTr("Rises and falls every eight seconds, looping for as long as the session runs.")
                                    : qsTr("Rises and falls every eight seconds, for the length of the programme.") },
                            { key: "custom", title: qsTr("Place them myself"),
                              body: qsTr("Opens the room and the timeline, one object at a time. (Advanced →)") },
                        ]
                        delegate: Rectangle {
                            id: trajCard
                            required property var modelData
                            readonly property bool active: wizard.traj === modelData.key

                            objectName: "wizardTraj-" + modelData.key
                            Layout.fillWidth: true
                            Layout.preferredHeight: 84
                            color: active ? Theme.accent100 : "transparent"
                            border.color: active ? Theme.accent : Theme.divider
                            border.width: active ? 2 : 1

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: Theme.space3
                                spacing: 2

                                Text {
                                    text: trajCard.modelData.title
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    color: Theme.text
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: trajCard.modelData.body
                                    wrapMode: Text.WordWrap
                                    font.pixelSize: 11
                                    color: Theme.neutral700
                                }
                            }
                            TapHandler {
                                enabled: !EncoderController.busy
                                onTapped: {
                                    if (trajCard.modelData.key === "custom") {
                                        // The fourth card IS the Advanced jump -
                                        // the room and the timeline, per object.
                                        const win = wizard.requestWindow();
                                        if (win) {
                                            win.fromGuided = true;
                                            win.tier = "advanced";
                                            win.currentTab = "objects";
                                        }
                                        return;
                                    }
                                    wizard.authorTrajectories(trajCard.modelData.key);
                                }
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("A preset is a starting point: every key it writes is on the Objects tab's timeline, where paths are refined one object at a time.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                }

                // ---- preview: the paths, playing on loop ----------------
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.space2
                    spacing: Theme.space4
                    visible: wizard.traj !== "hold"

                    ColumnLayout {
                        spacing: 4

                        Text {
                            text: qsTr("PREVIEW — PLAN")
                            font.pixelSize: 10
                            font.letterSpacing: 1
                            color: Theme.textMuted
                        }
                        Rectangle {
                            id: wizardPreviewRoom
                            Layout.preferredWidth: 150
                            Layout.preferredHeight: 150
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

                            Repeater {
                                model: EncoderController.objectCount

                                Rectangle {
                                    required property int index
                                    readonly property var pos:
                                        EncoderController.evaluateObjectPath(index, wizard.previewT)
                                    width: 8
                                    height: 8
                                    color: Theme.neutral800
                                    x: (pos ? pos.x : 0.5) * wizardPreviewRoom.width - 4
                                    y: (pos ? pos.y : 0.5) * wizardPreviewRoom.height - 4
                                }
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        text: wizard.liveSession
                              ? qsTr("%1 objects on their paths, looping every eight seconds — the room on the left meters the same encode.")
                                    .arg(EncoderController.objectCount)
                              : qsTr("%1 objects on their paths across the programme — the room on the left meters the same encode.")
                                    .arg(EncoderController.objectCount)
                        wrapMode: Text.WordWrap
                        font.pixelSize: 12
                        color: Theme.neutral700
                    }
                }
            }
        }

        // ================= 5 · Where it goes =================
        ColumnLayout {
            visible: wizard.currentStepKey === "output"
            Layout.fillWidth: true
            spacing: Theme.space4

            Text {
                text: qsTr("Where should it go?")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.ExtraBold
                color: Theme.text
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Save it as a file, or send it straight to a receiver and listen on the real thing.")
                wrapMode: Text.WordWrap
                font.pixelSize: 15
                color: Theme.neutral700
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3

                Rectangle {
                    objectName: "wizardDest-file"
                    readonly property bool active: wizard.dest === "file"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 104
                    color: active ? Theme.accent100 : "transparent"
                    border.color: active ? Theme.accent : Theme.divider
                    border.width: active ? 2 : 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.space3
                        spacing: 2

                        Text {
                            text: qsTr("Save a file")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Keeps everything — every channel and every object move.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                        Item { Layout.fillHeight: true }
                        Text {
                            text: {
                                void EncoderController.sourcePath;
                                void EncoderController.codecIndex;
                                void EncoderController.atmosEnabled;
                                void EncoderController.containerIndex;
                                return EncoderController.suggestedOutputName();
                            }
                            font.pixelSize: 11
                            font.family: Theme.monoFamily
                            color: Theme.neutral600
                        }
                    }
                    TapHandler { onTapped: wizard.dest = "file" }
                }

                Rectangle {
                    objectName: "wizardDest-amp"
                    readonly property bool active: wizard.dest === "amp"
                    Layout.fillWidth: true
                    Layout.preferredHeight: 104
                    color: active ? Theme.accent100 : "transparent"
                    border.color: active ? Theme.accent : Theme.divider
                    border.width: active ? 2 : 1

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: Theme.space3
                        spacing: 2

                        Text {
                            text: qsTr("Play it on my receiver")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Encodes the same file, then bitstreams it over HDMI as IEC 61937 bursts — the run strip's Play does the sending.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                        Item { Layout.fillHeight: true }
                        Text {
                            objectName: "wizardAmpDevicePreview"
                            // The first PASSTHROUGH-CAPABLE endpoint for THIS
                            // stream (item 30), not just the first enumerated
                            // one - a device that cannot bitstream the format
                            // about to be encoded is not a real answer to
                            // "where does this go".
                            text: EncoderController.outputDevices.length === 0
                                  ? qsTr("no output device found")
                                  : wizard.ampDeviceIndex >= 0
                                    ? EncoderController.outputDevices[wizard.ampDeviceIndex]
                                    : qsTr("no device here can take this stream")
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                            font.pixelSize: 11
                            font.family: Theme.monoFamily
                            color: wizard.ampDeviceIndex >= 0 ? Theme.neutral600 : Theme.accent700
                        }
                    }
                    TapHandler { onTapped: wizard.dest = "amp" }
                }
            }

            // "change…" - overrides the auto-pick above with a specific
            // device (item 30). Sits outside the amp card's own TapHandler
            // (which selects the destination itself) so a tap here cannot
            // also register as one on the card underneath it.
            RowLayout {
                Layout.fillWidth: true
                visible: wizard.dest === "amp" && EncoderController.outputDevices.length > 1
                spacing: Theme.space2

                Text {
                    objectName: "wizardAmpDeviceChange"
                    text: qsTr("Choose a different device →")
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: Theme.accent700
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: ampDeviceMenu.popup()
                    }
                }
                Item { Layout.fillWidth: true }

                Menu {
                    id: ampDeviceMenu
                    Repeater {
                        model: EncoderController.outputDevices
                        MenuItem {
                            required property string modelData
                            required property int index
                            text: modelData
                            onTriggered: wizard.ampDeviceOverride = index
                        }
                    }
                }
            }

            // The endpoint-specific caveat the shared notice below cannot
            // know: no device here can bitstream what is about to be
            // encoded, so Play will stay greyed rather than fail after the
            // click - driven by the same auto-pick above, not a separate
            // guess at which devices/formats disagree.
            Text {
                Layout.fillWidth: true
                visible: wizard.dest === "amp" && EncoderController.outputDevices.length > 0
                         && wizard.ampDeviceIndex < 0
                text: qsTr("None of the output devices here can bitstream this stream — Play will stay greyed. Pick a different layout or codec, or save a file instead.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.accent700
            }

            // ---- what you are about to make -------------------------------
            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
            Text {
                text: qsTr("WHAT YOU ARE ABOUT TO MAKE")
                font.pixelSize: 10
                font.letterSpacing: 1
                color: Theme.textMuted
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 4
                columnSpacing: Theme.space4

                Repeater {
                    model: [
                        { label: qsTr("FORMAT"),
                          value: wizard.appWindow ? wizard.appWindow.planLine : "" },
                        { label: qsTr("SPEAKERS"),
                          value: EncoderController.atmosEnabled
                                 ? qsTr("5.1 bed · objects carry the height")
                                 : qsTr("%1 · %2 speakers")
                                   .arg(EncoderController.channelShapeName)
                                   .arg(EncoderController.renderedChannelCount) },
                        { label: qsTr("LOUDNESS"),
                          value: (EncoderController.measureDialnorm
                                  ? qsTr("Measured") : qsTr("dialnorm %1").arg(EncoderController.dialnorm))
                                 + " · "
                                 + (EncoderController.drcIndex > 0
                                    ? EncoderController.drcNames[EncoderController.drcIndex]
                                    : qsTr("no compression")) },
                        { label: qsTr("LENGTH"),
                          value: (function() {
                              const sources = EncoderController.sourceModel;
                              let seconds = 0;
                              for (let i = 0; i < sources.length; i++) {
                                  seconds = Math.max(seconds, sources[i].seconds);
                              }
                              const mm = Math.floor(seconds / 60);
                              const ss = String(Math.floor(seconds % 60)).padStart(2, "0");
                              return mm + ":" + ss;
                          })() },
                    ]
                    delegate: ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2

                        Text {
                            text: parent.modelData.label
                            font.pixelSize: 9
                            font.letterSpacing: 1
                            color: Theme.textMuted
                        }
                        Text {
                            Layout.fillWidth: true
                            text: parent.modelData.value
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            font.family: Theme.monoFamily
                            color: Theme.text
                        }
                    }
                }
            }

            // The loudness contract's own promise, spelled out - the LOUDNESS
            // cell above already shows the number this produces, but not why
            // it moved from the app's spec-neutral defaults. Two versions:
            // dual mono gets the honest, partial one, since measurement is
            // refused there and only the DRC half of the contract applies.
            Text {
                Layout.fillWidth: true
                visible: !EncoderController.dualMono && !EncoderController.loudnessTouched
                text: qsTr("Loudness is measured from the programme and film-standard compression applied — Guided handles this for you.")
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.textMuted
            }
            Text {
                Layout.fillWidth: true
                visible: EncoderController.dualMono && !EncoderController.loudnessTouched
                text: qsTr("Film-standard compression is applied to both programmes — loudness measurement isn't offered for dual mono, so set dialnorm by hand if the defaults aren't right.")
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: Theme.textMuted
            }

            // The unassigned warnings, restated at the door — the same
            // inventory-derived strings the table shows.
            Repeater {
                model: EncoderController.unassignedWarnings
                delegate: Text {
                    required property string modelData
                    Layout.fillWidth: true
                    text: qsTr("⚠ %1").arg(modelData)
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: Theme.accent700
                }
            }
        }
    }

    // ---- the shared notice: one slot, every step ---------------------------
    // The mockup's priority chain - object mode wins, then the amp fold-down,
    // then the plain "extras made this an .ec3" note. One element rather than
    // a copy per step, so the story never depends on which step is open.
    Rectangle {
        readonly property string noticeText: {
            if (EncoderController.atmosEnabled) {
                return qsTr("Movement is on, so the speaker layout is fixed at 5.1 and the format at Dolby Digital Plus — objects carry the height instead of ceiling speakers. Turn movement off in step 4 to choose your own layout again.");
            }
            if (wizard.dest === "amp" && EncoderController.renderedChannelCount > 6) {
                return qsTr("Your receiver leg tops out at what it can bitstream — the extra speakers you picked may be folded into the five on the way out. Save a file instead to keep them separate.");
            }
            const extras = EncoderController.extrasModel;
            for (let i = 0; i < extras.length; i++) {
                if (extras[i].checked) {
                    return qsTr("Speakers beyond the basic five need Dolby Digital Plus, so this saves as .ec3 rather than .ac3. Every modern receiver reads it; a DVD player will not.");
                }
            }
            return "";
        }
        visible: noticeText.length > 0
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: Theme.space3
        color: Theme.accent100
        implicitHeight: guidedNoticeText.implicitHeight + Theme.space3 * 2

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 2
            color: Theme.accent
        }
        Text {
            id: guidedNoticeText
            anchors.fill: parent
            anchors.margins: Theme.space3
            text: parent.noticeText
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            color: Theme.accent800
        }
    }

            // Breathing room at the end of the scroll range, so the last
            // control never kisses the pinned footer's divider.
            Item { Layout.preferredHeight: Theme.space4 }
        }
    }

    // ---- assistant line + back/next -------------------------------------------
    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: Theme.space3
        Layout.bottomMargin: Theme.space4
        spacing: Theme.space3

        Button {
            id: backButton
            objectName: "wizardBackButton"
            text: qsTr("Back")
            enabled: wizard.currentStepIndex > 0 || wizard.roomPicker
            // The room picker is a sub-screen of the speakers step, so Back
            // closes it before it retreats a step.
            onClicked: {
                if (wizard.roomPicker) {
                    wizard.roomPicker = false;
                    return;
                }
                wizard.currentStepKey = wizard.steps[wizard.currentStepIndex - 1].key;
            }
        }

        Text {
            Layout.fillWidth: true
            text: wizard.currentStep.assistant
            font.pixelSize: 12
            elide: Text.ElideRight
            color: Theme.textMuted
        }

        Button {
            id: nextButton
            objectName: "wizardNextButton"
            highlighted: true
            text: wizard.currentStep.next
            enabled: wizard.currentStepIndex === 0
                     ? EncoderController.sourceReady
                     : (wizard.currentStepIndex < wizard.steps.length - 1
                        || (EncoderController.sourceReady && !EncoderController.busy))
            onClicked: {
                // Next leaves the room picker behind - returning to step 2
                // should land on the presets, not a sub-screen someone
                // finished with.
                wizard.roomPicker = false;
                if (wizard.currentStepIndex < wizard.steps.length - 1) {
                    wizard.currentStepKey = wizard.steps[wizard.currentStepIndex + 1].key;
                    return;
                }
                // Encode now — the run strip's encoding chip and Cancel
                // become the feedback; guided never leaves its own screen.
                const win = wizard.requestWindow();
                if (win) {
                    win.startEncodeFlow();
                }
            }
        }
    }

    // Guided step 1 opens files through its own dialog so the wizard works
    // even before the rail has been visited.
    FileDialog {
        id: wizardOpenDialog
        title: qsTr("Choose WAV files")
        fileMode: FileDialog.OpenFiles
        nameFilters: [qsTr("WAV audio (*.wav)"), qsTr("All files (*)")]
        onAccepted: {
            for (let i = 0; i < selectedFiles.length; ++i) {
                EncoderController.addSourceFile(selectedFiles[i]);
            }
        }
    }
}
