import QtQuick
import QtTest

import Ac3Forge

TestCase {
    id: testCase
    name: "RunHistory"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url fixtureUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_run_history.ac3")

    function test_encodingAddsADoneRunToTheStrip() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(fixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);

        const before = EncoderController.runs.length;
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        compare(EncoderController.runs.length, before + 1);
        const last = EncoderController.runs[EncoderController.runs.length - 1];
        compare(last.status, "done");
    }
}
