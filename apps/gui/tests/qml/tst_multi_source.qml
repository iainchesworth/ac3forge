import QtQuick
import QtTest

import Ac3Forge

// addSourceFile/setAssignment/sourceModel/assignmentRows/unassignedWarnings -
// the surface a real Assign table would drive. Two loaded WAVs mapped onto
// a 5.1 bed by hand, checking the model updates at each step rather than
// just the end state, since a table bound to sourceModel/assignmentRows
// needs each of those transitions to fire sourceChanged correctly.
TestCase {
    id: testCase
    name: "MultiSource"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url surroundUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-51.wav")

    // The controller is one singleton shared across every tst_*.qml file in
    // this binary, run in a platform-dependent order - a test here that left
    // a source loaded or an explicit assignment set broke unrelated tests in
    // other files before (see tst_sweep_conformance.qml's own cleanup() and
    // its comment). Every test in this file sets atmosEnabled, loads
    // sources, and/or calls setAssignment/setAssignmentTrim, so it resets
    // the same ground tst_sweep_conformance.qml's own cleanup() does.
    function cleanup() {
        EncoderController.atmosEnabled = false;
        if (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.containerIndex = 0;
        EncoderController.drcIndex = 0;
        EncoderController.codecIndex = 0;
        EncoderController.bitrateKbps = 192;
        EncoderController.applyChannelPreset("stereo");
    }

    function test_addingASecondSourceNeedsAnAssignment() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        compare(EncoderController.sourceModel.length, 1);

        EncoderController.addSourceFile(surroundUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2);
        compare(EncoderController.sourceModel[0].primary, true);
        compare(EncoderController.sourceModel[1].primary, false);

        // 2 + 6 = 8 rows, one per (source, channel), and every one of them
        // starts out unassigned ("none") - nothing here has been mapped yet.
        compare(EncoderController.assignmentRows.length, 8);
        for (let i = 0; i < EncoderController.assignmentRows.length; ++i) {
            compare(EncoderController.assignmentRows[i].destToken, "none");
        }
    }

    function test_explicitAssignmentClearsTheGoesNowhereWarnings() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(surroundUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2);

        // Still nothing assigned - every loaded channel should be named.
        verify(EncoderController.unassignedWarnings.length > 0);

        // Silence the stereo file entirely and map the 5.1 file straight
        // onto the bed it already matches.
        EncoderController.setAssignment(0, 0, "none");
        EncoderController.setAssignment(0, 1, "none");
        EncoderController.setAssignment(1, 0, "L");
        EncoderController.setAssignment(1, 1, "R");
        EncoderController.setAssignment(1, 2, "C");
        EncoderController.setAssignment(1, 3, "LFE");
        EncoderController.setAssignment(1, 4, "Ls");
        EncoderController.setAssignment(1, 5, "Rs");

        compare(EncoderController.unassignedWarnings.length, 0);
    }

    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_multi_source.ec3")

    function test_autoAssignByNameFillsChannelsTheirOwnLayoutNames() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(surroundUrl);   // 5.1 - natural names
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(stereoUrl);      // stereo - L and R
        tryVerify(() => EncoderController.sourceModel.length === 2);
        EncoderController.applyChannelPreset("5.1");
        verify(EncoderController.unassignedWarnings.length > 0);

        // Every channel of both sources has a name in its own natural
        // layout, and every one of those positions exists in the 5.1 plan -
        // so nothing is left to warn about.
        EncoderController.autoAssignByName();
        compare(EncoderController.unassignedWarnings.length, 0);

        // A 5.1 WAV's channel order is FL FR FC LFE BL BR - its first
        // channel is the bed's L, its third the centre.
        const rows = EncoderController.assignmentRows;
        compare(rows[0].destToken, "L");
        compare(rows[2].destToken, "C");
        // The stereo source's pair lands on L and R too - two sources may
        // legitimately feed one speaker; they sum there.
        compare(rows[6].destToken, "L");
        compare(rows[7].destToken, "R");

        // By-name filling never overwrites a decision already made.
        EncoderController.setAssignment(0, 0, "none");
        EncoderController.autoAssignByName();
        compare(EncoderController.assignmentRows[0].destToken, "none");
    }

    function test_encodingWithAnExplicitAssignmentProducesADoneRun() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(surroundUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2);

        // The assignment says which source channel feeds which position;
        // the bed picker still says what positions EXIST. They are
        // orthogonal - loading the stereo file left the bed at 2/0, and an
        // assignment naming C/LFE/Ls/Rs against that target would have
        // nowhere valid to land, so the bed has to move to 5.1 first.
        EncoderController.applyChannelPreset("5.1");

        EncoderController.setAssignment(0, 0, "none");
        EncoderController.setAssignment(0, 1, "none");
        EncoderController.setAssignment(1, 0, "L");
        EncoderController.setAssignment(1, 1, "R");
        EncoderController.setAssignment(1, 2, "C");
        EncoderController.setAssignment(1, 3, "LFE");
        EncoderController.setAssignment(1, 4, "Ls");
        EncoderController.setAssignment(1, 5, "Rs");
        compare(EncoderController.unassignedWarnings.length, 0);

        const before = EncoderController.runs.length;
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        compare(EncoderController.runs.length, before + 1);
        compare(EncoderController.runs[0].status, "done");
    }

    function test_trimRoundTripsThroughSetAssignmentTrimAndMapToken() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(surroundUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.applyChannelPreset("5.1");
        EncoderController.setAssignment(0, 0, "L");
        // A plain destination pick carries no trim yet - only
        // setAssignmentTrim (the row's own dB field) ever sets one.
        compare(EncoderController.assignmentRows[0].trimDb, 0);

        EncoderController.setAssignmentTrim(0, 0, -3.5);
        compare(EncoderController.assignmentRows[0].trimDb, -3.5);
        // mapToken carries it through in the exact map= grammar - the
        // command bar has to be able to paste this straight into a CLI
        // call (see CLI -> Metadata options' map= section).
        verify(EncoderController.mapToken.indexOf("0.0:L@-3.5") >= 0);

        // Clamped to the documented +-24dB bound, snapped to a tenth of a
        // dB - the same grid parse_destination's own "@" suffix uses.
        EncoderController.setAssignmentTrim(0, 0, 100);
        compare(EncoderController.assignmentRows[0].trimDb, 24);
        EncoderController.setAssignmentTrim(0, 0, -12.34);
        compare(EncoderController.assignmentRows[0].trimDb, -12.3);

        // "none" has no destination for a trim to ride - Assignment::set
        // erases the row outright, so setAssignmentTrim on it is a no-op.
        EncoderController.setAssignment(0, 1, "none");
        EncoderController.setAssignmentTrim(0, 1, 6.0);
        compare(EncoderController.assignmentRows[1].trimDb, 0);
    }
}
