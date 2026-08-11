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
}
