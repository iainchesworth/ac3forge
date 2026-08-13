import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// The assignment surface: one row per loaded source channel, each with a
// destination dropdown — a bed position of the current layout, a new object,
// a programme (dual mono) or nothing. This is the model everything else
// derives from: the meters' fed flags, the soundfield dots, the routing
// sentence and the CLI --map tokens all read what is set here.
//
// Two callers: the Format tab's full table (compact: false, with the header
// row and the unassigned warning banner) and guided step 1's "What each
// sound does" list (compact: true). Both drive the same
// EncoderController.setAssignment; there is no wizard draft.
ColumnLayout {
    id: root

    // Guided's list drops the table header and the banner — the wizard has
    // its own framing copy around it.
    property bool compact: false
    // The Preferences "show the plain-language notes beside controls" knob.
    property bool showExplanations: true

    spacing: Theme.space3

    // Sending a source to an object is the entry point to object mode: it
    // fixes E-AC-3 over a 5.1 bed and raises the bit rate to at least
    // 384 kbps, atomically with the assignment — the handoff's own rule.
    function assignDestination(sourceIndex, channel, token) {
        if (token === "obj" && !EncoderController.atmosEnabled) {
            if (EncoderController.dualMono) {
                return; // dual mono offers no object option at all
            }
            EncoderController.applyChannelPreset("5.1");
            EncoderController.atmosEnabled = true;
            if (EncoderController.bitrateKbps < 384) {
                EncoderController.bitrateKbps = 384;
            }
        }
        EncoderController.setAssignment(sourceIndex, channel, token);
    }

    // With exactly one source and nothing explicitly assigned, the controller
    // routes automatically — every channel is accounted for by construction
    // (its warnings list is empty), and the rows must say so rather than
    // claim the audio will not be heard.
    readonly property bool automaticRouting: EncoderController.sourceModel.length === 1
                                             && EncoderController.unassignedWarnings.length === 0

    // What a destination means, in plain language — the table's "Then"
    // column and the guided list's explanation, same strings.
    function thenText(token, touched) {
        if (token === "none") {
            if (touched) {
                return qsTr("Deliberately silent");
            }
            return automaticRouting
                   ? qsTr("Carried automatically — the routing panned it for you")
                   : qsTr("Unassigned — it will not be heard");
        }
        if (token === "obj") {
            return qsTr("An object, placed in the room");
        }
        if (token === "p1") {
            return qsTr("Programme 1 — its own independent soundtrack");
        }
        if (token === "p2") {
            return qsTr("Programme 2 — its own independent soundtrack");
        }
        return qsTr("Carried as a channel");
    }

    // The dropdown's option list follows the current plan: dual mono offers
    // programmes, everything else offers the coded positions actually
    // carried (object mode: the 5.1 bed, where an assignment pins the
    // channel as a static object at that speaker), then an object, then
    // nothing on purpose.
    readonly property var destinationOptions: {
        const options = [{ value: "",
                           label: automaticRouting ? qsTr("Automatic") : qsTr("Choose…") }];
        if (EncoderController.dualMono && !EncoderController.atmosEnabled) {
            options.push({ value: "p1", label: qsTr("Programme 1") });
            options.push({ value: "p2", label: qsTr("Programme 2") });
        } else {
            const planned = EncoderController.plannedChannels;
            for (let i = 0; i < planned.length; i++) {
                if (planned[i].replaced === true) {
                    continue;
                }
                options.push({ value: planned[i].token,
                               label: qsTr("Bed · %1").arg(planned[i].token) });
            }
            options.push({ value: "obj", label: qsTr("A new object") });
        }
        options.push({ value: "none", label: qsTr("Nothing") });
        return options;
    }

    // The banner naming what goes nowhere — built from the live inventory,
    // never a hard-coded filename.
    Rectangle {
        Layout.fillWidth: true
        visible: !root.compact && EncoderController.unassignedWarnings.length > 0
        implicitHeight: warningText.implicitHeight + Theme.space3 * 2
        color: Theme.accent100

        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: 2
            color: Theme.accent
        }
        Text {
            id: warningText
            anchors.fill: parent
            anchors.leftMargin: Theme.space3
            anchors.rightMargin: Theme.space3
            verticalAlignment: Text.AlignVCenter
            text: {
                const warnings = EncoderController.unassignedWarnings;
                const joined = warnings.join(", ").replace(/ is loaded but goes nowhere/g, "");
                return warnings.length === 1
                       ? qsTr("%1 — it will not be in the encode until you give it a destination.").arg(warnings[0])
                       : qsTr("%1 are loaded but go nowhere — they will not be in the encode until you give them a destination.").arg(joined);
            }
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            color: Theme.accent700
        }
    }

    // Header row (full table only), with the by-name fill: every channel of
    // a source that HAS a natural layout goes to the position it holds in
    // that layout — a real action, not the prototype's dead button.
    RowLayout {
        visible: !root.compact
        Layout.fillWidth: true
        spacing: Theme.space2

        Text { Layout.preferredWidth: 170; text: qsTr("FILE"); font.pixelSize: 10; font.letterSpacing: 1; color: Theme.textMuted }
        Text { Layout.preferredWidth: 50; text: qsTr("CH"); font.pixelSize: 10; font.letterSpacing: 1; color: Theme.textMuted }
        Text { Layout.preferredWidth: 200; text: qsTr("GOES TO"); font.pixelSize: 10; font.letterSpacing: 1; color: Theme.textMuted }
        Text { Layout.fillWidth: true; text: qsTr("THEN"); font.pixelSize: 10; font.letterSpacing: 1; color: Theme.textMuted }
        Button {
            objectName: "autoAssignButton"
            text: qsTr("Auto-assign by name")
            flat: true
            font.pixelSize: 11
            visible: EncoderController.sourceModel.length > 0
            enabled: !EncoderController.busy
            onClicked: EncoderController.autoAssignByName()
        }
    }
    Rectangle {
        visible: !root.compact
        Layout.fillWidth: true
        Layout.preferredHeight: 1
        color: Theme.divider
    }

    Repeater {
        id: rows
        objectName: "assignmentRows"
        model: EncoderController.assignmentRows

        delegate: ColumnLayout {
            id: row
            required property var modelData
            required property int index

            Layout.fillWidth: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.space2

                Text {
                    Layout.preferredWidth: 170
                    text: row.modelData.sourceLabel
                    elide: Text.ElideMiddle
                    font.pixelSize: 12
                    font.family: Theme.monoFamily
                    color: Theme.text
                }
                Text {
                    Layout.preferredWidth: 50
                    text: qsTr("ch %1").arg(row.modelData.channel + 1)
                    font.pixelSize: 12
                    font.family: Theme.monoFamily
                    color: Theme.textMuted
                }
                ComboBox {
                    id: destBox
                    objectName: "assignDest-" + row.modelData.source + "-" + row.modelData.channel
                    Layout.preferredWidth: 200
                    model: root.destinationOptions
                    textRole: "label"
                    valueRole: "value"
                    font.pixelSize: 12
                    // "none" untouched shows the placeholder; touched shows
                    // the deliberate "Nothing".
                    currentIndex: {
                        const token = row.modelData.destToken;
                        const shown = token === "none" && row.modelData.touched !== true
                                      ? "" : token;
                        const options = root.destinationOptions;
                        for (let i = 0; i < options.length; i++) {
                            if (options[i].value === shown) return i;
                        }
                        return 0;
                    }
                    onActivated: {
                        if (currentValue === "") {
                            return; // the placeholder is not a destination
                        }
                        root.assignDestination(row.modelData.source,
                                               row.modelData.channel, currentValue);
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: root.thenText(row.modelData.destToken,
                                        row.modelData.touched === true)
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    color: row.modelData.destToken === "none" && row.modelData.touched !== true
                           && !root.automaticRouting
                           ? Theme.accent700 : Theme.textMuted
                }
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: Theme.neutral200
                visible: row.index < rows.count - 1
            }
        }
    }

    Text {
        visible: rows.count === 0
        text: qsTr("Load a source and its channels appear here, each with a destination.")
        font.pixelSize: 12
        color: Theme.textMuted
    }

    Text {
        visible: !root.compact && rows.count > 0 && root.showExplanations
        Layout.fillWidth: true
        text: EncoderController.atmosEnabled
              ? qsTr("Object mode is on: sources sent to an object are placed in the room and ride as metadata. A bed position pins the channel there instead.")
              : qsTr("Sending a source to an object turns object mode on, which fixes the stream at Dolby Digital Plus over a 5.1 bed.")
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.textMuted
    }
}
