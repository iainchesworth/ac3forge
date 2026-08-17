import QtQuick
import QtTest

import Ac3Forge

// The decode-side counterpart to tst_qc_panel.qml: ObjectDecodeController (a
// decode-and-collect pass over an ALREADY-ENCODED E-AC-3 file, deliberately
// separate from QcController/EncoderController - see
// object_decode_controller.hpp's own header comment) plus
// ObjectInspectorDialog.qml, the room view it drives. ObjectDecodeController
// is a singleton, like QcController - shared across every test function in
// this whole suite, so each test starts from a fresh inspectFile() call
// rather than assuming what an earlier test left behind.
TestCase {
    id: testCase
    name: "ObjectInspector"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // A real, already-encoded Dolby Atmos E-AC-3 stream - three objects
    // circling continuously for real (not a single static frame; see
    // CONTRIBUTING.md's "test with real audio, from frame 1 onward" rule),
    // generated the same way examples/atmos_objects.cpp's own end-to-end
    // proof is. NOT fuzz/seeds/fuzz_eac3_decode/atmos-objects.ec3, despite
    // the similar name: that corpus seed decodes cleanly (it exists to fuzz
    // robustness, not to exercise a real payload) but carries no OAMD this
    // decoder recognises - verified directly, 0 of its 32 frames carry
    // object_metadata. This fixture is this test's own, kept beside it
    // rather than added to the shared fuzz corpus, so it stays this
    // dialog's test data and nothing about the corpus changes as a side
    // effect of adding it.
    readonly property url objectStreamUrl:
        Qt.resolvedUrl("../fixtures/atmos-objects.ec3")

    function init() {
        ObjectDecodeController.stopAudition();
    }

    // Proves two things at once: the decode genuinely runs off the GUI
    // thread (busy flips true synchronously, then back to false once the
    // worker's queued completion lands - the same tryCompare pattern
    // tst_qc_panel.qml already uses to prove QcController's own measurement
    // is asynchronous), and what it finds is REAL decoded data, not a
    // placeholder: real object positions move between the first and last
    // frame rather than sitting frozen at Position's own (0.5, 0.5, 0.0)
    // default.
    function test_inspectingRealObjectStreamIsAsyncAndReportsRealMotion() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        ObjectDecodeController.inspectFile(objectStreamUrl);
        compare(ObjectDecodeController.busy, true);
        tryCompare(ObjectDecodeController, "busy", false, 15000);

        compare(ObjectDecodeController.error, "");
        compare(ObjectDecodeController.hasResult, true);
        verify(ObjectDecodeController.summaryLine.indexOf("E-AC-3") === 0);
        verify(ObjectDecodeController.summaryLine.indexOf("Hz") > 0);
        verify(ObjectDecodeController.frameCount > 1);

        const frames = ObjectDecodeController.frames;
        compare(frames.length, ObjectDecodeController.frameCount);

        const first = frames[0];
        const last = frames[frames.length - 1];
        verify(first.objects.length > 0);
        compare(last.objects.length, first.objects.length);

        // Every position stays inside the room-anchored coordinate system
        // (§4.2.1: x/y in [0, 1], z in [-1, 1]) on every frame, not just
        // the first.
        for (let f = 0; f < frames.length; f++) {
            for (let o = 0; o < frames[f].objects.length; o++) {
                const p = frames[f].objects[o];
                verify(p.x >= 0.0 && p.x <= 1.0);
                verify(p.y >= 0.0 && p.y <= 1.0);
                verify(p.z >= -1.0 && p.z <= 1.0);
            }
        }

        // Real motion, not a frozen placeholder: at least one object's
        // position differs between the first and last decoded frame.
        let moved = false;
        for (let o = 0; o < first.objects.length; o++) {
            const a = first.objects[o];
            const b = last.objects[o];
            if (Math.abs(a.x - b.x) > 1e-6 || Math.abs(a.y - b.y) > 1e-6
                    || Math.abs(a.z - b.z) > 1e-6) {
                moved = true;
            }
        }
        verify(moved);
    }

    // The dialog's room views draw one marker per decoded object at the
    // current scrub frame, in both the plan and the elevation - real data
    // reaching the screen, not just the controller.
    function test_dialogRendersOneMarkerPerObject() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        ObjectDecodeController.inspectFile(objectStreamUrl);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.hasResult, true);

        win.objectInspectorDialogRef.open();
        tryVerify(() => win.objectInspectorDialogRef.opened);

        const objectCount = ObjectDecodeController.frames[0].objects.length;

        const planMarkers = findChild(win.contentItem, "oiPlanMarkers");
        verify(planMarkers !== null);
        compare(planMarkers.count, objectCount);

        const elevationMarkers = findChild(win.contentItem, "oiElevationMarkers");
        verify(elevationMarkers !== null);
        compare(elevationMarkers.count, objectCount);

        const rows = findChild(win.contentItem, "oiObjectRows");
        verify(rows !== null);
        compare(rows.count, objectCount);

        // One audition button per object, reachable and labelled - the
        // playback path itself is not exercised here (it opens a real
        // platform audio device, which this offscreen suite does not
        // assume is present, the same reason no existing test in this
        // suite drives MonitorSink/motion preview either).
        for (let i = 0; i < objectCount; i++) {
            const rowItem = rows.itemAt(i);
            verify(rowItem !== null, "row " + i + " of " + objectCount + " not realized by Repeater.itemAt");
            let auditionButton = null;
            tryVerify(() => {
                auditionButton = findChild(rowItem, "oiAuditionButton-" + i);
                return auditionButton !== null;
            }, 5000, "audition button " + i + " not found under its own row item");
            compare(auditionButton.text, "Audition");
        }

        win.objectInspectorDialogRef.close();
    }

    // Scrubbing moves which frame's positions are on screen - the slider
    // drives frameIndex, and the room markers/object rows follow it, not a
    // permanently-first-frame snapshot.
    function test_scrubbingMovesToADifferentDecodedFrame() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        ObjectDecodeController.inspectFile(objectStreamUrl);
        tryCompare(ObjectDecodeController, "busy", false, 15000);
        compare(ObjectDecodeController.hasResult, true);
        verify(ObjectDecodeController.frameCount > 1);

        win.objectInspectorDialogRef.open();
        tryVerify(() => win.objectInspectorDialogRef.opened);
        compare(win.objectInspectorDialogRef.frameIndex, 0);

        const lastIndex = ObjectDecodeController.frameCount - 1;
        const scrub = findChild(win.contentItem, "oiScrubSlider");
        verify(scrub !== null);
        scrub.value = lastIndex;
        scrub.moved();

        compare(win.objectInspectorDialogRef.frameIndex, lastIndex);
        compare(win.objectInspectorDialogRef.currentFrame,
                ObjectDecodeController.frames[lastIndex]);

        win.objectInspectorDialogRef.close();
    }
}
