import QtQuick
import QtTest

import Ac3Forge

// The meters' CLIP box used to reflect only the newest snapshot; now the
// controller latches it: once a channel clips, it stays lit until the user
// clicks it (clearClipLatch) or a new transport starts (clearClipLatches,
// called at every encode/record/live-session/motion-preview start). Forcing
// a real clip uses the bundled test signal (real, multi-cycle tones - see
// CONTRIBUTING.md's "test with real audio" rule) plus an aggressive +24dB
// trim: the signal's amplitude envelope never dips its linear amplitude
// below ~0.1, and 0.1 * 10^(24/20) is already well past full scale, so
// every sample of the trimmed channel clips regardless of the envelope's
// phase at any given moment.
TestCase {
    id: testCase
    name: "ClipLatch"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_clip_latch.ec3")

    // The controller singleton is shared across every tst_*.qml file in this
    // binary - see tst_sweep_conformance.qml's own cleanup() and its
    // comment on why leaving a loaded source/assignment here would break
    // another file.
    function cleanup() {
        if (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.bitrateKbps = 192;
        EncoderController.applyChannelPreset("stereo");
    }

    function test_clipLatchStaysLitUntilClickedOrANewTransportStarts() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadBundledTestSignal();
        tryCompare(EncoderController, "sourceReady", true, 10000);
        EncoderController.applyChannelPreset("5.1");
        EncoderController.setAssignment(0, 0, "L");  // the signal's FL -> L

        const idx = EncoderController.channelNames.indexOf("L");
        verify(idx >= 0);

        EncoderController.setAssignmentTrim(0, 0, 24);
        tryVerify(() => EncoderController.channelLevels[idx].clipped === true, 10000);

        // Removing the trim stops the RAW clipping, but the latch must
        // still read true across a fresh preview publish - that is the
        // entire point of a latch over the newest snapshot alone.
        EncoderController.setAssignmentTrim(0, 0, 0);
        wait(800);  // let another background preview pass land
        compare(EncoderController.channelLevels[idx].clipped, true);

        // Clicking (clearClipLatch, ChannelMeter's own handler) clears just
        // this channel, immediately - not on the next tick.
        EncoderController.clearClipLatch(idx);
        compare(EncoderController.channelLevels[idx].clipped, false);
        wait(800);  // not clipping any more - stays cleared, not relatched
        compare(EncoderController.channelLevels[idx].clipped, false);

        // An out-of-range channel is a safe no-op.
        EncoderController.clearClipLatch(-1);
        EncoderController.clearClipLatch(9999);

        // A fresh transport clears every latch automatically. Trim is
        // already back at 0 (no genuine clip this run), so a lingering
        // `true` after the run finishes could only come from a stale
        // latch clearClipLatches() failed to reset at the run's start -
        // never from this run's own actual audio.
        EncoderController.setAssignmentTrim(0, 0, 24);
        tryVerify(() => EncoderController.channelLevels[idx].clipped === true, 10000);
        EncoderController.setAssignmentTrim(0, 0, 0);

        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 20000);
        compare(EncoderController.runs[0].status, "done");
        compare(EncoderController.channelLevels[idx].clipped, false);
    }
}
