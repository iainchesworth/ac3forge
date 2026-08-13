import QtQuick
import QtTest

import Ac3Forge

// Bundle B2's two-device capture (item 10) and parallel downmix receiver leg
// (item 16). Like tst_live_session.qml, a REAL live session needs real
// hardware this offscreen CI environment does not reliably have - what IS
// portable here is the CONTRACT: the formulas captureDeviceRows/
// captureDeviceCount/captureDeviceCapReached/captureDeviceTotals/
// liveCaptureChannelLabels satisfy regardless of how many real capture
// devices this machine happens to enumerate (0 on a bare CI box, 1+ on a
// dev machine with a real microphone), the no-op refusal convention for
// out-of-range add/remove, the cliLine capture2= token's presence exactly
// tracking a two-device selection, and the idle-state defaults for every
// new liveDownmixLeg/liveSecondDevice*/liveDriftPpm/liveDriftText property
// and the QML surfaces that read them.
TestCase {
    id: testCase
    name: "LiveMultiDevice"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // Matches tst_live_session.qml's own helper - the StackLayout page these
    // reads live on only reflects real bindings once it is the CURRENT page
    // (see the qml-stacklayout-effective-visibility memory).
    function openLiveSessionTab(win) {
        win.everHadSource = true;
        win.inputMode = "live";
        win.tier = "advanced";
        win.currentTab = "session";
    }

    function test_captureDeviceCountAndCapFormulasHoldForWhateverIsSelected() {
        // Invariants, not fixed values - true whether this machine has 0
        // real capture devices or several.
        compare(EncoderController.captureDeviceCount, EncoderController.captureDeviceRows.length);
        compare(EncoderController.captureDeviceCapReached, EncoderController.captureDeviceCount >= 2);
        for (let i = 0; i < EncoderController.captureDeviceRows.length; i++) {
            compare(EncoderController.captureDeviceRows[i].isMaster, i === 0);
            compare(EncoderController.captureDeviceRows[i].slotIndex, i);
        }
    }

    function test_captureDeviceTotalsIsEmptyIffNothingIsSelected() {
        compare(EncoderController.captureDeviceTotals.length === 0,
               EncoderController.captureDeviceCount === 0);
        if (EncoderController.captureDeviceCount > 0) {
            verify(EncoderController.captureDeviceTotals.indexOf("captured") >= 0);
        }
    }

    function test_liveCaptureChannelLabelsCountsMatchTheSelectedDevicesOwnChannelCounts() {
        let expected = 0;
        for (let i = 0; i < EncoderController.captureDeviceRows.length; i++) {
            expected += EncoderController.captureDeviceRows[i].channels;
        }
        compare(EncoderController.liveCaptureChannelLabels.length, expected);
        // The master's own labels are plain "Ch N"; a slave's are prefixed
        // "Dev2 Ch N" - never the other way around.
        if (EncoderController.captureDeviceRows.length > 0) {
            const masterChannels = EncoderController.captureDeviceRows[0].channels;
            for (let ch = 0; ch < masterChannels; ch++) {
                verify(EncoderController.liveCaptureChannelLabels[ch].indexOf("Dev2") < 0);
            }
        }
        if (EncoderController.captureDeviceRows.length > 1) {
            const masterChannels = EncoderController.captureDeviceRows[0].channels;
            if (EncoderController.liveCaptureChannelLabels.length > masterChannels) {
                verify(EncoderController.liveCaptureChannelLabels[masterChannels].indexOf("Dev2") === 0);
            }
        }
    }

    function test_addCaptureDeviceIsANoOpForAnOutOfRangeIndex() {
        const before = EncoderController.captureDeviceCount;
        EncoderController.addCaptureDevice(-1);
        EncoderController.addCaptureDevice(99999);
        compare(EncoderController.captureDeviceCount, before);
    }

    function test_removeCaptureDeviceIsANoOpForAnOutOfRangeSlot() {
        const before = EncoderController.captureDeviceRows.slice();
        EncoderController.removeCaptureDevice(-1);
        EncoderController.removeCaptureDevice(99999);
        compare(EncoderController.captureDeviceRows.length, before.length);
    }

    function test_addCaptureDeviceRefusesADeviceAlreadySelected() {
        if (EncoderController.captureDeviceRows.length === 0) {
            // Nothing selected on this machine to duplicate - the no-op
            // convention is still exercised (a bad index refuses, same as
            // the out-of-range test above), just not this specific path.
            return;
        }
        const before = EncoderController.captureDeviceCount;
        EncoderController.addCaptureDevice(EncoderController.captureDeviceRows[0].deviceIndex);
        compare(EncoderController.captureDeviceCount, before);
    }

    function test_railShowsOneRowPerSelectedDeviceAndTheAddInputGatingFormula() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        // everHadSource=true first - otherwise the first-run screen covers
        // the rail entirely (see FirstRunScreen.qml) and its own totals/cap-
        // note visibility bindings never get a live layout pass, matching
        // every other test here that reads rail/session content.
        win.everHadSource = true;
        win.inputMode = "live";

        let list = null;
        let addButton = null;
        let capNote = null;
        let totals = null;
        tryVerify(() => {
            list = findChild(win.contentItem, "captureDeviceList");
            addButton = findChild(win.contentItem, "addCaptureDeviceButton");
            capNote = findChild(win.contentItem, "captureDeviceCapNote");
            totals = findChild(win.contentItem, "captureDeviceTotals");
            return list !== null && addButton !== null && capNote !== null && totals !== null;
        });

        compare(list.count, EncoderController.captureDeviceRows.length);
        compare(addButton.enabled,
               !EncoderController.captureDeviceCapReached && !EncoderController.busy);
        compare(capNote.visible, EncoderController.captureDeviceCapReached);
        compare(totals.visible, EncoderController.captureDeviceCount > 0);
    }

    function test_cliLineCarriesCapture2TokenIffTwoDevicesAreSelected() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        win.inputMode = "live";

        const hasToken = win.cliLine.indexOf("capture2=") >= 0;
        compare(hasToken, EncoderController.captureDeviceRows.length > 1);
        if (hasToken) {
            verify(win.cliLine.indexOf(
                       "capture2=" + EncoderController.captureDeviceRows[1].deviceIndex) >= 0);
        }
    }

    function test_liveDownmixLegAndSecondDeviceStateDefaultToIdle() {
        compare(EncoderController.liveActive, false);
        compare(EncoderController.liveDownmixLeg, false);
        compare(EncoderController.liveSecondDeviceActive, false);
        compare(EncoderController.liveSecondDeviceName, "");
        compare(EncoderController.liveDriftPpm, 0);
        compare(EncoderController.liveDriftText, "");
    }

    function test_chainCaptureCellShowsTheMasterNameAndHidesTheDriftLineWhenIdle() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        openLiveSessionTab(win);

        let nameText = null;
        let driftText = null;
        tryVerify(() => {
            nameText = findChild(win.contentItem, "chainCaptureName");
            driftText = findChild(win.contentItem, "chainCaptureDrift");
            return nameText !== null && driftText !== null;
        });

        compare(nameText.text, win.liveMasterCaptureName.length > 0
                              ? win.liveMasterCaptureName : qsTr("Capture device"));
        // liveDriftText is empty outside a two-device session (this suite
        // never starts a real one), so the readout stays hidden.
        compare(driftText.visible, false);
    }

    function test_receiverReportRowsAndGapBannerReadIdleDefaultsAndFormulas() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        openLiveSessionTab(win);

        let format = null;
        let input = null;
        let gapBanner = null;
        tryVerify(() => {
            format = findChild(win.contentItem, "receiverReportFormat");
            input = findChild(win.contentItem, "receiverReportInput");
            gapBanner = findChild(win.contentItem, "liveGapMessage");
            return format !== null && input !== null && gapBanner !== null;
        });

        // Idle: no passthrough leg exists at all, so both rows read the
        // documented dash regardless of codec/atmos/downmix-leg state.
        compare(EncoderController.livePassthrough, false);
        compare(format.text, "—");
        compare(input.text, "—");
        compare(gapBanner.parent.visible, EncoderController.liveGap);
        compare(EncoderController.liveGap, false);
    }

    function test_layoutSwitcherLegendVisibilityFollowsItsDocumentedFormula() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        openLiveSessionTab(win);

        let capped = null;
        let full = null;
        tryVerify(() => {
            capped = findChild(win.contentItem, "liveLayoutLegendCapped");
            full = findChild(win.contentItem, "liveLayoutLegendFull");
            return capped !== null && full !== null;
        });

        compare(capped.visible, !EncoderController.atmosEnabled
                                && EncoderController.livePassthrough
                                && !EncoderController.liveReceiverEac3);
        compare(full.visible, !EncoderController.atmosEnabled
                              && EncoderController.livePassthrough
                              && EncoderController.liveReceiverEac3);
        // Idle, both are false together - livePassthrough itself is false,
        // so neither formula can be true no matter what else varies.
        compare(EncoderController.livePassthrough, false);
        compare(capped.visible, false);
        compare(full.visible, false);
    }

    // switchLiveReceiver's hot-swap re-evaluates the downmix leg's
    // capability check between frames (see encoder_controller.cpp's
    // wants_downmix_leg) - not drivable without a real session, but its
    // no-op-outside-a-session guarantee already covered by
    // tst_live_session.qml's own test extends to liveDownmixLeg too: a
    // refused hot-swap call must never turn the leg on.
    function test_switchLiveReceiverNeverTurnsOnTheDownmixLegOutsideASession() {
        compare(EncoderController.liveActive, false);
        compare(EncoderController.liveDownmixLeg, false);

        EncoderController.switchLiveReceiver(0);
        EncoderController.switchLiveReceiver(-1);

        compare(EncoderController.liveDownmixLeg, false);
    }
}
