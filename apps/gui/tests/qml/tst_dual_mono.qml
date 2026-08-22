import QtQuick
import QtTest

import Ac3Forge

// Selecting 1+1 as the bed - the "dual mono is a bed, not a layout" surface.
// bedChoices()[0] is always the dual-mono entry (see kBeds' own comment).
TestCase {
    id: testCase
    name: "DualMono"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    // Ch1 silent, Ch2 a real 300 Hz tone - built for the independent-
    // measurement tests below, where a blended pass across both dual-mono
    // channels (the bug) and a per-channel one (the fix) give different,
    // observable pass/fail outcomes rather than just different numbers.
    readonly property url ch2OnlyUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/dual-mono-ch2-only.wav")
    readonly property url outputUrl:
        Qt.resolvedUrl("_test_output/tst_dual_mono.ac3")

    function test_selectingDualMonoClearsLfeAndLocksExtras() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        compare(EncoderController.bedChoices[0].id, "1+1");

        // EncoderController is a singleton shared across every test function
        // in the whole suite (see e.g. tst_run_history.qml's own note on
        // this), so this establishes its own known starting point - 3/2 with
        // LFE on - rather than assume nothing earlier left the bed on 1+1
        // already. Qt Quick Test does not run functions in declaration
        // order (alphabetical within a file, confirmed the hard way: this
        // test failed its very next line when it assumed dualMono started
        // false, because test_dualMonoHasNoSoundstage - alphabetically
        // first - already ran and left it selected).
        EncoderController.bedIndex = 6;  // 3/2, per kBeds' own order
        EncoderController.bedLfe = true;
        compare(EncoderController.dualMono, false);
        verify(EncoderController.bedLfe);

        EncoderController.bedIndex = 0;  // 1+1

        compare(EncoderController.dualMono, true);
        compare(EncoderController.channelShapeName, "1+1");
        compare(EncoderController.bedLfe, false);
        compare(EncoderController.bedLfeLocked, true);
        compare(EncoderController.extrasLocked, true);
    }

    function test_dualMonoHasNoSoundstage() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        // A plain stereo source surrounds - the baseline this test would be
        // meaningless without.
        verify(EncoderController.surround);

        EncoderController.bedIndex = 0;  // 1+1
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        // The meters now reflect what was actually encoded (Ch1/Ch2), not
        // the source's own natural stereo layout - the soundfield's gate is
        // read off that, same as everywhere else in this file.
        compare(EncoderController.surround, false);
        compare(EncoderController.runs[0].status, "done");
    }

    // --- dialnorm=auto for dual mono ----------------------------------------

    function test_dialnormAutoNoLongerRefusedForDualMono() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.bedIndex = 0;  // 1+1
        compare(EncoderController.dualMono, true);

        EncoderController.measureDialnorm = true;
        EncoderController.measureDialnorm2 = true;
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        // stereoUrl's own L/R both carry real audio, so both programmes'
        // measurements pass their absolute gate and the encode completes -
        // encodeChannels no longer hard-refuses measure_dialnorm(2) just
        // because the bed is dual mono.
        compare(EncoderController.runs[0].status, "done");

        EncoderController.measureDialnorm = false;
        EncoderController.measureDialnorm2 = false;
        EncoderController.applyChannelPreset("stereo");
    }

    function test_dialnormAutoMeasuresEachDualMonoProgrammeOnItsOwnChannel() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        // ch2OnlyUrl's Ch1 is pure silence and Ch2 a real tone. A blended
        // pass across both dual-mono channels at once - measuring Ch1 with
        // Ch2's content mixed in, the bug this feature replaces - would let
        // Ch2's real signal carry Ch1 past the -70 LKFS absolute gate too.
        // Measuring Ch1 on its own coded channel instead correctly finds
        // nothing there and refuses, which is the observable difference
        // this test checks for.
        EncoderController.loadSourceFile(ch2OnlyUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.bedIndex = 0;  // 1+1
        compare(EncoderController.dualMono, true);

        EncoderController.measureDialnorm = true;
        EncoderController.measureDialnorm2 = false;  // Ch2 stays manual - irrelevant here
        EncoderController.encodeTo(outputUrl);
        tryCompare(EncoderController, "busy", false, 10000);

        compare(EncoderController.runs[0].status, "failed");
        verify(EncoderController.runs[0].detail.indexOf("Program 1") >= 0);

        EncoderController.measureDialnorm = false;
        EncoderController.applyChannelPreset("stereo");
    }

    // --- Ch2's own DRC (bundle D, item 23) ----------------------------------

    function test_drc2IsIndependentOfProgramme1sDrc() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.bedIndex = 0;  // 1+1
        compare(EncoderController.dualMono, true);

        EncoderController.drcIndex = 1;   // film-standard
        EncoderController.drc2Index = 5;  // speech - deliberately different
        compare(EncoderController.drcIndex, 1);
        compare(EncoderController.drc2Index, 5);

        // Changing one must never drag the other along - the whole point of
        // item 23's fix (see plan::Metadata::drc2's own comment: dialnorm2
        // already established that a dual-mono field has no fallback to its
        // programme-1 sibling).
        EncoderController.drcIndex = 2;
        compare(EncoderController.drc2Index, 5);

        EncoderController.drcIndex = 0;
        EncoderController.drc2Index = 0;
        EncoderController.applyChannelPreset("stereo");
    }

    function test_metaTokensEmitDrc2AndHeavy2OnlyUnderDualMono() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.bedIndex = 0;  // 1+1
        compare(EncoderController.dualMono, true);

        EncoderController.drc2Index = 5;  // speech
        EncoderController.heavy2 = true;
        verify(EncoderController.metaTokens.indexOf("drc2=speech") >= 0);
        verify(EncoderController.metaTokens.indexOf("heavy2") >= 0);

        // Leaving dual mono must stop advertising Ch2 tokens for a bed that
        // no longer has a Ch2 - metaTokens gates this on isDualMono(), the
        // same way the GUI's own Programme 2 controls disappear. drc2Index/
        // heavy2 are deliberately left non-default across the bed switch:
        // if they were cleared first, the tokens' absence would prove
        // nothing about the isDualMono() gate specifically.
        EncoderController.applyChannelPreset("stereo");
        compare(EncoderController.dualMono, false);
        verify(EncoderController.metaTokens.indexOf("drc2=") < 0);
        verify(EncoderController.metaTokens.indexOf("heavy2") < 0);

        EncoderController.heavy2 = false;
        EncoderController.drc2Index = 0;
    }
}
