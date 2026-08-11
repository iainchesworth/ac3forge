import QtQuick
import QtTest

import Ac3Forge

// Guided's own step sequence (GuidedWizard.qml) - real simulated clicks
// through Back/Next and the step dots, not property pokes, over the SAME
// EncoderController state Advanced/Expert read and write. The core promise
// under test throughout: there is no separate wizard draft, so anything set
// from a wizard step is exactly what Expert would show for the same field.
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

    // EncoderController is a singleton shared across every test function in
    // the whole suite (see e.g. tst_run_history.qml's own note on this) -
    // codecIndex in particular decides whether the "rate" step exists at
    // all, so every test here sets it explicitly rather than trusting
    // whatever an earlier, alphabetically-prior test left behind.

    // Guided is the DEFAULT tier - the one page in the two-pane window's
    // StackLayout that is current from the very first frame, never reached
    // via an explicit currentIndex change. The window itself (and the
    // Layout chain feeding this page's real geometry) still take a handful
    // of polish passes after creation to settle, same as any freshly-shown
    // window; interactively that is imperceptible, but this window is
    // created and clicked into within the same scripted tick. Confirmed
    // empirically (not guessed): a real mouseClick() issued immediately
    // after createTemporaryObject() lands without effect, one issued after
    // a short wait does not - so this waits, the same "poll/wait rather
    // than assume the tree is fully realised the instant the window is
    // created" rule tst_format_channels.qml's own comment already states
    // for a freshly-realised Repeater.
    function waitForWizardLayout(win) {
        wait(300);
        return findChild(win.contentItem, "guidedWizard");
    }

    function test_nextIsGatedOnASourceAndBackRetreatsOneStepAtATime() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.codecIndex = 0;  // AC-3 - no "rate" step to skip past
        const wizard = waitForWizardLayout(win);

        const nextButton = findChild(win.contentItem, "wizardNextButton");
        const backButton = findChild(win.contentItem, "wizardBackButton");
        verify(nextButton !== null);
        verify(backButton !== null);

        compare(wizard.currentStepKey, "source");
        compare(backButton.enabled, false);
        compare(nextButton.enabled, EncoderController.sourceReady);

        if (!EncoderController.sourceReady) {
            EncoderController.loadSourceFile(stereoUrl);
            tryCompare(EncoderController, "sourceReady", true);
        }
        compare(nextButton.enabled, true);

        mouseClick(nextButton);
        compare(wizard.currentStepKey, "format");
        mouseClick(nextButton);
        // AC-3: "rate" was filtered out, so Format leads straight to Loudness.
        compare(wizard.currentStepKey, "loudness");
        mouseClick(nextButton);
        compare(wizard.currentStepKey, "review");
        compare(nextButton.visible, false);

        mouseClick(backButton);
        compare(wizard.currentStepKey, "loudness");
        mouseClick(backButton);
        compare(wizard.currentStepKey, "format");
    }

    function test_dolbyDigitalPlusAddsTheRateStepAndRemovingItRelocatesCleanly() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.codecIndex = 0;  // AC-3 to start
        const wizard = waitForWizardLayout(win);
        wizard.currentStepKey = "format";
        compare(wizard.activeSteps.some((s) => s.key === "rate"), false);

        // seg-<value> is SegmentedControl.qml's own convention - "eac3"/
        // "ac3" are this control's own model values (distinct from the
        // header tier control's "seg-guided"/"seg-advanced"/"seg-expert",
        // so the two never collide).
        const eac3Seg = findChild(win.contentItem, "seg-eac3");
        verify(eac3Seg !== null);
        mouseClick(eac3Seg);
        compare(EncoderController.codecIndex, 1);
        compare(wizard.activeSteps.some((s) => s.key === "rate"), true);

        wizard.currentStepKey = "rate";
        compare(wizard.currentStepIndex, wizard.activeSteps.findIndex((s) => s.key === "rate"));

        // Switching back to AC-3 drops "rate" while it is the current step -
        // onActiveStepsChanged has to land somewhere real, not on a step
        // that no longer exists. Only the current step's Component is ever
        // instantiated (GuidedWizard's own Loader - see its file comment on
        // why), so "format"'s own codec control - findable a moment ago -
        // no longer exists to click while "rate" is current; setting
        // codecIndex directly is exactly what that same click already
        // proved it does, and is the only way left to reach this state from
        // here.
        EncoderController.codecIndex = 0;
        compare(wizard.activeSteps.some((s) => s.key === "rate"), false);
        verify(wizard.activeSteps.some((s) => s.key === wizard.currentStepKey));
    }

    function test_presetButtonsAndDualMonoWriteTheSameStateExpertReads() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.codecIndex = 0;
        EncoderController.applyChannelPreset("5.1");
        const wizard = waitForWizardLayout(win);
        wizard.currentStepKey = "format";

        const preset71 = findChild(win.contentItem, "wizardPreset-7.1");
        verify(preset71 !== null);
        mouseClick(preset71);
        compare(EncoderController.channelShapeName, "7.1");
        // 7.1 needs a dependent substream - the wizard's own preset button
        // has to upgrade the codec exactly like the Format tab's own preset
        // row does (applyChannelPreset's own job, not something the wizard
        // duplicates), since there is only one currentPlan() either reads.
        compare(EncoderController.codecIndex, 1);

        const dualMonoButton = findChild(win.contentItem, "wizardDualMonoButton");
        verify(dualMonoButton !== null);
        mouseClick(dualMonoButton);
        compare(EncoderController.dualMono, true);
        compare(EncoderController.channelShapeName, "1+1");
    }
}
