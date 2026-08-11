import QtQuick
import QtTest

import Ac3Forge

// Object mode addresses an object by a flat index into the concatenated
// source list - the same addressing plan::Assignment uses for the regular
// channel-routing path (see EncoderController::objectSourceLabel's own
// comment). Two things follow once more than one source can be loaded:
// objectModel's own sourceLabel should name which FILE an object came from,
// not just a channel number nothing distinguishes source-to-source; and
// removing a source from the middle of the list - which shifts every later
// index down - must not let authored position/motion silently reattach to
// whatever different channel now sits at an index it used to own.
TestCase {
    id: testCase
    name: "ObjectsPerSource"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url surroundUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-51.wav")

    function test_objectLabelsNameTheSourceOnceMoreThanOneIsLoaded() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = true;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        // One source: unchanged from what this has always shown - a plain
        // channel number, since there is nothing a filename would add.
        compare(EncoderController.objectModel.length, 2);
        compare(EncoderController.objectModel[0].sourceLabel, "Ch 1");
        compare(EncoderController.objectModel[1].sourceLabel, "Ch 2");

        EncoderController.addSourceFile(surroundUrl);
        // objectModel's own NOTIFY (objectsChanged) has to fire on its own
        // here - nothing else in this test touches an individual object to
        // trigger it incidentally, which is exactly the gap this covers.
        tryVerify(() => EncoderController.objectModel.length === 8);

        const stereoLabel = EncoderController.sourceModel[0].label;
        const surroundLabel = EncoderController.sourceModel[1].label;
        compare(EncoderController.objectModel[0].sourceLabel, stereoLabel + " ch 1");
        compare(EncoderController.objectModel[1].sourceLabel, stereoLabel + " ch 2");
        compare(EncoderController.objectModel[2].sourceLabel, surroundLabel + " ch 1");
        compare(EncoderController.objectModel[7].sourceLabel, surroundLabel + " ch 6");
    }

    function test_removingAMiddleSourceResetsAuthoredObjectState() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = true;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(surroundUrl);       // objects 2-7
        tryVerify(() => EncoderController.objectModel.length === 8);
        EncoderController.addSourceFile(stereoUrl);         // objects 8-9
        tryVerify(() => EncoderController.objectModel.length === 10);

        // A distinctive position for the third source's first channel -
        // nowhere near any default spread position (see
        // refreshObjectConfigs' own x formula, which never reaches 0.9).
        EncoderController.setObjectPosition(8, 0.9, 0.9, 0.5);
        compare(EncoderController.objectModel[8].x, 0.9);

        // Removing the MIDDLE source (the surround file, sourceModel index
        // 1) drops objects 2-7 and shifts what was index 8 down to index 2
        // - a different channel now sits where the authored position used
        // to point.
        EncoderController.removeSource(1);
        tryVerify(() => EncoderController.objectModel.length === 4);
        compare(EncoderController.objectCount, 4);

        // Index 2 must NOT have inherited the stale 0.9/0.9 position - it
        // is back to a fresh default, proving the reset actually ran rather
        // than resize() silently preserving the old array contents at their
        // old indices.
        verify(Math.abs(EncoderController.objectModel[2].x - 0.9) > 0.01
               || Math.abs(EncoderController.objectModel[2].y - 0.9) > 0.01);
        compare(EncoderController.objectModel[2].hasPath, false);
        compare(EncoderController.objectKeyframes(2).length, 0);

        // The survivors are exactly the two loaded files, in their new
        // (post-removal) order.
        const primaryLabel = EncoderController.sourceModel[0].label;
        const secondLabel = EncoderController.sourceModel[1].label;
        compare(EncoderController.objectModel[0].sourceLabel, primaryLabel + " ch 1");
        compare(EncoderController.objectModel[2].sourceLabel, secondLabel + " ch 1");
    }

    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_objects_per_source.eb3")

    function test_encodingObjectsFromTwoSourcesProducesADoneRun() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = true;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(surroundUrl);
        tryVerify(() => EncoderController.objectModel.length === 8);

        const before = EncoderController.runs.length;
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        compare(EncoderController.runs.length, before + 1);
        compare(EncoderController.runs[0].status, "done");
    }
}
