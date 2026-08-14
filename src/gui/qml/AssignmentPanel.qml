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
    // "objm-pair" is this panel's own synthetic spelling (never a real
    // destToken - EncoderController never sees it) for "fold this
    // two-channel source's pair into one mono object": it writes objm to
    // BOTH channels at once, through the exact same setAssignment path a
    // single channel takes, so nothing downstream needs to know a pair
    // affordance exists at all. Picking anything else on EITHER row breaks
    // the group on its own — dynamicObjectChannels() only groups a
    // CONTIGUOUS run of objm rows, so one row leaving objm is enough.
    function assignDestination(sourceIndex, channel, token) {
        const entersObjectMode = token === "obj" || token === "objm-pair";
        if (entersObjectMode && !EncoderController.atmosEnabled) {
            if (EncoderController.dualMono) {
                return; // dual mono offers no object option at all
            }
            EncoderController.applyChannelPreset("5.1");
            EncoderController.atmosEnabled = true;
            if (EncoderController.bitrateKbps < 384) {
                EncoderController.bitrateKbps = 384;
            }
        }
        if (token === "objm-pair") {
            EncoderController.setAssignment(sourceIndex, 0, "objm");
            EncoderController.setAssignment(sourceIndex, 1, "objm");
            return;
        }
        EncoderController.setAssignment(sourceIndex, channel, token);
    }

    // Formats a trim for the row's own field: "0" at the neutral default,
    // one decimal otherwise ("-3.5") - the control only ever needs to show
    // what setAssignmentTrim's own tenth-of-a-dB grid already snapped to.
    function formatTrim(dbTrim) {
        return (dbTrim === undefined || dbTrim === 0) ? "0" : dbTrim.toFixed(1);
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

    // destinationOptions plus, for either row of a two-channel source, the
    // "objm-pair" affordance (see assignDestination's own comment) - the
    // stereo-source shortcut for "fold both channels into one mono object".
    // Never offered for a wider source (a range picker for >2 channels is
    // out of this panel's per-row model entirely) or under dual mono (no
    // object option exists there at all).
    function rowDestinationOptions(sourceIndex, channel) {
        const options = root.destinationOptions.slice();
        if (EncoderController.dualMono) {
            return options;
        }
        const sources = EncoderController.sourceModel;
        if (sourceIndex >= sources.length || sources[sourceIndex].channels !== 2) {
            return options;
        }
        // Ahead of "Nothing" (the list's last entry), matching where "A new
        // object" itself sits relative to it.
        options.splice(options.length - 1, 0,
                       { value: "objm-pair", label: qsTr("One object, folded to mono") });
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
                    model: root.rowDestinationOptions(row.modelData.source, row.modelData.channel)
                    textRole: "label"
                    valueRole: "value"
                    font.pixelSize: 12
                    // "none" untouched shows the placeholder; touched shows
                    // the deliberate "Nothing". A row's own "objm" reads as
                    // this ComboBox's "objm-pair" entry when one is offered
                    // here - there is no separate plain "objm" spelling in
                    // the list, only the pair affordance (see
                    // rowDestinationOptions).
                    currentIndex: {
                        const token = row.modelData.destToken;
                        const shown = token === "none" && row.modelData.touched !== true
                                      ? "" : (token === "objm" ? "objm-pair" : token);
                        const options = root.rowDestinationOptions(row.modelData.source,
                                                                    row.modelData.channel);
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
                // The trim control: a small dB field, one per row, riding
                // alongside every destination kind (see
                // Destination::trim_db's own comment - it applies wherever
                // the channel's content reaches the stream, bed or object
                // alike). 0 dB - the default, nothing trimmed - renders
                // muted; any other value stands out the way a deliberate
                // "Nothing" already does above.
                TextField {
                    id: trimField
                    objectName: "assignTrim-" + row.modelData.source + "-" + row.modelData.channel
                    Layout.preferredWidth: 52
                    horizontalAlignment: Text.AlignRight
                    font.pixelSize: 12
                    font.family: Theme.monoFamily
                    color: (row.modelData.trimDb || 0) !== 0 ? Theme.text : Theme.textMuted
                    enabled: row.modelData.destToken !== "none"
                    opacity: enabled ? 1.0 : 0.45
                    validator: DoubleValidator { bottom: -24; top: 24; decimals: 1 }
                    text: root.formatTrim(row.modelData.trimDb)
                    onEditingFinished: {
                        const value = parseFloat(text);
                        if (!isNaN(value)) {
                            EncoderController.setAssignmentTrim(row.modelData.source,
                                                                row.modelData.channel, value);
                        }
                    }
                }
                Text {
                    text: qsTr("dB")
                    font.pixelSize: 10
                    color: Theme.textMuted
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
