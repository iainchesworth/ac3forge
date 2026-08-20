import QtQuick
import QtTest

import Ac3Forge

// Roadmap C3's own QC surface: QcController (a decode-and-measure pass over
// an ALREADY-ENCODED file, deliberately separate from EncoderController - see
// qc_controller.hpp's own header comment) plus QcDialog.qml/QcGateMeter.qml,
// the report view it drives. QcController is a singleton, like
// EncoderController - shared across every test function in this whole suite
// (see e.g. tst_run_history.qml's own note on this), so each test here
// starts by putting it into a known state rather than assuming what an
// earlier test left behind.
TestCase {
    id: testCase
    name: "QcPanel"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // A real, already-encoded AC-3 stream - one of the fuzz corpus's own
    // seed files (not silence, not a single frame; see CONTRIBUTING.md's
    // "test with real audio, from frame 1 onward" rule, which this fixture
    // already satisfies since it backs the roundtrip fuzzer). Using it
    // directly, rather than encoding one first, exercises exactly the real
    // workflow this dialog exists for: opening a file that already exists.
    readonly property url realStreamUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.ac3")

    function init() {
        QcController.presetIndex = 0;
    }

    // Proves two things at once: the measurement genuinely runs off the GUI
    // thread (busy flips true synchronously, then back to false once the
    // worker's queued completion lands - the same tryCompare pattern
    // tst_vbr.qml already uses to prove an encode is asynchronous), and the
    // report it produces is REAL data, not a placeholder - a stream this
    // short still clears the -70 LKFS absolute gate, so hasLoudness/
    // hasTruePeak read true rather than the "nothing measured" defaults.
    function test_measuringRealFileIsAsyncAndReportsRealData() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        QcController.measureFile(realStreamUrl);
        // Set synchronously by measureFile() itself, before the worker
        // thread is even scheduled - still true here proves the measurement
        // has NOT completed inline on this thread.
        compare(QcController.busy, true);
        tryCompare(QcController, "busy", false, 15000);

        compare(QcController.error, "");
        compare(QcController.hasResult, true);
        compare(QcController.filePath.length > 0, true);
        verify(QcController.summaryLine.indexOf("AC-3") === 0);
        verify(QcController.summaryLine.indexOf("Hz") > 0);

        const programmes = QcController.programmes;
        compare(programmes.length, 1);  // a plain stereo stream, not 1+1
        const p = programmes[0];
        compare(p.hasLoudness, true);
        compare(p.hasTruePeak, true);
        verify(p.dialnorm >= 1 && p.dialnorm <= 31);
        // integratedLkfs/truePeakDbtp are real measured numbers once
        // hasLoudness/hasTruePeak are true - a placeholder report would
        // leave them at their C++-side default of exactly 0.0, which no
        // genuine BS.1770 measurement of real audio lands on.
        verify(p.integratedLkfs !== 0.0);
        verify(p.truePeakDbtp !== 0.0);
    }

    // presetIndex 0 ("All presets") reports all three named delivery gates;
    // picking one narrows the list to it. The target/tolerance/ceiling
    // numbers are fixed constants from ac3::meta::qc_preset() (qc.hpp), so
    // asserting their exact values is a check against known, non-random
    // data - proof the QML is reading the real C++ table, not inventing
    // display numbers of its own.
    function test_presetSelectionNarrowsToTheChosenPresetsRealNumbers() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        QcController.measureFile(realStreamUrl);
        tryCompare(QcController, "busy", false, 15000);
        compare(QcController.hasResult, true);

        let presets = QcController.programmes[0].presets;
        compare(presets.length, 3);
        compare(presets[0].id, "ebu-r128-s2");
        compare(presets[0].targetLkfs, -23.0);
        compare(presets[0].toleranceLu, 1.0);
        compare(presets[1].id, "atsc-a85");
        compare(presets[1].targetLkfs, -24.0);
        compare(presets[2].id, "netflix");
        compare(presets[2].targetLkfs, -27.0);
        compare(presets[2].maxTruePeakDbtp, -2.0);

        QcController.presetIndex = 2;  // ATSC A/85
        presets = QcController.programmes[0].presets;
        compare(presets.length, 1);
        compare(presets[0].id, "atsc-a85");
        compare(presets[0].targetLkfs, -24.0);
        compare(presets[0].toleranceLu, 2.0);

        QcController.presetIndex = 0;
    }

    // The report view itself renders that same real data - the dialog's
    // summary text matches the controller, and one Card exists per measured
    // programme (one, for this plain-stereo fixture).
    function test_dialogRendersTheRealMeasurement() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        QcController.measureFile(realStreamUrl);
        tryCompare(QcController, "busy", false, 15000);
        win.qcDialogRef.open();
        tryVerify(() => win.qcDialogRef.opened);

        let summary = null;
        tryVerify(() => {
            summary = findChild(win.contentItem, "qcSummaryText");
            return summary !== null && summary.visible;
        });
        compare(summary.text, QcController.summaryLine);

        const programmes = findChild(win.contentItem, "qcProgrammes");
        verify(programmes !== null);
        compare(programmes.count, 1);

        win.qcDialogRef.close();
    }

    // The pass/fail two-state colouring itself, checked directly against
    // QcGateMeter rather than against a real file's own loudness (which
    // preset it happens to pass depends on content this test does not
    // control) - the same deterministic-component-property approach
    // ChannelMeter's own CLIP colouring would be tested with. Mirrors the
    // "good vs bad" tokens Theme.qml already defines and ChannelMeter's CLIP
    // box already uses, so a pass and a fail are never the same colour.
    Component {
        id: gateMeterComponent
        QcGateMeter {}
    }

    function test_gateMeterPassAndFailAreVisuallyDistinguishable() {
        const passMeter = createTemporaryObject(gateMeterComponent, testCase, {
            hasValue: true, value: -23.0, minValue: -40, maxValue: 0,
            bandLow: -24.0, bandHigh: -22.0, pass: true
        });
        verify(passMeter !== null);
        const failMeter = createTemporaryObject(gateMeterComponent, testCase, {
            hasValue: true, value: -10.0, minValue: -40, maxValue: 0,
            bandLow: -24.0, bandHigh: -22.0, pass: false
        });
        verify(failMeter !== null);

        const passFill = findChild(passMeter, "qcGateFill");
        const failFill = findChild(failMeter, "qcGateFill");
        verify(passFill !== null);
        verify(failFill !== null);
        compare(String(passFill.color), String(Theme.good));
        compare(String(failFill.color), String(Theme.bad));
        verify(String(passFill.color) !== String(failFill.color));
    }
}
