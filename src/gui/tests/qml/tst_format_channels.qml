import QtQuick
import QtTest

import Ac3Forge

// A real simulated click on the live tier control, not a property poke -
// the actual point of Qt Quick Test over the existing --smoke harness's
// prop=value mechanism (see src/gui/main.cpp).
TestCase {
    id: testCase
    name: "FormatChannels"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // A freshly created window's layouts settle on a POLISH pass, which only
    // runs when the scene graph gets around to a frame - and how soon that
    // is differs by Qt version and platform plugin (Qt 6.10's offscreen
    // defers it long enough that a click issued straight after
    // createTemporaryObject() lands on pre-layout geometry, where the
    // not-yet-arranged workbench overlaps the header and eats the click;
    // 6.8 on Windows had polished already). So: don't wait a guessed number
    // of milliseconds, wait for the observable fact the click depends on -
    // the header row has laid out, which puts the tier control on the right
    // half of the window instead of at its implicit-width position.
    function waitForHeaderLayout(win, seg) {
        tryVerify(() => {
            const corner = seg.mapToItem(null, 0, 0);
            return corner.x > win.width / 2;
        });
    }

    function test_clickingExpertRevealsTheHiddenTabsAndTheTabBar() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(win.tier, "guided");

        // seg-<value> is SegmentedControl.qml's own convention (every
        // instance, everywhere it's used) - see its objectName comment. The
        // Repeater behind it needs at least one processed event before
        // findChild can see its delegates - visible:true alone does not
        // guarantee the tree is fully realised the instant the window is
        // created, so poll rather than assume either zero or one wait is
        // always enough.
        let advancedSeg = null;
        tryVerify(() => {
            advancedSeg = findChild(win.contentItem, "seg-advanced");
            return advancedSeg !== null;
        });
        waitForHeaderLayout(win, advancedSeg);
        mouseClick(advancedSeg);
        compare(win.tier, "advanced");
        // Guided's own tab bar stays hidden while Guided is not selected -
        // Advanced does show one, just not Coding tools/Metadata yet.
        compare(win.visibleTabs.some((t) => t.key === "coding"), false);

        let expertSeg = null;
        tryVerify(() => {
            expertSeg = findChild(win.contentItem, "seg-expert");
            return expertSeg !== null;
        });
        mouseClick(expertSeg);
        compare(win.tier, "expert");
        compare(win.visibleTabs.some((t) => t.key === "coding"), true);
        compare(win.visibleTabs.some((t) => t.key === "meta"), true);
    }

    function test_tickingAnExtraUnderAc3PromotesTheCodec() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.applyChannelPreset("5.1");
        EncoderController.codecIndex = 0;  // plain AC-3, 3/2 + LFE
        compare(EncoderController.extrasLocked, false);

        // The extras decide the codec, never the reverse - the circular
        // gate ("extras need DD+, DD+ needs choosing first") was a real
        // design bug the handoff calls out by name.
        EncoderController.toggleExtra("rear");
        compare(EncoderController.codecIndex, 1);
        compare(EncoderController.channelShapeName, "7.1");

        // Unticking does NOT demote: E-AC-3 with a plain bed is a real
        // encoder capability (VBR needs it), so the codec stays until the
        // now-unlocked Codec control says otherwise.
        EncoderController.toggleExtra("rear");
        compare(EncoderController.codecIndex, 1);
        EncoderController.codecIndex = 0;
        compare(EncoderController.codecIndex, 0);
    }

    function test_guidedHidesTheTabBarAndShowsTheWizard() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(win.tier, "guided");
        // Until a source has ever been chosen, the body is the first-run
        // screen and the whole workbench (wizard included) stays hidden.
        win.everHadSource = true;

        let wizardStep = null;
        tryVerify(() => {
            wizardStep = findChild(win.contentItem, "wizardStepDot-source");
            return wizardStep !== null && wizardStep.visible;
        });
        verify(wizardStep.visible);

        // GuidedWizard is one more StackLayout page, not destroyed when
        // another page is current - so the dot still exists once Advanced
        // is picked, just no longer visible (the same "still there, just
        // hidden" behaviour every other StackLayout page already relies on
        // for its own Card content). The header's own tier SegmentedControl
        // is separate from this - it switches tiers, so it stays visible
        // (and findable) regardless of which tier is current.
        const advancedSeg = findChild(win.contentItem, "seg-advanced");
        verify(advancedSeg !== null);
        waitForHeaderLayout(win, advancedSeg);
        mouseClick(advancedSeg);
        compare(win.tier, "advanced");
        tryVerify(() => !wizardStep.visible);
    }
}
