import QtQuick
import QtTest

import Ac3Forge

// Variable bit rate: an E-AC-3 + file-output-only "Rate mode" surface (see
// EncoderController::vbrAvailable()'s own comment on why AC-3, object mode
// and a live session are excluded). Presence lives on the min/max checkboxes,
// never a sentinel value, and a finished run reports what it actually spent
// rather than a target it never had.
TestCase {
    id: testCase
    name: "Vbr"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_vbr.ec3")

    // EncoderController is a singleton shared across every test function in
    // the whole suite (see e.g. tst_run_history.qml's own note on this) -
    // every test here starts by putting codec/atmos into a known state
    // rather than assuming whatever an earlier, alphabetically-prior test
    // left behind.
    function test_vbrAvailableFollowsCodecAndObjectMode() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 0;  // AC-3
        compare(EncoderController.vbrAvailable, false);

        EncoderController.codecIndex = 1;  // E-AC-3
        compare(EncoderController.vbrAvailable, true);

        EncoderController.atmosEnabled = true;
        compare(EncoderController.vbrAvailable, false);

        EncoderController.atmosEnabled = false;
        compare(EncoderController.vbrAvailable, true);
    }

    function test_switchingBackToAc3LeavesVbrHarmlesslyStale() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 1;  // E-AC-3
        EncoderController.vbrEnabled = true;
        compare(EncoderController.vbrAvailable, true);

        // currentPlan() must not carry a stale vbr into an AC-3 plan -
        // validate() would refuse it (PlanError::kVbrNeedsEac3) for a
        // setting the Rate mode control no longer even shows.
        EncoderController.codecIndex = 0;  // AC-3
        compare(EncoderController.vbrAvailable, false);

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.encodeTo(
            Qt.resolvedUrl("_test_output/tst_vbr_ac3_stale.ac3"));
        tryCompare(EncoderController, "busy", false, 10000);
        compare(EncoderController.runs[0].status, "done");

        EncoderController.vbrEnabled = false;
    }

    function test_vbrTokenMatchesTheCliGrammar() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 1;  // E-AC-3
        EncoderController.vbrEnabled = true;
        EncoderController.vbrQuality = 75;
        EncoderController.vbrMinEnabled = false;
        EncoderController.vbrMaxEnabled = false;
        compare(EncoderController.vbrToken, "q:0.750000");

        EncoderController.vbrMinEnabled = true;
        EncoderController.vbrMinKbps = 192;
        compare(EncoderController.vbrToken, "q:0.750000,min:192");

        EncoderController.vbrMaxEnabled = true;
        EncoderController.vbrMaxKbps = 640;
        compare(EncoderController.vbrToken, "q:0.750000,min:192,max:640");

        // Presence lives on the checkbox: unticking min drops it from the
        // token entirely rather than leaving a 0 or a default behind.
        EncoderController.vbrMinEnabled = false;
        compare(EncoderController.vbrToken, "q:0.750000,max:640");

        EncoderController.vbrEnabled = false;
        EncoderController.vbrMaxEnabled = false;
    }

    function test_encodingWithVbrReportsWhatItActuallySpent() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 1;  // E-AC-3
        EncoderController.vbrEnabled = true;
        EncoderController.vbrQuality = 75;
        EncoderController.vbrMinEnabled = false;
        EncoderController.vbrMaxEnabled = true;
        EncoderController.vbrMaxKbps = 640;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);

        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        compare(EncoderController.runs[0].status, "done");
        // "what it did", not a target it never had (the design's own
        // framing) - the run strip shows the real avg/min/max, not the
        // quality alone.
        verify(EncoderController.runs[0].rateText.indexOf("VBR q75") === 0);
        verify(EncoderController.runs[0].rateText.indexOf("avg") > 0);
        verify(EncoderController.runs[0].rateText.indexOf("kbps") > 0);

        EncoderController.vbrEnabled = false;
        EncoderController.vbrMaxEnabled = false;
    }
}
