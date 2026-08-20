import QtQuick
import QtTest

import Ac3Forge

// Proves the harness itself works end to end: the Ac3Forge module this
// binary embeds resolves, Main.qml (the real shell, not a stand-in) loads
// under the offscreen platform, and EncoderController - the real singleton,
// not a mock - is reachable with no source loaded. Every other tst_*.qml
// file builds on this working; if this one fails, look here first.
TestCase {
    id: testCase
    name: "MainShell"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    function test_windowMeetsTheHandoffsFloor() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(win.minimumWidth, 1280);
        compare(win.minimumHeight, 900);
        verify(win.title.length > 0);
    }

    function test_encoderControllerSingletonStartsWithNoSource() {
        compare(EncoderController.sourceReady, false);
        compare(EncoderController.sourcePath, "");
    }
}
