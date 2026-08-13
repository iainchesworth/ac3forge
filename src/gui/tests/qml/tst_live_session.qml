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
        // The warning lives in the Live session tab's pre-flight section
        // now (bundle B1 moved the session's own controls off the rail -
        // see Main.qml's "Live session" Card). Reaching it needs the tab
        // actually CURRENT: Item.visible reads effective (ancestor-aware)
        // visibility in Qt Quick, and StackLayout hides every page but the
        // current one by setting that page's own visible false, which
        // cascades to everything inside it regardless of its own binding.
        // Guided does not even offer the session tab, so tier has to move
        // off it first; nothing here starts a real session to auto-focus
        // the tab, so currentTab is set explicitly too.
        win.everHadSource = true;
        win.inputMode = "live";
        win.tier = "advanced";
        win.currentTab = "session";

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

    // Shared by every test below that reads something inside the Live
    // session tab's StackLayout page - see
    // test_vbrWarningAppearsOnlyWhenVbrIsOnAndAvailable's own comment on why
    // that page has to be the CURRENT one for Item.visible reads inside it
    // to mean anything.
    function openLiveSessionTab(win) {
        win.everHadSource = true;
        win.inputMode = "live";
        win.tier = "advanced";
        win.currentTab = "session";
    }

    function test_startSessionButtonGatingMatchesItsDocumentedFormula() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        openLiveSessionTab(win);

        let button = null;
        tryVerify(() => {
            button = findChild(win.contentItem, "startSessionButton");
            return button !== null;
        });
        // Item 9's contract: Start is enabled exactly when idle with a
        // device available (captureSupported && !busy) - asserted as the
        // formula itself, since this offscreen CI environment has no real
        // capture device to flip captureSupported true, and forcing busy_
        // needs a real encode/session this suite cannot start either.
        compare(button.enabled, EncoderController.captureSupported && !EncoderController.busy);
    }

    function test_transportShowsExactlyOneOfStartOrStopAtOnce() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        openLiveSessionTab(win);

        let startButton = null;
        let stopButton = null;
        tryVerify(() => {
            startButton = findChild(win.contentItem, "startSessionButton");
            stopButton = findChild(win.contentItem, "stopSessionButton");
            return startButton !== null && stopButton !== null;
        });
        // Idle by construction - nothing in this suite ever starts a real
        // session - so the pre-flight half shows and the running half does
        // not.
        compare(EncoderController.liveActive, false);
        compare(startButton.visible, true);
        compare(stopButton.visible, false);
    }

    function test_safetyCopyCheckboxIsGatedOnWriteToDiskAndRoundTripsTheProperty() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        openLiveSessionTab(win);

        let writeCheck = null;
        let safetyCheck = null;
        tryVerify(() => {
            writeCheck = findChild(win.contentItem, "liveWriteCheck");
            safetyCheck = findChild(win.contentItem, "liveWavSafetyCheck");
            return writeCheck !== null && safetyCheck !== null;
        });

        compare(writeCheck.checked, false);
        compare(safetyCheck.enabled, false);

        writeCheck.checked = true;
        compare(safetyCheck.enabled, true);

        // Controller -> checkbox is a plain binding (checked:
        // EncoderController.liveWavSafetyCopy) and reacts to a programmatic
        // change same as an interactive one.
        EncoderController.liveWavSafetyCopy = true;
        compare(safetyCheck.checked, true);
        EncoderController.liveWavSafetyCopy = false;
        compare(safetyCheck.checked, false);

        // Checkbox -> controller only happens through onToggled, which - like
        // deviceBox's onActivated elsewhere in this file - fires on real
        // interaction (the toggled signal), not on assigning .checked
        // directly, so this direction needs an actual click to exercise.
        mouseClick(safetyCheck);
        compare(EncoderController.liveWavSafetyCopy, true);
        mouseClick(safetyCheck);
        compare(EncoderController.liveWavSafetyCopy, false);

        // Leave state clean for whichever test runs next.
        EncoderController.liveWavSafetyCopy = false;
    }

    function test_receiverComboExistsForBothPreFlightAndHotSwap() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        openLiveSessionTab(win);

        let receiverBox = null;
        tryVerify(() => {
            receiverBox = findChild(win.contentItem, "liveReceiverBox");
            return receiverBox !== null;
        });
        // One combo serves both roles (item 9's pre-flight choice AND item
        // 15's hot-swap) - it exists and is interactive whether or not a
        // session is running, unlike the write/safety-copy controls above
        // which only make sense before Start.
        verify(receiverBox.visible);
        verify(receiverBox.enabled);
        compare(receiverBox.model[0], qsTr("No passthrough"));
    }

    // Items 13 and 15's new invokables all share the same refusal
    // convention every other "start/change a running thing" entry point in
    // this controller already uses - silently do nothing outside a live
    // session. Asserted here because a real live session cannot be driven
    // in this offscreen CI environment (see this file's own header
    // comment); this is the one guarantee about them that IS testable
    // without one, and it is also the guard against a stray call anywhere
    // in the app before a session exists.
    function test_addAndReassignLiveObjectAreNoOpsOutsideALiveSession() {
        compare(EncoderController.liveActive, false);
        compare(EncoderController.liveObjectSlotsBound, 0);
        compare(EncoderController.liveObjectChannels.length, 0);

        EncoderController.addLiveObject(0);
        EncoderController.addLiveObject(-1);
        EncoderController.reassignLiveObjectSlot(0, 0);
        EncoderController.reassignLiveObjectSlot(-1, -1);

        compare(EncoderController.liveObjectSlotsBound, 0);
        compare(EncoderController.liveObjectChannels.length, 0);
    }

    function test_switchLiveReceiverIsANoOpOutsideALiveSession() {
        compare(EncoderController.liveActive, false);
        compare(EncoderController.livePassthrough, false);
        const before = EncoderController.liveReceiverPlanText;

        EncoderController.switchLiveReceiver(0);
        EncoderController.switchLiveReceiver(-1);

        compare(EncoderController.livePassthrough, false);
        compare(EncoderController.liveReceiverPlanText, before);
    }

    function test_measuredLatencyAndDeviceChannelsDefaultToIdle() {
        // liveLatencyMs starts as the fixed estimate (see
        // EncoderController::startLiveSession), never the measured one,
        // until a session has actually run one frame past the settle
        // point - which never happens here.
        compare(EncoderController.liveActive, false);
        compare(EncoderController.liveLatencyMeasured, false);
        compare(EncoderController.liveDeviceChannels, 0);
    }
}
