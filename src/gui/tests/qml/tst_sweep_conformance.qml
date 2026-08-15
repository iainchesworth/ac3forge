import QtQuick
import QtTest

import Ac3Forge

// The mockup-conformance sweep's behavioural contracts: honest run history,
// CLI-line parity tokens, the E-AC-3 rate rung, the object-mode rate floor,
// keyframe retiming and gain seeding, and the live tab existing for a live
// source. Each of these was a real gap an audit found; the test pins the
// fix so it cannot silently regress.
TestCase {
    id: testCase
    name: "SweepConformance"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")
    readonly property url surroundUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-51.wav")

    // The runner shares ONE controller singleton across every tst_ file, and
    // the files' order is not the same on every platform - leaving sources
    // or an explicit assignment behind here broke tst_source_loading and
    // tst_guided_wizard on Linux while Windows happened to order this file
    // last. Every test leaves the controller as it found it.
    function cleanup() {
        EncoderController.atmosEnabled = false;
        if (EncoderController.sourceModel.length > 0) {
            EncoderController.removeSource(0);
        }
        EncoderController.containerIndex = 0;
        EncoderController.drcIndex = 0;
        EncoderController.codecIndex = 0;
        EncoderController.bitrateKbps = 192;
        EncoderController.applyChannelPreset("stereo");
    }

    function test_extrasRowsCarryChannelTokens() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        const extras = EncoderController.extrasModel;
        let wide = null;
        for (let i = 0; i < extras.length; i++) {
            if (extras[i].id === "wide") { wide = extras[i]; break; }
        }
        verify(wide !== null);
        // The row prints the Table E2.5 tokens themselves, not a count.
        compare(wide.tokens, "Lw Rw");
    }

    function test_dualMonoLockReasonIsNotPartOfDualMono() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        // Bed 0 is 1+1 (dual mono) in bedChoices' display order.
        compare(EncoderController.bedChoices[0].id, "1+1");
        EncoderController.bedIndex = 0;
        tryCompare(EncoderController, "dualMono", true);

        const extras = EncoderController.extrasModel;
        for (let i = 0; i < extras.length; i++) {
            compare(extras[i].enabled, false);
            compare(extras[i].reason, "not part of dual mono");
        }

        // Back to a plain bed for whatever runs next.
        EncoderController.applyChannelPreset("stereo");
    }

    function test_eac3Gains768AndAc3ClampsBack() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.applyChannelPreset("stereo");

        EncoderController.codecIndex = 1;
        verify(EncoderController.bitrates.indexOf(768) >= 0);
        EncoderController.bitrateKbps = 768;
        compare(EncoderController.bitrateKbps, 768);

        // AC-3's Table 5.18 tops out at 640 - switching back clamps rather
        // than leaving a rate the encoder would refuse at encode time.
        EncoderController.codecIndex = 0;
        compare(EncoderController.bitrateKbps, 640);
        verify(EncoderController.bitrates.indexOf(768) < 0);
    }

    function test_atmosEnableFloorsTheRate() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 1;
        EncoderController.bitrateKbps = 192;

        // The switch is never "a flag the table ignores": enabling object
        // mode raises the rate to what a bed + JOC + OAMD frame needs.
        EncoderController.atmosEnabled = true;
        compare(EncoderController.bitrateKbps, 384);
        compare(EncoderController.codecIndex, 1);

        EncoderController.atmosEnabled = false;
    }

    function test_cliLineCarriesSrcMapAndMetaTokens() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(surroundUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(stereoUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2);
        EncoderController.applyChannelPreset("5.1");
        EncoderController.autoAssignByName();

        win.inputMode = "file";
        // Extra sources ride as src=, the assignment as map= - the line is
        // reproducible, not a sketch of the primary alone.
        verify(win.cliLine.indexOf("src=") >= 0);
        verify(win.cliLine.indexOf("map=") >= 0);

        // A non-default metadata choice appears in ac3cli's own grammar.
        EncoderController.drcIndex = 1;
        verify(win.cliLine.indexOf("drc=") >= 0);
        EncoderController.drcIndex = 0;
        verify(win.cliLine.indexOf("drc=") < 0);
    }

    function test_matroskaIsHonestlyTwoCommands() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.inputMode = "file";

        EncoderController.containerIndex = 1;
        verify(win.cliLine.indexOf("&& ac3cli mkv") >= 0);
        EncoderController.containerIndex = 0;
        verify(win.cliLine.indexOf("&& ac3cli mkv") < 0);
    }

    function test_spdifIsHonestlyTwoCommandsToo() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.inputMode = "file";

        verify(EncoderController.containerNames.length === 6);
        compare(EncoderController.containerNames[2], "S/PDIF (.wav)");

        EncoderController.containerIndex = 2;
        verify(win.cliLine.indexOf("&& ac3cli spdif out.ac3 out.wav") >= 0);
        compare(EncoderController.outputSuffix(), "wav");
        // mkv and spdif are mutually exclusive container choices - never both
        // in the same command line.
        verify(win.cliLine.indexOf("&& ac3cli mkv") < 0);
        EncoderController.containerIndex = 0;
        verify(win.cliLine.indexOf("&& ac3cli spdif") < 0);
    }

    function test_mp4IsHonestlyTwoCommands() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.inputMode = "file";

        compare(EncoderController.containerNames[3], "MP4 (.mp4)");

        EncoderController.containerIndex = 3;
        verify(win.cliLine.indexOf("&& ac3cli mp4 out.ac3 out.mp4") >= 0);
        compare(EncoderController.outputSuffix(), "mp4");
        verify(!EncoderController.outputIsFolder());
        // mkv, spdif, fmp4 and ts are mutually exclusive container choices -
        // never more than one in the same command line.
        verify(win.cliLine.indexOf("&& ac3cli mkv") < 0);
        verify(win.cliLine.indexOf("&& ac3cli spdif") < 0);
        verify(win.cliLine.indexOf("&& ac3cli fmp4") < 0);
        verify(win.cliLine.indexOf("&& ac3cli ts") < 0);
        EncoderController.containerIndex = 0;
        verify(win.cliLine.indexOf("&& ac3cli mp4") < 0);
    }

    function test_fmp4IsHonestlyTwoCommandsAndNeedsAFolder() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.inputMode = "file";

        compare(EncoderController.containerNames[4], "Fragmented MP4/CMAF (folder)");

        EncoderController.containerIndex = 4;
        verify(win.cliLine.indexOf("&& ac3cli fmp4 out.ac3 out_dir") >= 0);
        // fMP4/CMAF writes a FOLDER of files, not one file with a single
        // extension - outputSuffix() is empty and outputIsFolder() is true,
        // the signal the save dialog and the Encode button both act on.
        compare(EncoderController.outputSuffix(), "");
        verify(EncoderController.outputIsFolder());
        verify(win.cliLine.indexOf("&& ac3cli mkv") < 0);
        verify(win.cliLine.indexOf("&& ac3cli spdif") < 0);
        verify(win.cliLine.indexOf("&& ac3cli mp4 ") < 0);
        verify(win.cliLine.indexOf("&& ac3cli ts") < 0);
        EncoderController.containerIndex = 0;
        verify(win.cliLine.indexOf("&& ac3cli fmp4") < 0);
        verify(!EncoderController.outputIsFolder());
    }

    function test_mpegTsIsHonestlyTwoCommands() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.inputMode = "file";

        compare(EncoderController.containerNames[5], "MPEG-TS (.ts)");

        EncoderController.containerIndex = 5;
        verify(win.cliLine.indexOf("&& ac3cli ts out.ac3 out.ts") >= 0);
        compare(EncoderController.outputSuffix(), "ts");
        verify(!EncoderController.outputIsFolder());
        verify(win.cliLine.indexOf("&& ac3cli mkv") < 0);
        verify(win.cliLine.indexOf("&& ac3cli spdif") < 0);
        verify(win.cliLine.indexOf("&& ac3cli mp4 ") < 0);
        verify(win.cliLine.indexOf("&& ac3cli fmp4") < 0);
        EncoderController.containerIndex = 0;
        verify(win.cliLine.indexOf("&& ac3cli ts") < 0);
    }

    function test_bitrateFloorAdvisoryTracksCodedChannelsAndFloor() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;
        // The whole workbench (Format tab included) stays behind the
        // first-run screen until a source has ever been chosen - without
        // this, everything below reads .visible false regardless of tier.
        win.everHadSource = true;
        // The advisory lives on the Format tab, Advanced/Expert only - a
        // fresh window defaults to Guided, where the tab content is not the
        // current page and reads .visible false regardless of its own
        // binding (Qt Quick Test's effective-visibility rule for anything
        // off the current StackLayout page).
        win.tier = "advanced";
        // Established explicitly rather than assumed: currentTab persists
        // via QSettings, which is process-wide across this whole test
        // binary run - an earlier test (or a stale settings file) could
        // leave it on something other than "format".
        win.currentTab = "format";
        wait(50);
        EncoderController.codecIndex = 1;  // E-AC-3 - 7.1's extras need it
        EncoderController.applyChannelPreset("7.1");
        // 7 full-bandwidth channels (LFE excluded) * 77 kbps/channel = 539,
        // comfortably above 192 - the advisory must show.
        EncoderController.bitrateKbps = 192;
        compare(EncoderController.fullBandwidthCodedChannelCount, 7);
        verify(EncoderController.fullBandwidthCodedChannelCount
               * EncoderController.kbpsPerChannelFloor > EncoderController.bitrateKbps);

        let advisory = null;
        tryVerify(() => {
            advisory = findChild(win.contentItem, "bitrateFloorAdvisory");
            return advisory !== null && advisory.visible;
        });

        // A rate comfortably above the floor for the same layout hides it
        // again - this is a hint, not a hard-refusing gate, so it has to
        // track the bitrate live rather than latch on.
        EncoderController.bitrateKbps = 768;
        compare(advisory.visible, false);
    }

    readonly property url refusalUrl:
        Qt.resolvedUrl("_test_output/tst_sweep_refused.ec3")

    function test_preRunRefusalRaisesTheBanner() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.addSourceFile(surroundUrl);
        tryVerify(() => EncoderController.sourceModel.length === 2);

        // Two sources, nothing assigned: encodeTo refuses before a run entry
        // ever opens - and that refusal must land in the banner, not only in
        // a status line the run strip may have scrolled away.
        compare(win.refusalText, "");
        EncoderController.encodeTo(refusalUrl);
        tryVerify(() => win.refusalText.length > 0);
        compare(EncoderController.busy, false);
    }

    function test_moveObjectKeyframeRetimesTheCue() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(surroundUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount > 0);

        EncoderController.clearObjectPath(0);
        EncoderController.addObjectKeyframe(0, 1.0);
        compare(EncoderController.objectKeyframes(0).length, 1);
        compare(EncoderController.objectKeyframes(0)[0].time, 1.0);

        EncoderController.moveObjectKeyframe(0, 1.0, 3.0);
        compare(EncoderController.objectKeyframes(0).length, 1);
        compare(EncoderController.objectKeyframes(0)[0].time, 3.0);

        // Landing a drag on another cue replaces it - one instant, one cue.
        EncoderController.addObjectKeyframe(0, 5.0);
        EncoderController.moveObjectKeyframe(0, 3.0, 5.0);
        compare(EncoderController.objectKeyframes(0).length, 1);
        compare(EncoderController.objectKeyframes(0)[0].time, 5.0);

        EncoderController.clearObjectPath(0);
        EncoderController.atmosEnabled = false;
    }

    function test_handAddedKeySeedsTheInverseRootGain() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        EncoderController.loadSourceFile(surroundUrl);   // 6 channels
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.atmosEnabled = true;
        tryVerify(() => EncoderController.objectCount === 6);

        EncoderController.clearObjectPath(0);
        EncoderController.addObjectKeyframe(0, 0.5);
        const keys = EncoderController.objectKeyframes(0);
        compare(keys.length, 1);
        // The same 0.7/sqrt(n) law the path-less fallback encodes at - unity
        // here made an object jump ~3-9 dB the moment its first cue landed.
        fuzzyCompare(keys[0].gain, 0.7 / Math.sqrt(6), 0.0001);

        EncoderController.clearObjectPath(0);
        EncoderController.atmosEnabled = false;
    }

    function test_liveSessionTabExistsForALiveSource() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        win.everHadSource = true;
        win.tier = "advanced";
        win.inputMode = "live";
        // The tab is where a session is UNDERSTOOD - it exists whenever the
        // live source is selected, running session or not.
        tryVerify(() => win.visibleTabs.some(tab => tab.key === "session"));

        win.inputMode = "file";
        tryVerify(() => !win.visibleTabs.some(tab => tab.key === "session"));
    }

    function test_guidedFooterStaysOnScreen() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.tier = "guided";
        wait(300);   // layout polish, same rule the wizard tests document

        // The Back/Next footer is PINNED: whatever the step content's
        // height, the way forward is on screen - it used to sit at the
        // bottom of the whole page's scroll range, a screenful of blank
        // space below short content (the StackLayout inherited the tallest
        // tab's height).
        const nextButton = findChild(win.contentItem, "wizardNextButton");
        verify(nextButton !== null);
        tryVerify(() => {
            const bottom = nextButton.mapToItem(null, 0, nextButton.height).y;
            return nextButton.visible && bottom > 0 && bottom <= win.height;
        });

        // And the step content scrolls on its own, between the pinned bars.
        verify(findChild(win.contentItem, "wizardScroll") !== null);
    }

    function test_tabPagesFollowTheCurrentPagesHeight() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.tier = "expert";
        win.currentTab = "meta";
        wait(300);

        // The scroll range follows the CURRENT page, not the union of every
        // page - the Metadata tab must not inherit the Format tab's much
        // larger height as trailing blank space.
        const pages = findChild(win.contentItem, "tabPages");
        const scroll = findChild(win.contentItem, "tabScroll");
        verify(pages !== null);
        verify(scroll !== null);
        tryVerify(() => {
            const current = pages.children[pages.currentIndex];
            return current !== undefined
                   && Math.abs(scroll.contentHeight - current.implicitHeight) < 1;
        });

        win.currentTab = "format";
        tryVerify(() => {
            const current = pages.children[pages.currentIndex];
            return current !== undefined
                   && Math.abs(scroll.contentHeight - current.implicitHeight) < 1;
        });
    }

    function test_cliChipOpensThePopoverWithTheLiveLine() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        EncoderController.atmosEnabled = false;

        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        win.tier = "advanced";
        wait(300);

        // The chip still follows the showCli preference (the contract
        // tst_tiers_and_flows pins), and clicking it opens the popover
        // carrying the LIVE line - the same window.cliLine property the
        // command-line parity tests already exercise.
        const chip = findChild(win.contentItem, "commandBar");
        verify(chip !== null);
        verify(chip.visible);

        const popup = findChild(win, "cliPopup");
        verify(popup !== null);
        compare(popup.opened, false);

        mouseClick(chip);
        tryVerify(() => popup.opened);

        const line = findChild(popup.contentItem, "cliPopupLine");
        verify(line !== null);
        compare(line.text, win.cliLine);
        verify(findChild(popup.contentItem, "cliPopupCopy") !== null);

        popup.close();
        tryVerify(() => !popup.opened);
    }
}
