import QtQuick
import QtTest

import Ac3Forge

// Selecting 1+1 as the bed - the "dual mono is a bed, not a layout" surface.
// bedChoices()[0] is always the dual-mono entry (see kBeds' own comment).
TestCase {
    id: testCase
    name: "DualMono"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_dual_mono.ac3")

    function test_selectingDualMonoClearsLfeAndLocksExtras() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(EncoderController.bedChoices[0].id, "1+1");

        // EncoderController is a singleton shared across every test function
        // in the whole suite (see e.g. tst_run_history.qml's own note on
        // this), so this establishes its own known starting point - 3/2 with
        // LFE on - rather than assume nothing earlier left the bed on 1+1
        // already. Qt Quick Test does not run functions in declaration
        // order (alphabetical within a file, confirmed the hard way: this
        // test failed its very next line when it assumed dualMono started
        // false, because test_dualMonoHasNoSoundstage - alphabetically
        // first - already ran and left it selected).
        EncoderController.bedIndex = 6;  // 3/2, per kBeds' own order
        EncoderController.bedLfe = true;
        compare(EncoderController.dualMono, false);
        verify(EncoderController.bedLfe);

        EncoderController.bedIndex = 0;  // 1+1

        compare(EncoderController.dualMono, true);
        compare(EncoderController.channelShapeName, "1+1");
        compare(EncoderController.bedLfe, false);
        compare(EncoderController.bedLfeLocked, true);
        compare(EncoderController.extrasLocked, true);
    }

    function test_dualMonoHasNoSoundstage() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        // A plain stereo source surrounds - the baseline this test would be
        // meaningless without.
        verify(EncoderController.surround);

        EncoderController.bedIndex = 0;  // 1+1
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        // The meters now reflect what was actually encoded (Ch1/Ch2), not
        // the source's own natural stereo layout - the soundfield's gate is
        // read off that, same as everywhere else in this file.
        compare(EncoderController.surround, false);
        compare(EncoderController.runs[0].status, "done");
    }
}
