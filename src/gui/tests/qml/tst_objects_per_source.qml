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
        // 1) shifts every later flat index down - a different channel now
        // sits where the authored position used to point, so the controller
        // clears the whole table rather than guess. Objects follow the
        // assignments now, and with the table cleared NOTHING rides as an
        // object until channels are assigned again - the same "refuse to
        // guess" call the assignment itself makes.
        EncoderController.removeSource(1);
        tryVerify(() => EncoderController.objectModel.length === 0);
        compare(EncoderController.objectCount, 0);
        verify(EncoderController.unassignedWarnings.length > 0);

        // Assigning a surviving channel to an object brings it back with a
        // FRESH default - never the stale 0.9/0.9 authored for the channel
        // that used to sit at its index.
        EncoderController.setAssignment(0, 0, "obj");
        tryVerify(() => EncoderController.objectModel.length === 1);
        verify(Math.abs(EncoderController.objectModel[0].x - 0.9) > 0.01
               || Math.abs(EncoderController.objectModel[0].y - 0.9) > 0.01);
        compare(EncoderController.objectModel[0].hasPath, false);
        compare(EncoderController.objectKeyframes(0).length, 0);
        const primaryLabel = EncoderController.sourceModel[0].label;
        compare(EncoderController.objectModel[0].sourceLabel, primaryLabel + " ch 1");
    }

    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_objects_per_source.eb3")
    readonly property url pinnedOutputUrl:
        Qt.resolvedUrl("_test_output/tst_objects_pinned.eb3")

    function test_assignmentsPickWhichChannelsRideAsObjects() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = true;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        // Nothing assigned: every channel is a dynamic object, the behaviour
        // this path has always had.
        compare(EncoderController.objectCount, 2);

        // ch 1 pins to the bed's L as a static object. The moment anything
        // is explicit, the table is the whole truth: ch 2 is unassigned, so
        // nothing rides as a dynamic object yet and the warning names it.
        EncoderController.setAssignment(0, 0, "L");
        compare(EncoderController.objectCount, 0);
        compare(EncoderController.unassignedWarnings.length, 1);
        // ch 2 to an object — the canon "helicopter is an object while
        // orbit51 feeds the bed" shape, on the smallest source that shows it.
        EncoderController.setAssignment(0, 1, "obj");
        compare(EncoderController.objectCount, 1);
        compare(EncoderController.unassignedWarnings.length, 0);

        const before = EncoderController.runs.length;
        EncoderController.encodeTo(pinnedOutputUrl);
        tryCompare(EncoderController, "busy", false, 10000);
        compare(EncoderController.runs.length, before + 1);
        compare(EncoderController.runs[0].status, "done");
    }

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
