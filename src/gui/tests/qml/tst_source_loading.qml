import QtQuick
import QtTest

import Ac3Forge

TestCase {
    id: testCase
    name: "SourceLoading"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // The same fixture src/gui/main.cpp's --smoke harness already trusts, so
    // the two can never disagree about what a "known good" WAV looks like.
    readonly property url fixtureUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")

    function test_loadingAFileUpdatesSourceReadyAndTheDisplayedName() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(EncoderController.sourceReady, false);

        EncoderController.loadSourceFile(fixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);

        compare(win.sourceLabel, "roundtrip-stereo.wav");
    }
}
