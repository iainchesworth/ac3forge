import QtQuick
import QtTest

import Ac3Forge

// The TrueHD lossless lab: TruehdController (its own controller for the
// second codec family - see truehd_controller.hpp's header comment for why
// it is not a third codecIndex in EncoderController) plus TruehdDialog.qml.
// TruehdController is a singleton like every other controller here, shared
// across test functions - each one starts from the state the previous left,
// so ordering within this file is deliberate: the rejection test runs
// first, proving a failed load leaves the controller usable for the real
// encode after it.
TestCase {
    id: testCase
    name: "TruehdLab"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // Real programme material, both ways: the PCM16 fixture is 0.5 s of the
    // roundtrip-stereo seed's own audio converted to integer samples
    // (CONTRIBUTING.md's "test with real audio" rule), and the float
    // original doubles as the rejection case - the integer-only reader must
    // refuse it rather than quietly round-tripping through floats.
    readonly property url pcm16Url:
        Qt.resolvedUrl("../fixtures/truehd-stereo-pcm16.wav")
    readonly property url floatUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_truehd_lab.mlp")

    function test_aFloatSourceIsRefusedNotConverted() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        TruehdController.setSource(floatUrl);
        tryCompare(TruehdController, "busy", false, 15000);
        compare(TruehdController.sourceReady, false);
        verify(TruehdController.error.indexOf("integer PCM") >= 0);
    }

    function test_encodeIsAsyncAndVerifiesBitExact() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        TruehdController.setSource(pcm16Url);
        // Loading runs off the GUI thread: busy flips true synchronously,
        // back to false once the worker's queued completion lands - the
        // same tryCompare pattern tst_qc_panel.qml uses.
        compare(TruehdController.busy, true);
        tryCompare(TruehdController, "busy", false, 15000);

        compare(TruehdController.error, "");
        compare(TruehdController.sourceReady, true);
        verify(TruehdController.sourceInfo.indexOf("2 ch") === 0);
        verify(TruehdController.sourceInfo.indexOf("16-bit") > 0);
        // A source suggests <source>.mlp beside itself.
        verify(TruehdController.outputPath.indexOf(".mlp") > 0);

        TruehdController.setOutput(outputUrl);
        verify(TruehdController.outputPath.indexOf("tst_truehd_lab.mlp") > 0);

        TruehdController.encode();
        compare(TruehdController.busy, true);
        tryCompare(TruehdController, "busy", false, 30000);

        compare(TruehdController.error, "");
        compare(TruehdController.hasResult, true);
        // The report is real data: the worker decoded the written stream
        // and diffed it against the source before saying this.
        verify(TruehdController.resultInfo.indexOf("verified bit-exact") > 0);
        verify(TruehdController.resultInfo.indexOf("access units") > 0);
    }

    function test_objectsModeEncodesAndReportsObjects() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        // The singleton still holds the loaded PCM16 source from the
        // previous test; flip to objects mode and encode again.
        compare(TruehdController.sourceReady, true);
        TruehdController.objectsMode = true;
        compare(TruehdController.objectsMode, true);

        TruehdController.setOutput(Qt.resolvedUrl("_test_output/tst_truehd_objects.mlp"));
        TruehdController.encode();
        tryCompare(TruehdController, "busy", false, 30000);

        compare(TruehdController.error, "");
        verify(TruehdController.resultInfo.indexOf("verified bit-exact") > 0);
        verify(TruehdController.resultInfo.indexOf("2 objects") > 0);
        TruehdController.objectsMode = false;
    }

    function test_dialogOpensFromTheControlsRail() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        const button = findChild(win, "truehdOpenButton");
        verify(button !== null);
        const dialog = findChild(win, "truehdDialog");
        verify(dialog !== null);
        compare(dialog.visible, false);
        button.clicked();
        tryCompare(dialog, "visible", true, 5000);
        const close = findChild(win, "truehdCloseButton");
        verify(close !== null);
        close.clicked();
        tryCompare(dialog, "visible", false, 5000);
    }
}
