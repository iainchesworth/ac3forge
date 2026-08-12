import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

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

    readonly property var steps: [
        { key: "source", label: qsTr("Audio"),
          assistant: qsTr("Start with the audio. Everything after this follows from what you bring in."),
          next: qsTr("Next — your speakers") },
        { key: "setup", label: qsTr("Speakers"),
          assistant: qsTr("Tell me what you are playing this back on. I will set the channels to match."),
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
        // The enclosing ApplicationWindow, for tier/tab jumps.
        let item = wizard.parent;
        while (item && !item.goAssign) {
            item = item.parent;
        }
        return item;
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
                font.weight: Font.Black
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
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                color: "transparent"
                border.color: Theme.text
                border.width: EncoderController.sourceModel.length > 0 ? 2 : 1

                Rectangle {
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    color: EncoderController.sourceModel.length > 0 ? Theme.accent : "transparent"
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
                Layout.fillWidth: true
                Layout.preferredHeight: 72
                color: "transparent"
                border.color: Theme.divider
                border.width: 1

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
            visible: wizard.currentStepKey === "setup"
            Layout.fillWidth: true
            spacing: Theme.space4

            Text {
                text: qsTr("What are you playing this back on?")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.Black
                color: Theme.text
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.space3
                rowSpacing: Theme.space3

                Repeater {
                    model: [
                        { key: "stereo", preset: "stereo", signature: "2/0|0|",
                          title: qsTr("A laptop, or a stereo pair"),
                          body: qsTr("Two channels, no subwoofer. Plays absolutely everywhere.") },
                        { key: "home", preset: "5.1", signature: "3/2|1|",
                          title: qsTr("A home theatre — 5.1"),
                          body: qsTr("Five speakers and a subwoofer. The shape most receivers were built around.") },
                        { key: "atmos", preset: "5.1.4", signature: "3/2|1|topf,topr",
                          title: qsTr("Atmos — 5.1 plus ceiling"),
                          body: qsTr("The same bed with four height speakers above it.") },
                        { key: "full", preset: "7.1.4", signature: "3/2|1|rear,topf,topr",
                          title: qsTr("The full room — 7.1.4"),
                          body: qsTr("Rears behind you and four heights. Everything a living room can hold.") },
                    ]
                    delegate: Rectangle {
                        id: setupCard
                        required property var modelData
                        readonly property bool active: wizard.setupSignature === modelData.signature

                        objectName: "wizardSetup-" + modelData.key
                        Layout.fillWidth: true
                        Layout.preferredHeight: 84
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
                        }
                        TapHandler {
                            enabled: !EncoderController.atmosEnabled && !EncoderController.busy
                            onTapped: EncoderController.applyChannelPreset(setupCard.modelData.preset)
                        }
                    }
                }
            }

            Text {
                visible: EncoderController.atmosEnabled
                Layout.fillWidth: true
                text: qsTr("Movement is on, so the speaker layout is fixed at 5.1 and the format at Dolby Digital Plus — objects carry the height instead of ceiling speakers. Turn movement off in step 4 to choose your own layout again.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.accent700
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
                font.weight: Font.Black
                color: Theme.text
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3

                Repeater {
                    model: [
                        { rate: 192, title: qsTr("Broadcast"), body: qsTr("What digital TV uses for 5.1. 24 KB/s.") },
                        { rate: 448, title: qsTr("DVD"), body: qsTr("The DVD standard's comfortable ceiling. 56 KB/s.") },
                        { rate: 768, title: qsTr("Generous"), body: qsTr("More than any layout here needs. 96 KB/s.") },
                    ]
                    delegate: Rectangle {
                        id: rateCard
                        required property var modelData
                        readonly property bool active: EncoderController.bitrateKbps === modelData.rate

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
                                text: qsTr("%1 kbps").arg(rateCard.modelData.rate)
                                font.pixelSize: 18
                                font.family: Theme.headingFamily
                                font.weight: Font.Black
                                color: Theme.text
                            }
                            Text {
                                text: rateCard.modelData.title
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                color: Theme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                text: rateCard.modelData.body
                                wrapMode: Text.WordWrap
                                font.pixelSize: 11
                                color: Theme.neutral700
                            }
                        }
                        TapHandler {
                            enabled: !EncoderController.busy
                            onTapped: EncoderController.bitrateKbps = rateCard.modelData.rate
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
        }

        // ================= 4 · Movement =================
        ColumnLayout {
            visible: wizard.currentStepKey === "motion"
            Layout.fillWidth: true
            spacing: Theme.space4

            Text {
                text: qsTr("Should sounds move around the room?")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.Black
                color: Theme.text
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
                        onTapped: EncoderController.atmosEnabled = false
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
                text: qsTr("Adding objects auto-selected the 5.1 bed and E-AC-3 — that is the format objects ride in, not a preference. Place each object, give it a path and set its LFE send on the Objects tab.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.textMuted
            }
            Text {
                visible: EncoderController.atmosEnabled
                objectName: "wizardPlaceObjectsLink"
                text: qsTr("Place the objects →")
                font.pixelSize: 12
                font.weight: Font.DemiBold
                color: Theme.accent700
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        const win = wizard.requestWindow();
                        if (win) {
                            win.fromGuided = true;
                            win.tier = "advanced";
                            win.currentTab = "objects";
                        }
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
                text: qsTr("Where does it go?")
                font.pixelSize: 30
                font.family: Theme.headingFamily
                font.weight: Font.Black
                color: Theme.text
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space3

                Rectangle {
                    objectName: "wizardDest-file"
                    readonly property bool active: wizard.dest === "file"
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
                            text: qsTr("Save a file")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("A .%1 elementary stream, kept exactly as encoded — every channel and every object.")
                                  .arg(EncoderController.outputSuffix())
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                    }
                    TapHandler { onTapped: wizard.dest = "file" }
                }

                Rectangle {
                    objectName: "wizardDest-amp"
                    readonly property bool active: wizard.dest === "amp"
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
                            text: qsTr("Play it to the amplifier")
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Encode a file first, then bitstream it to a receiver as IEC 61937 bursts — Dolby Digital only today.")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.neutral700
                        }
                    }
                    TapHandler { onTapped: wizard.dest = "amp" }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: {
                    if (wizard.dest !== "amp") return false;
                    const meta = EncoderController.channelMeta;
                    return meta.length > 6 || EncoderController.codecIndex === 1;
                }
                text: qsTr("Your receiver leg takes Dolby Digital, which tops out at 5.1 — anything past that is folded in on the way out. Save a file instead to keep it separate.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.accent700
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

    Item { Layout.fillHeight: true }

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
            enabled: wizard.currentStepIndex > 0
            onClicked: wizard.currentStepKey = wizard.steps[wizard.currentStepIndex - 1].key
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
