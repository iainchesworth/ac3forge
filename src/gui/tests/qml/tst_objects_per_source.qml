import QtQuick
import QtTest

import Ac3Forge

// Object mode addresses an object by a flat index into the concatenated
// source list - the same addressing plan::Assignment uses for the regular
// channel-routing path (see EncoderController::objectSourceLabel's own
// comment). Two things follow once more than one source can be loaded:
// objectModel's own sourceLabel should name which FILE an object came from,
// not just a channel number nothing distinguishes source-to-source; and
// authored position/motion is keyed by (source, channel) identity, not by
// an object's position in the list (EncoderController::ObjectKey), so
// removing a source from the middle - which shifts every later source index
// down - must not lose a SURVIVING source's authored state, only the
// departed source's own.
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

    function test_removingAMiddleSourceKeepsSurvivingSourcesObjectState() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = true;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(surroundUrl);       // objects 2-7
        tryVerify(() => EncoderController.objectModel.length === 8);
        EncoderController.addSourceFile(stereoUrl);         // objects 8-9
        tryVerify(() => EncoderController.objectModel.length === 10);

        // A distinctive position and a keyframe for the third source's
        // first channel - nowhere near any default spread position (see
        // refreshObjectConfigs' own x formula, which never reaches 0.9).
        EncoderController.setObjectPosition(8, 0.9, 0.9, 0.5);
        compare(EncoderController.objectModel[8].x, 0.9);
        EncoderController.addObjectKeyframe(8, 1.5);
        compare(EncoderController.objectKeyframes(8).length, 1);

        // Removing the MIDDLE source (the surround file, sourceModel index
        // 1) shifts every later source index down by one - the third
        // source becomes source index 1. The assignment table still clears
        // (a row there addressed a position, and every later one just
        // changed - see source-assignment.md), so nothing rides as an
        // object again until channels are reassigned. But object state
        // itself is keyed by (source, channel) identity, not position (see
        // EncoderController::ObjectKey), so it survives the shift dormant
        // rather than being wiped - only the departed source's OWN entries
        // are dropped.
        EncoderController.removeSource(1);
        tryVerify(() => EncoderController.objectModel.length === 0);
        compare(EncoderController.objectCount, 0);
        verify(EncoderController.unassignedWarnings.length > 0);

        // The primary's first channel was never touched - assigning IT to
        // an object gets a fresh default, not the 0.9/0.9 authored for a
        // different channel.
        EncoderController.setAssignment(0, 0, "obj");
        tryVerify(() => EncoderController.objectModel.length === 1);
        verify(Math.abs(EncoderController.objectModel[0].x - 0.9) > 0.01
               || Math.abs(EncoderController.objectModel[0].y - 0.9) > 0.01);
        compare(EncoderController.objectModel[0].hasPath, false);
        compare(EncoderController.objectKeyframes(0).length, 0);
        const primaryLabel = EncoderController.sourceModel[0].label;
        compare(EncoderController.objectModel[0].sourceLabel, primaryLabel + " ch 1");

        // The surviving third source (now source index 1, channel 0) is
        // the one the 0.9/0.9/0.5 position and the keyframe were actually
        // authored on - assigning it back to an object recovers them
        // exactly, proving the removal above never touched it.
        EncoderController.setAssignment(1, 0, "obj");
        tryVerify(() => EncoderController.objectModel.length === 2);
        const recovered = EncoderController.objectModel[1];
        compare(recovered.x, 0.9);
        compare(recovered.y, 0.9);
        compare(recovered.z, 0.5);
        compare(recovered.hasPath, true);
        compare(EncoderController.objectKeyframes(1).length, 1);
        compare(EncoderController.objectKeyframes(1)[0].time, 1.5);
        const thirdLabel = EncoderController.sourceModel[1].label;
        compare(EncoderController.objectModel[1].sourceLabel, thirdLabel + " ch 1");
    }

    function test_reassigningAChannelAwayFromObjectAndBackKeepsItsMotion() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = true;

        EncoderController.loadSourceFile(surroundUrl);
        tryCompare(EncoderController, "sourceReady", true);
        tryVerify(() => EncoderController.objectModel.length === 6);

        // Explicit assignments for every channel, all to "an object" - the
        // moment ANY channel gets an explicit destination, automatic
        // routing stops applying to the rest too (see
        // source-assignment.md), so this is the baseline the test then
        // edits one entry of rather than relying on the implicit default.
        for (let c = 0; c < 6; c++) {
            EncoderController.setAssignment(0, c, "obj");
        }
        tryVerify(() => EncoderController.objectModel.length === 6);

        EncoderController.setObjectPosition(2, 0.85, 0.2, 0.6);
        EncoderController.addObjectKeyframe(2, 2.0);
        compare(EncoderController.objectKeyframes(2).length, 1);

        // Sending channel 2 to the bed's L instead removes it from the
        // dynamic-object list (five objects left). Its motion does not
        // travel with the position it used to hold in that list; it stays
        // with the channel.
        EncoderController.setAssignment(0, 2, "L");
        tryVerify(() => EncoderController.objectModel.length === 5);

        // Sending it back to "an object" - it lands wherever the current
        // dynamic-object order puts it, but its own authored position and
        // keyframe come back untouched.
        EncoderController.setAssignment(0, 2, "obj");
        tryVerify(() => EncoderController.objectModel.length === 6);
        const restored = EncoderController.objectModel.find(
            (obj) => obj.sourceLabel === "Ch 3");
        verify(restored !== undefined);
        compare(restored.x, 0.85);
        compare(restored.y, 0.2);
        compare(restored.z, 0.6);
        compare(restored.hasPath, true);
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
