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

    function test_clickingAdvancedRevealsTheHiddenTabs() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(win.advanced, false);

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

        compare(win.advanced, true);
    }
}
