import QtQuick
import QtTest

import Ac3Forge

// Bundle A - the Objects tab's timeline and time model: a derived
// programme length (A1), per-source start offsets (A2), programme-
// absolute keyframes with a "move keys with source" shift (A3), and
// zoom/snap tiers (A4). Timeline UI interactions (drags, wheel, clicks)
// are not driven here - this suite's established convention is testing
// motion/timeline behaviour at the EncoderController/objectsTab API level
// rather than simulating pixel-perfect mouse gestures on custom-drawn
// items (see tst_sweep_conformance.qml's own keyframe tests for the same
// pattern).
TestCase {
    id: testCase
    name: "TimelineTimeModel"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url surroundUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-51.wav")
    readonly property url offsetOutputUrl:
        Qt.resolvedUrl("_test_output/tst_timeline_offset.ec3")
    readonly property url exportedPathsUrl:
        Qt.resolvedUrl("_test_output/tst_timeline_export.txt")

    function findObjectsTab(win) {
        let tab = null;
        tryVerify(() => {
            tab = findChild(win.contentItem, "objectsTab");
            return tab !== null;
        });
        return tab;
    }

    function test_timelineLengthDerivesFromSourcesAndOffsets() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount > 0);

        const objectsTab = findObjectsTab(win);

        // A single source with no offset: the derived length is just its
        // own duration (roundtrip-stereo.wav is 1.024 s).
        fuzzyCompare(objectsTab.timelineLength, EncoderController.sourceModel[0].seconds, 0.001);

        // A second source further out than the first grows the derived
        // length to max(offset + duration) over every source, not just
        // whichever source is longest on its own.
        EncoderController.addSourceFile(surroundUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2);
        EncoderController.setSourceOffset(1, 10);
        compare(EncoderController.sourceModel[1].offsetSeconds, 10);
        tryVerify(() => Math.abs(objectsTab.timelineLength - 11.024) < 0.001);

        EncoderController.setSourceOffset(1, 0);
        EncoderController.atmosEnabled = false;
    }

    function test_encodeWithASourceOffsetProducesADoneRun() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);

        // Leading silence for the whole source - encodeChannels'
        // apply_channel_offsets bakes this in before the existing zero-
        // pad-past-the-end loop ever runs; this only checks the run
        // completes honestly, not exact sample values (the CLI's own
        // offset= tests decode and compare real frame counts/silence).
        EncoderController.setSourceOffset(0, 0.5);
        compare(EncoderController.sourceModel[0].offsetSeconds, 0.5);

        const before = EncoderController.runs.length;
        EncoderController.encodeTo(offsetOutputUrl);
        tryCompare(EncoderController, "busy", false, 10000);
        compare(EncoderController.runs.length, before + 1);
        compare(EncoderController.runs[0].status, "done");

        EncoderController.setSourceOffset(0, 0);
    }

    function test_shiftObjectKeyframesMovesEveryKeyClampedAtZero() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(surroundUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount === 6);

        EncoderController.clearObjectPath(0);
        EncoderController.addObjectKeyframe(0, 0.2);
        EncoderController.addObjectKeyframe(0, 0.5);
        EncoderController.addObjectKeyframe(0, 0.8);
        compare(EncoderController.objectKeyframes(0).length, 3);

        // A positive shift moves every key by the same delta - the "move
        // keys with source" clip-band drag, with no clamping in play.
        EncoderController.shiftObjectKeyframes(0, 1.0);
        let keys = EncoderController.objectKeyframes(0);
        compare(keys.length, 3);
        fuzzyCompare(keys[0].time, 1.2, 0.001);
        fuzzyCompare(keys[1].time, 1.5, 0.001);
        fuzzyCompare(keys[2].time, 1.8, 0.001);

        // A shift larger than the earliest key's own time clamps the
        // WHOLE path at 0 together - every key moves by the same reduced
        // delta, so the path's shape (relative spacing) survives, rather
        // than only the earliest key stopping short and the path
        // deforming.
        EncoderController.shiftObjectKeyframes(0, -5.0);
        keys = EncoderController.objectKeyframes(0);
        compare(keys.length, 3);
        fuzzyCompare(keys[0].time, 0.0, 0.001);
        fuzzyCompare(keys[1].time, 0.3, 0.001);
        fuzzyCompare(keys[2].time, 0.6, 0.001);

        EncoderController.clearObjectPath(0);
        EncoderController.atmosEnabled = false;
    }

    function test_objectModelExposesSourceIndexForClipBandTargeting() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(surroundUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2);
        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectModel.length === 8);

        // Objects 0-1 are the stereo file's own two channels (sourceIndex
        // 0); objects 2-7 are the surround file's six (sourceIndex 1) -
        // this is what the clip-band's shift-drag uses to find "every
        // object this source owns" without parsing sourceLabel text.
        const objects = EncoderController.objectModel;
        compare(objects[0].sourceIndex, 0);
        compare(objects[1].sourceIndex, 0);
        for (let i = 2; i < 8; i++) {
            compare(objects[i].sourceIndex, 1);
        }

        EncoderController.atmosEnabled = false;
    }

    function test_exportObjectPathsWritesTheDocumentedGrammar() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(surroundUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount === 6);

        EncoderController.clearObjectPath(0);
        EncoderController.addObjectKeyframe(0, 0.0);
        EncoderController.addObjectKeyframe(0, 0.5);

        // Neither synchronous nor async QML XMLHttpRequest reliably reads
        // a local file back in this offscreen test environment (the
        // synchronous form throws "Invalid state"; the async form's
        // onreadystatechange never reaches DONE), so this checks the write
        // itself succeeds rather than re-parsing the result - the format
        // exportObjectPaths writes (parse_path_file's own grammar,
        // src/cli/main.cpp: "object_index time_s x y z gain lfe_send" per
        // line, addressed by flat channel index) is what the CLI side's
        // own atmos-encode-with-keyframes-file tests exercise end to end.
        const ok = EncoderController.exportObjectPaths(exportedPathsUrl);
        verify(ok);

        EncoderController.clearObjectPath(0);
        EncoderController.atmosEnabled = false;
    }

    function test_zoomClampsAndSnapFollowsTheDocumentedTiers() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount > 0);

        const objectsTab = findObjectsTab(win);

        // Zoom clamps to [1, maxZoom] regardless of how far past either
        // edge it is asked to go.
        objectsTab.setZoom(1000, 0);
        compare(objectsTab.zoomFactor, objectsTab.maxZoom);
        objectsTab.setZoom(0, 0);
        compare(objectsTab.zoomFactor, 1);

        // Snap increment follows how many pixels a second currently spans
        // - the documented 1 s coarse / 0.1 s fine / 32 ms floor
        // progression (one 1536-sample OAMD frame at 48 kHz).
        compare(objectsTab.zoomTier(30), "coarse");
        compare(objectsTab.snapIncrement(30), 1.0);
        compare(objectsTab.zoomTier(100), "medium");
        compare(objectsTab.snapIncrement(100), 0.1);
        compare(objectsTab.zoomTier(500), "fine");
        fuzzyCompare(objectsTab.snapIncrement(500), 0.032, 0.0001);

        // The ruler's tick promotion moves in lockstep with the same tiers.
        compare(objectsTab.tickInterval(30), 10.0);
        compare(objectsTab.tickInterval(100), 1.0);
        fuzzyCompare(objectsTab.tickInterval(500), 0.1, 0.0001);

        EncoderController.atmosEnabled = false;
    }
}
