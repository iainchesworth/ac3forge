import QtQuick
import QtTest

import Ac3Forge

TestCase {
    id: testCase
    name: "RunHistory"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url fixtureUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_run_history.ac3")

    function test_encodingAddsADoneRunToTheStrip() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(fixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);

        const before = EncoderController.runs.length;
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        compare(EncoderController.runs.length, before + 1);
        const last = EncoderController.runs[EncoderController.runs.length - 1];
        compare(last.status, "done");
    }

    // --- item 27 / 33: per-run cliLine/eac3/playDeviceIndex snapshots -------

    function test_startingARunSnapshotsCliLineEac3AndPlayDeviceIndex() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(fixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.codecIndex = 1;  // E-AC-3 - this run's own eac3 reads true
        EncoderController.atmosEnabled = false;

        EncoderController.setPendingCliLine("ac3cli eac3-encode snapshot-test.wav out.ec3 448 51");
        EncoderController.setPendingPlayDevice(2);
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        // Newest first (runs' own doc comment) - this run is index 0.
        const run = EncoderController.runs[0];
        compare(run.status, "done");
        compare(run.cliLine, "ac3cli eac3-encode snapshot-test.wav out.ec3 448 51");
        compare(run.eac3, true);
        compare(run.playDeviceIndex, 2);

        // The pending values are one-shot: a run started right afterwards
        // with nothing pending reads the defaults, not what this run just
        // set.
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);
        compare(EncoderController.runs[0].cliLine, "");
        compare(EncoderController.runs[0].playDeviceIndex, -1);

        EncoderController.codecIndex = 0;
    }

    // outputDeviceSupportsFormat is outputDeviceCanBitstream's general form
    // (items 27/30) - a run chip's own Play checks a device against THAT
    // run's stored "eac3" field, and Guided's amp auto-pick checks the
    // PROSPECTIVE plan's format, neither reading the shared output_eac3_ a
    // later run could have since overwritten. Whatever real output hardware
    // this machine happens to have, an index at or past the device count is
    // always out of range - the same defensive false outputDeviceCanBitstream
    // already gives for one.
    function test_outputDeviceSupportsFormatIsFalseOutOfRange() {
        // Whatever this machine's own output hardware happens to be -
        // deviceIndex == the device count is always one past the end,
        // regardless of how many real devices this test environment has.
        const count = EncoderController.outputDevices.length;
        compare(EncoderController.outputDeviceSupportsFormat(-1, true), false);
        compare(EncoderController.outputDeviceSupportsFormat(count, true), false);
        compare(EncoderController.outputDeviceSupportsFormat(count, false), false);
    }

    // playFileToReceiver mirrors playToReceiver's own guard shape - a no-op
    // for an out-of-range device or an empty path, never a crash.
    function test_playFileToReceiverIsANoOpForAnInvalidDeviceOrEmptyPath() {
        compare(EncoderController.playing, false);
        EncoderController.playFileToReceiver("", 0);
        compare(EncoderController.playing, false);
        EncoderController.playFileToReceiver("C:/nowhere/out.ec3", -1);
        compare(EncoderController.playing, false);
    }

    // --- item 33: the per-run details popover --------------------------------

    function test_clickingARunChipOpensItsDetailsPopoverWithTheSnapshottedCliLine() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        wait(300);

        EncoderController.loadSourceFile(fixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.setPendingCliLine(
            "ac3cli encode details-popover-test.wav out.ac3 448 51");
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);
        // The run strip's Repeater needs a layout pass to actually size and
        // position the newly-added chip before a coordinate-based click can
        // land on it reliably - without this, mouseClick can land on a
        // neighbouring, already-settled chip instead (see the equivalent
        // wait in test_detailsPopoverShowsTheFailureTextForAFailedRun).
        wait(100);

        const run = EncoderController.runs[0];
        compare(run.cliLine, "ac3cli encode details-popover-test.wav out.ac3 448 51");
        const chipSummary = findChild(win.contentItem, "runChipSummary-" + run.id);
        verify(chipSummary !== null);
        mouseClick(chipSummary);

        compare(win.detailsRunId, run.id);
        tryCompare(win.runDetailsPopup, "visible", true);
        // window.detailsRun's own id-keyed lookup resolves to the same run
        // the chip was clicked on, not just whatever runs() happened to
        // return moments earlier.
        compare(win.detailsRun.cliLine, run.cliLine);

        const cliLineText = findChild(win.runDetailsPopup, "runDetailsCliLine");
        verify(cliLineText !== null);

        const closeButton = findChild(win.runDetailsPopup, "runDetailsClose");
        verify(closeButton !== null);
        mouseClick(closeButton);
        tryCompare(win.runDetailsPopup, "visible", false);
        compare(win.detailsRunId, -1);
    }

    function test_detailsPopoverShowsTheFailureTextForAFailedRun() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        wait(300);

        // A deterministic, non-racy way to fail a real run: an output path
        // inside a directory that does not exist. writeOutput cannot open it
        // and finishRun(false, ...) settles the chip as "failed" with a real
        // message, the same shape a genuine write error gives - unlike
        // racing cancel() against however fast this machine happens to
        // finish a 1-second encode.
        EncoderController.loadSourceFile(fixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.setPendingCliLine("ac3cli encode failure-test.wav out.ac3 448 51");
        const badUrl = Qt.resolvedUrl("_test_output/does-not-exist/unreachable.ac3");
        EncoderController.encodeTo(badUrl);
        tryCompare(EncoderController, "busy", false, 10000);
        // See the equivalent wait in
        // test_clickingARunChipOpensItsDetailsPopoverWithTheSnapshottedCliLine
        // for why this needs to settle before a coordinate-based click.
        wait(100);

        const run = EncoderController.runs[0];
        compare(run.status, "failed");
        verify(run.detail.length > 0);
        compare(run.cliLine, "ac3cli encode failure-test.wav out.ac3 448 51");

        const chipSummary = findChild(win.contentItem, "runChipSummary-" + run.id);
        verify(chipSummary !== null);
        mouseClick(chipSummary);
        compare(win.detailsRunId, run.id);
        tryCompare(win.runDetailsPopup, "visible", true);
        compare(win.detailsRun.cliLine, run.cliLine);
        compare(win.detailsRun.detail, run.detail);

        const cliLineText = findChild(win.runDetailsPopup, "runDetailsCliLine");
        verify(cliLineText !== null);

        win.runDetailsPopup.close();
    }

    // --- item 32: run history persists across a restart -----------------------

    function test_restoreRunsAddsSavedEntriesAndDropsAnyStillEncoding() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        const before = EncoderController.runs.length;
        const saved = [
            { id: 999901, filename: "tst-restore-done.ec3", path: "", bitrateKbps: 448,
              durationText: "0:05", status: "done", sizeText: "12 KB", detail: "",
              framesText: "", rateText: "448 kbps",
              cliLine: "ac3cli encode saved.wav out.ec3 448 51",
              eac3: true, playDeviceIndex: -1 },
            { id: 999902, filename: "tst-restore-still-encoding.ec3", path: "",
              bitrateKbps: 448, durationText: "0:05", status: "encoding", sizeText: "",
              detail: "", framesText: "", rateText: "448 kbps", cliLine: "", eac3: true,
              playDeviceIndex: -1 },
        ];
        EncoderController.restoreRuns(saved);

        // The "encoding" entry belonged to a process that never finished it
        // and is dropped; the "done" one survives - appended after whatever
        // else is already here, which is exactly what a genuinely fresh
        // process' empty runs_ makes this in practice.
        compare(EncoderController.runs.length, before + 1);
        let found = null;
        for (const run of EncoderController.runs) {
            if (run.filename === "tst-restore-done.ec3") { found = run; break; }
        }
        verify(found !== null);
        compare(found.status, "done");
        compare(found.cliLine, "ac3cli encode saved.wav out.ec3 448 51");
        for (const run of EncoderController.runs) {
            verify(run.filename !== "tst-restore-still-encoding.ec3");
        }
    }

    function test_saveSessionPersistsFinishedRunsAsJsonExcludingAnyStillEncoding() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(fixtureUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        win.settings.restoreSession = true;
        win.saveSession();

        let parsed = null;
        try {
            parsed = JSON.parse(win.settings.sessionRuns);
        } catch (error) {
            parsed = null;
        }
        verify(Array.isArray(parsed));
        verify(parsed.length > 0);
        for (const run of parsed) {
            verify(run.status !== "encoding");
        }
        compare(parsed[0].filename, EncoderController.runs[0].filename);
        compare(parsed[0].id, EncoderController.runs[0].id);

        // appSettings has no organization/application name in this test
        // binary (see tst_tiers_and_flows.qml's own comment), but its
        // backing store is still shared across every Settings{} instance
        // THIS PROCESS creates - a saveSession() left with a real source
        // path in sessionSources would make the next window's own
        // Component.onCompleted -> restoreSession() silently reload it.
        // Reset both back to empty so a later test's fresh window starts
        // exactly as if this test had never run.
        win.settings.sessionSources = "[]";
        win.settings.sessionRuns = "[]";
    }
}
