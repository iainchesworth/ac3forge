import QtQuick
import QtTest

import Ac3Forge

// Live session itself needs a real capture device to drive - startRecording/
// startLiveSession have never had Quick Test coverage for that reason (there
// is none in the offscreen CI environment this suite runs in), and that
// stays true here. What IS testable without one: the pieces of the
// surrounding UI that read EncoderController state to warn about a live
// session in advance, before Start is ever clicked.
TestCase {
    id: testCase
    name: "LiveSession"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    function test_vbrWarningAppearsOnlyWhenVbrIsOnAndAvailable() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        // The warning lives on the rail's live-capture branch, which only
        // exists on screen once the first-run screen has been left and the
        // input selector is on Live.
        win.everHadSource = true;
        win.inputMode = "live";

        let warning = null;
        tryVerify(() => {
            warning = findChild(win.contentItem, "liveVbrWarning");
            return warning !== null;
        });

        EncoderController.codecIndex = 0;  // AC-3: vbrAvailable is false
        EncoderController.vbrEnabled = false;
        compare(warning.visible, false);

        EncoderController.codecIndex = 1;  // E-AC-3: vbrAvailable is true
        compare(EncoderController.vbrAvailable, true);
        compare(warning.visible, false);  // vbrEnabled is still false

        EncoderController.vbrEnabled = true;
        compare(warning.visible, true);

        EncoderController.vbrEnabled = false;
        compare(warning.visible, false);

        // Leave state clean for whichever test runs next.
        EncoderController.codecIndex = 0;
    }
}
