import QtQuick
import QtTest

import Ac3Forge

// Guided's step sequence (GuidedWizard.qml) — the handoff's five steps
// (Audio · Speakers · Quality · Movement · Where it goes) — driven by real
// simulated clicks over the SAME EncoderController state Advanced/Expert
// read and write. The core promise under test throughout: there is no
// separate wizard draft, so anything set from a wizard step is exactly what
// Expert would show for the same field.
TestCase {
    id: testCase
    name: "GuidedWizard"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url surroundUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-51.wav")

    // The window (and the Layout chain feeding this page's real geometry)
    // takes a handful of polish passes after creation to settle; a real
    // mouseClick() issued immediately after createTemporaryObject() lands
    // without effect — same wait rule tst_format_channels.qml documents for
    // a freshly-realised Repeater.
    function waitForWizardLayout(win) {
        wait(300);
        return findChild(win.contentItem, "guidedWizard");
    }

    // The wizard's Back/Next footer sits at the foot of a scrolling panel,
    // which makes a positional mouseClick on it brittle (it needs a scroll,
    // a settled layout AND enough spacing not to read as a double-click).
    // Navigation is driven through the button's own clicked() signal instead
    // — the same handler a real click runs — with the enabled gate asserted
    // separately where it matters. The step CARDS keep real hit-tested
    // clicks; they are mid-panel and stable.
    function clickNav(win, button) {
        verify(button.enabled);
        button.clicked();
    }

    function test_nextIsGatedOnASourceAndBackRetreatsOneStepAtATime() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        const wizard = waitForWizardLayout(win);
        verify(wizard !== null);

        const nextButton = findChild(win.contentItem, "wizardNextButton");
        const backButton = findChild(win.contentItem, "wizardBackButton");
        verify(nextButton !== null);
        verify(backButton !== null);

        wizard.currentStepKey = "source";
        compare(wizard.currentStepIndex, 0);
        compare(backButton.enabled, false);
        compare(nextButton.enabled, EncoderController.sourceReady);

        if (!EncoderController.sourceReady) {
            EncoderController.loadSourceFile(stereoUrl);
            tryCompare(EncoderController, "sourceReady", true);
        }
        compare(nextButton.enabled, true);

        clickNav(win, nextButton);
        compare(wizard.currentStepKey, "setup");
        clickNav(win, nextButton);
        compare(wizard.currentStepKey, "quality");
        clickNav(win, nextButton);
        compare(wizard.currentStepKey, "motion");
        clickNav(win, nextButton);
        compare(wizard.currentStepKey, "output");

        clickNav(win, backButton);
        compare(wizard.currentStepKey, "motion");
        clickNav(win, backButton);
        compare(wizard.currentStepKey, "quality");
    }

    function test_setupCardsWriteTheSameChannelStateExpertReads() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 0;
        EncoderController.applyChannelPreset("5.1");
        const wizard = waitForWizardLayout(win);
        verify(wizard !== null);
        wizard.currentStepKey = "setup";
        wait(50);

        // 7.1.4 needs dependent substreams — the codec must FOLLOW the
        // channels (applyChannelPreset's own job, not something the wizard
        // duplicates), since there is only one currentPlan() either reads.
        const fullCard = findChild(win.contentItem, "wizardSetup-full");
        verify(fullCard !== null);
        mouseClick(fullCard);
        compare(EncoderController.channelShapeName, "7.1.4");
        compare(EncoderController.codecIndex, 1);

        // The card highlight is read back OUT of the channel state, so an
        // Advanced edit round-trips into Guided instead of the card lying.
        compare(fullCard.active, true);

        const stereoCard = findChild(win.contentItem, "wizardSetup-stereo");
        verify(stereoCard !== null);
        mouseClick(stereoCard);
        compare(EncoderController.channelShapeName, "2.0");
        compare(stereoCard.active, true);
        compare(fullCard.active, false);
    }

    function test_qualityCardsSetTheBitrate() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        const wizard = waitForWizardLayout(win);
        verify(wizard !== null);
        wizard.currentStepKey = "quality";
        wait(50);

        const card448 = findChild(win.contentItem, "wizardRate-448");
        verify(card448 !== null);
        mouseClick(card448);
        compare(EncoderController.bitrateKbps, 448);
        compare(card448.active, true);

        const card192 = findChild(win.contentItem, "wizardRate-192");
        verify(card192 !== null);
        mouseClick(card192);
        compare(EncoderController.bitrateKbps, 192);
    }

    function test_roomPickerDerivesTheBedFromTheRoomsParts() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 0;
        EncoderController.applyChannelPreset("5.1");
        const wizard = waitForWizardLayout(win);
        verify(wizard !== null);
        wizard.currentStepKey = "setup";
        wizard.roomPicker = true;
        wait(100);

        // 5.1 reads back as fronts + centre + sides + one sub.
        compare(wizard.roomFronts, true);
        compare(wizard.roomCentre, true);
        compare(wizard.roomSurround, "sides");
        compare(wizard.roomSubs, 1);

        // A single speaker behind instead of sides: 3/2 -> 3/1.
        const backSeg = findChild(win.contentItem, "seg-back");
        verify(backSeg !== null);
        mouseClick(backSeg);
        compare(EncoderController.channelShapeName, "4.1");

        // No centre: 3/1 -> 2/1.
        const centreOff = findChild(findChild(win.contentItem, "roomCentre"), "seg-off");
        verify(centreOff !== null);
        mouseClick(centreOff);
        compare(EncoderController.channelShapeName, "3.1");

        // No fronts collapses to the lone centre, extras and all.
        const frontsOff = findChild(findChild(win.contentItem, "roomFronts"), "seg-off");
        verify(frontsOff !== null);
        mouseClick(frontsOff);
        compare(EncoderController.channelShapeName, "1.1");

        // Back closes the sub-screen before it retreats a step.
        const backButton = findChild(win.contentItem, "wizardBackButton");
        verify(backButton !== null);
        backButton.clicked();
        compare(wizard.roomPicker, false);
        compare(wizard.currentStepKey, "setup");
    }

    function test_trajectoryPresetsAuthorRealKeyframes() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        if (!EncoderController.sourceReady) {
            EncoderController.loadSourceFile(stereoUrl);
            tryCompare(EncoderController, "sourceReady", true);
        }
        const wizard = waitForWizardLayout(win);
        verify(wizard !== null);

        EncoderController.applyChannelPreset("5.1");
        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount > 0);

        // The preset writes through setObjectPathKeyframes — the same keys
        // the Objects tab's timeline shows and encodeObjects plays back.
        // Key count follows the source's own derived duration now (one key
        // per whole second, plus a final key exactly at the end) rather
        // than a fixed 8 s - roundtrip-stereo.wav is 1.024 s, so that's
        // keys at 0 s, 1 s and 1.024 s.
        const duration = EncoderController.sourceModel[0].seconds;
        wizard.authorTrajectories("orbit");
        compare(EncoderController.objectModel[0].hasPath, true);
        compare(EncoderController.objectKeyframes(0).length, 3);
        compare(EncoderController.objectKeyframes(0)[2].time, duration);

        wizard.authorTrajectories("hold");
        compare(EncoderController.objectModel[0].hasPath, false);
        compare(EncoderController.objectKeyframes(0).length, 0);

        EncoderController.atmosEnabled = false;
    }

    function test_trajectoryPresetsSpanTheWholeDerivedProgramme() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(surroundUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2);
        // Pushes the second source's own end (1.024 s) well past 8 s, so
        // the derived programme length (offset + duration, the same
        // max() the Objects tab's timelineLength uses) covers more than
        // one 8 s lap - 20 + 1.024 = 21.024 s, three whole laps.
        EncoderController.setSourceOffset(1, 20);
        const wizard = waitForWizardLayout(win);
        verify(wizard !== null);

        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount > 0);

        wizard.authorTrajectories("orbit");
        const keys = EncoderController.objectKeyframes(0);
        // 0..21 inclusive by whole seconds (22 keys, since 21 < 21.024)
        // plus one final key exactly at 21.024 s.
        compare(keys.length, 23);
        compare(keys[keys.length - 1].time, 21.024);
        // A seamless loop: the position 8 s into the first lap is exactly
        // the position at 0 s (and at 16 s, the third lap's own start) -
        // the phase wraps every cycleSeconds rather than the whole path
        // being one single 21 s ellipse.
        fuzzyCompare(keys[8].x, keys[0].x, 0.0001);
        fuzzyCompare(keys[8].y, keys[0].y, 0.0001);
        fuzzyCompare(keys[16].x, keys[0].x, 0.0001);
        fuzzyCompare(keys[16].y, keys[0].y, 0.0001);

        wizard.authorTrajectories("hold");
        EncoderController.atmosEnabled = false;
        EncoderController.setSourceOffset(1, 0);
    }

    function test_trajectoryPresetsLoopFixedlyForALiveSession() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        // A live session has no file duration to derive a programme length
        // from - the preset falls back to looping one fixed 8 s cycle
        // instead (see authorTrajectories' own comment).
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.inputMode = "live";
        const wizard = waitForWizardLayout(win);
        verify(wizard !== null);
        compare(wizard.liveSession, true);

        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount > 0);

        wizard.authorTrajectories("orbit");
        // The old fixed-8-second shape, independent of the loaded file's
        // own (much shorter) duration: keys at 0..8 s inclusive.
        compare(EncoderController.objectKeyframes(0).length, 9);
        compare(EncoderController.objectKeyframes(0)[8].time, 8);

        wizard.authorTrajectories("hold");
        EncoderController.atmosEnabled = false;
        win.inputMode = "file";
    }

    function test_movementCardsDriveObjectModeWithItsConstraints() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 0;
        EncoderController.bitrateKbps = 192;
        EncoderController.applyChannelPreset("7.1");
        // Alphabetically this test runs first, so no earlier test has loaded
        // a source yet — without one the first-run screen hides the wizard
        // and every click lands on nothing.
        if (!EncoderController.sourceReady) {
            EncoderController.loadSourceFile(stereoUrl);
            tryCompare(EncoderController, "sourceReady", true);
            EncoderController.applyChannelPreset("7.1");
        }
        const wizard = waitForWizardLayout(win);
        verify(wizard !== null);
        wizard.currentStepKey = "motion";
        wait(50);

        // Turning movement on fixes a 5.1 bed, E-AC-3, and the 384 kbps
        // floor — atomically, per the handoff's own object-mode rule.
        const moveCard = findChild(win.contentItem, "wizardMotion-on");
        verify(moveCard !== null);
        mouseClick(moveCard);
        compare(EncoderController.atmosEnabled, true);
        verify(EncoderController.bitrateKbps >= 384);

        const stayCard = findChild(win.contentItem, "wizardMotion-off");
        verify(stayCard !== null);
        mouseClick(stayCard);
        compare(EncoderController.atmosEnabled, false);
    }
}
