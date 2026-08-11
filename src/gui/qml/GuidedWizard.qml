import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// The Guided tier's own surface: a short, linear question sequence over the
// SAME EncoderController state Advanced/Expert read and write - there is no
// separate "wizard draft" to reconcile, so switching tiers mid-session is
// always a lossless round trip (see the tier SegmentedControl in Main.qml).
// Source loading itself stays on the left rail - it is "always visible"
// regardless of tier - so this only ever needs to show what state that
// produced, not a second file picker. Encoding itself stays on the
// persistent command bar below this panel, for the same reason: one Encode
// button, not a second copy of encodeTo()'s call site.
//
// Scope cut, flagged rather than silent: multi-source assignment and object
// (Atmos) placement are Advanced/Expert-only for now - both are inherently
// non-linear (a table, a spatial canvas), which a five-step sequence has
// nowhere honest to put. A user who needs either is pointed at Expert
// rather than offered a cut-down version of either surface here.
ColumnLayout {
    id: root
    objectName: "guidedWizard"
    spacing: Theme.gap

    // ---- the step sequence -------------------------------------------------
    // Data-driven rather than hand-nested ifs: adding a step means adding
    // one entry here, one Component below, and one case in stepComponent()
    // - not threading a new branch through Back/Next/the indicator row too.
    // "rate" (VBR) is the one step a choice earlier in the sequence can
    // remove - AC-3 has no such thing to configure - so the filter runs on
    // every read rather than being decided once up front.
    function stepDefs() {
        return [
            { key: "source", title: qsTr("Source") },
            { key: "format", title: qsTr("Format") },
            { key: "rate", title: qsTr("Rate mode"), skip: !EncoderController.vbrAvailable },
            { key: "loudness", title: qsTr("Loudness") },
            { key: "review", title: qsTr("Review") },
        ].filter((s) => !s.skip);
    }
    readonly property var activeSteps: stepDefs()
    property string currentStepKey: "source"
    readonly property int currentStepIndex: {
        const idx = activeSteps.findIndex((s) => s.key === currentStepKey);
        return idx >= 0 ? idx : 0;
    }

    function goNext() {
        if (currentStepIndex < activeSteps.length - 1) {
            currentStepKey = activeSteps[currentStepIndex + 1].key;
        }
    }
    function goBack() {
        if (currentStepIndex > 0) {
            currentStepKey = activeSteps[currentStepIndex - 1].key;
        }
    }
    // A step the sequence just dropped (picking AC-3 removes "rate" while
    // sitting on it) cannot stay current - land on whichever step took its
    // place instead of a page nothing in the indicator row points at.
    onActiveStepsChanged: {
        if (!activeSteps.some((s) => s.key === currentStepKey)) {
            currentStepKey = activeSteps.length > 0 ? activeSteps[0].key : "source";
        }
    }

    // Only the current step's Component is ever instantiated (see the Loader
    // below) - five permanently-alive sibling ColumnLayouts, four of them
    // merely toggled invisible, was tried first and rejected: QtQuick.
    // Layouts still folds an invisible child's own natural (unwrapped, pre-
    // constraint) content width into its parent's implicit-width
    // computation, so the Card - and everything chained back up through it
    // to the StackLayout that sizes this whole page - kept negotiating
    // toward a width wider than the actual two-pane window has room for.
    // Confirmed empirically: real mouseClick()s on the Back/Next row landed
    // off the right edge of the window (scenePos.x past window.width) until
    // this was rewritten, and every existing StackLayout page - none of
    // which stacks several conditionally-visible siblings this way - has
    // never shown the same symptom.
    function stepComponent(key) {
        switch (key) {
            case "source": return sourceStep;
            case "format": return formatStep;
            case "rate": return rateStep;
            case "loudness": return loudnessStep;
            case "review": return reviewStep;
            default: return sourceStep;
        }
    }

    // ---- step indicator ------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: 18
        spacing: 8

        Repeater {
            model: root.activeSteps

            delegate: RowLayout {
                required property var modelData
                required property int index
                spacing: 8

                Rectangle {
                    objectName: "wizardStepDot-" + modelData.key
                    width: 22
                    height: 22
                    radius: 11
                    color: index === root.currentStepIndex ? Theme.accent
                           : index < root.currentStepIndex ? Theme.neutral400 : Theme.neutral200

                    Text {
                        anchors.centerIn: parent
                        text: index + 1
                        color: index <= root.currentStepIndex ? Theme.bg : Theme.textMuted
                        font.pixelSize: 11
                        font.bold: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.currentStepKey = modelData.key
                    }
                }
                Text {
                    text: modelData.title
                    color: index === root.currentStepIndex ? Theme.text : Theme.textMuted
                    font.pixelSize: 12
                    font.bold: index === root.currentStepIndex
                }
                Rectangle {
                    visible: index < root.activeSteps.length - 1
                    Layout.preferredWidth: 24
                    height: 1
                    color: Theme.divider
                }
            }
        }
        Item { Layout.fillWidth: true }
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        height: 1
        color: Theme.divider
    }

    // ---- step content -----------------------------------------------------
    Card {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.topMargin: 8
        title: root.activeSteps.length > 0
               ? root.activeSteps[root.currentStepIndex].title : ""

        Loader {
            Layout.fillWidth: true
            sourceComponent: root.stepComponent(root.currentStepKey)
        }
    }

    // ---- Back / Next ----------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        Layout.rightMargin: 24
        Layout.bottomMargin: 18
        spacing: Theme.gap

        Button {
            objectName: "wizardBackButton"
            text: qsTr("Back")
            enabled: root.currentStepIndex > 0
            onClicked: root.goBack()
        }
        Item { Layout.fillWidth: true }
        Button {
            objectName: "wizardNextButton"
            text: qsTr("Next")
            visible: root.currentStepIndex < root.activeSteps.length - 1
            highlighted: true
            enabled: root.currentStepKey !== "source" || EncoderController.sourceReady
            onClicked: root.goNext()
        }
        Text {
            visible: root.currentStepIndex === root.activeSteps.length - 1
            text: qsTr("Ready — press Encode below.")
            color: Theme.good
            font.pixelSize: Theme.fontNormal
            font.bold: true
        }
    }

    // ---- step Components, one per key stepComponent() names above ---------

    Component {
        id: sourceStep
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            Text {
                Layout.fillWidth: true
                text: qsTr("Choose a WAV file, or record from a capture device, using the panel on the left.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontNormal
                wrapMode: Text.WordWrap
            }
            Text {
                objectName: "wizardSourceStatus"
                Layout.fillWidth: true
                text: EncoderController.sourceReady
                      ? qsTr("✓ %1").arg(EncoderController.sourceInfo)
                      : qsTr("No source loaded yet.")
                color: EncoderController.sourceReady ? Theme.good : Theme.textMuted
                font.pixelSize: Theme.fontNormal
                wrapMode: Text.WordWrap
            }
        }
    }

    Component {
        id: formatStep
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            Text {
                text: qsTr("CODEC")
                color: Theme.neutral600
                font.pixelSize: 10
                font.letterSpacing: 1
            }
            SegmentedControl {
                objectName: "wizardCodecControl"
                model: [{ value: "ac3", label: qsTr("Dolby Digital") },
                        { value: "eac3", label: qsTr("Dolby Digital Plus") }]
                currentValue: EncoderController.codecIndex === 1 ? "eac3" : "ac3"
                onSelected: (value) =>
                    EncoderController.codecIndex = value === "eac3" ? 1 : 0
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Dolby Digital Plus adds wider layouts, coding tools and variable bit rate. Dolby Digital is the narrower, universally-compatible original.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
                wrapMode: Text.WordWrap
            }

            Text {
                Layout.topMargin: Theme.gap
                text: qsTr("CHANNELS")
                color: Theme.neutral600
                font.pixelSize: 10
                font.letterSpacing: 1
            }
            Flow {
                Layout.fillWidth: true
                spacing: Theme.gap

                Repeater {
                    model: ["5.1", "7.1", "5.1.4", "7.1.4", "5.2"]
                    delegate: Button {
                        required property string modelData
                        objectName: "wizardPreset-" + modelData
                        text: modelData
                        enabled: !EncoderController.busy
                        onClicked: EncoderController.applyChannelPreset(modelData)
                    }
                }
                Button {
                    objectName: "wizardDualMonoButton"
                    text: qsTr("1+1 · Dual mono")
                    enabled: !EncoderController.busy
                    highlighted: EncoderController.dualMono
                    onClicked: EncoderController.bedIndex = 0

                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Two independent programmes, not a stereo pair")
                }
            }
            Text {
                Layout.fillWidth: true
                text: qsTr("Currently: %1").arg(EncoderController.channelShapeName)
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }

            Text {
                Layout.topMargin: Theme.gap
                text: qsTr("BIT RATE")
                color: Theme.neutral600
                font.pixelSize: 10
                font.letterSpacing: 1
            }
            ComboBox {
                objectName: "wizardBitrateCombo"
                Layout.preferredWidth: 160
                enabled: !EncoderController.busy
                model: EncoderController.bitrates
                currentIndex: EncoderController.bitrates.indexOf(EncoderController.bitrateKbps)
                displayText: currentText + " kbps"
                onActivated: EncoderController.bitrateKbps =
                                 EncoderController.bitrates[currentIndex]
            }
        }
    }

    Component {
        id: rateStep
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            Text {
                Layout.fillWidth: true
                text: qsTr("Dolby Digital Plus can target a quality level instead of a fixed rate — useful when file size should track how demanding the material actually is, rather than a number picked in advance.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontNormal
                wrapMode: Text.WordWrap
            }
            VbrPanel {}
        }
    }

    Component {
        id: loudnessStep
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            Text {
                Layout.fillWidth: true
                text: qsTr("dialnorm says where dialogue sits below full scale. The default (31, \"not indicated\") is fine to leave alone unless the programme's actual loudness is known.")
                color: Theme.textMuted
                font.pixelSize: Theme.fontNormal
                wrapMode: Text.WordWrap
            }
            LoudnessGroup {}
        }
    }

    Component {
        id: reviewStep
        ColumnLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: Theme.gap
                rowSpacing: 6

                Text { text: qsTr("Codec"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text {
                    objectName: "wizardReviewCodec"
                    text: EncoderController.codecNames[EncoderController.codecIndex] || ""
                    color: Theme.text
                    font.pixelSize: Theme.fontNormal
                }

                Text { text: qsTr("Channels"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text {
                    text: EncoderController.channelShapeName
                    color: Theme.text
                    font.pixelSize: Theme.fontNormal
                }

                Text { text: qsTr("Rate"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text {
                    objectName: "wizardReviewRate"
                    text: EncoderController.vbrAvailable && EncoderController.vbrEnabled
                          ? qsTr("VBR quality %1").arg(EncoderController.vbrQuality)
                          : qsTr("%1 kbps").arg(EncoderController.bitrateKbps)
                    color: Theme.text
                    font.pixelSize: Theme.fontNormal
                }

                Text { text: qsTr("Output"); color: Theme.textMuted; font.pixelSize: Theme.fontSmall }
                Text {
                    text: "." + EncoderController.outputSuffix()
                    color: Theme.text
                    font.pixelSize: Theme.fontNormal
                    font.family: "monospace"
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
}
