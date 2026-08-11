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

    function test_guidedHidesTheTabBarAndShowsTheWizard() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(win.tier, "guided");

        let wizardStep = null;
        tryVerify(() => {
            wizardStep = findChild(win.contentItem, "wizardStepDot-source");
            return wizardStep !== null;
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
        mouseClick(advancedSeg);
        compare(win.tier, "advanced");
        tryVerify(() => !wizardStep.visible);
    }
}
