import QtQuick
import QtTest

import Ac3Forge

// Cross-tier flows: the promises that hold BETWEEN Guided, Advanced and
// Expert rather than inside any one of them, plus the preference-driven
// behaviours (explanations, the codec-change warning, session restore, file
// naming, the Preferences dialog's own save/cancel contract) and the
// first-run screen's ways in. Settings has no backing store under the test
// harness (no organization name is set - deliberately, so tests can never
// read a developer's real preferences or write over them), so every value
// behaves as a plain in-memory property with its declared default.
TestCase {
    id: testCase
    name: "TiersAndFlows"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    readonly property url stereoUrl:
        Qt.resolvedUrl("../../../../fuzz/seeds/fuzz_wav_read/roundtrip-stereo.wav")

    function makeWindow() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);
        wait(300);
        return win;
    }

    // ---- first run -------------------------------------------------------
    function test_firstRunBundledTestSignalLoadsARealSource() {
        const win = makeWindow();
        // A fresh singleton state is not guaranteed (tests share it).
        // Dropping every source lets everHadSource's own binding read false
        // - assigning it here would break that binding and the final check.
        EncoderController.removeSource(0);
        tryVerify(() => !win.everHadSource);
        wait(100);

        const testButton = findChild(win.contentItem, "firstRun-test");
        verify(testButton !== null);
        mouseClick(testButton);
        // The button synthesises an actual 5.1 WAV and loads it like any
        // other file - a real session to explore, not a placeholder.
        tryCompare(EncoderController, "sourceReady", true);
        compare(EncoderController.sourceModel.length, 1);
        compare(EncoderController.channelShapeName, "5.1");
        verify(win.everHadSource);
    }

    function test_firstRunCaptureSwitchesToTheLiveBranch() {
        const win = makeWindow();
        EncoderController.removeSource(0);
        tryVerify(() => !win.everHadSource);
        wait(100);

        const liveButton = findChild(win.contentItem, "firstRun-live");
        verify(liveButton !== null);
        mouseClick(liveButton);
        compare(win.everHadSource, true);
        compare(win.inputMode, "live");
    }

    // ---- tier structure ---------------------------------------------------
    function test_leavingExpertOnAnExpertOnlyTabFallsBackToFormat() {
        const win = makeWindow();
        win.tier = "expert";
        win.currentTab = "coding";
        win.tier = "advanced";
        // Falling back to Format beats showing an empty panel - the
        // handoff's own rule for switching down a tier.
        compare(win.currentTab, "format");
    }

    function test_tabBadgesCountTheHiddenNonDefaults() {
        const win = makeWindow();
        win.tier = "expert";
        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 1;
        EncoderController.coupling = true;
        EncoderController.spx = true;
        EncoderController.aht = false;
        EncoderController.heavy = true;
        EncoderController.mixmeta = false;
        EncoderController.drcIndex = 0;

        const coding = win.visibleTabs.find((t) => t.key === "coding");
        const meta = win.visibleTabs.find((t) => t.key === "meta");
        verify(coding !== undefined);
        verify(meta !== undefined);
        compare(coding.badge, "2");
        compare(meta.badge, "1");

        EncoderController.atmosEnabled = true;
        const objects = win.visibleTabs.find((t) => t.key === "objects");
        compare(objects.badge, "on");

        // Leave shared state clean.
        EncoderController.atmosEnabled = false;
        EncoderController.coupling = false;
        EncoderController.spx = false;
        EncoderController.heavy = false;
        EncoderController.codecIndex = 0;
    }

    function test_goAssignFromGuidedRoundTripsLosslessly() {
        const win = makeWindow();
        win.everHadSource = true;
        win.tier = "guided";
        compare(win.fromGuided, false);

        win.goAssign();
        compare(win.tier, "advanced");
        compare(win.currentTab, "format");
        compare(win.fromGuided, true);

        // The return strip's own button, clicked for real.
        let backButton = null;
        tryVerify(() => {
            backButton = findChild(win.contentItem, "backToGuidedButton");
            return backButton !== null && backButton.visible;
        });
        backButton.clicked();
        compare(win.tier, "guided");
        compare(win.fromGuided, false);
    }

    // ---- preferences-driven behaviour --------------------------------------
    function test_fileNamePatternDrivesThePlannedName() {
        const win = makeWindow();
        EncoderController.atmosEnabled = false;
        EncoderController.codecIndex = 0;
        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);

        win.settings.namePattern = "{source}-take.{ext}";
        compare(win.plannedFileName(), "roundtrip-stereo-take.ac3");
        win.settings.namePattern = "{source}.{ext}";
        compare(win.plannedFileName(), "roundtrip-stereo.ac3");
    }

    function test_explanationsToggleHidesThePlainLanguageNotes() {
        const win = makeWindow();
        win.everHadSource = true;
        win.tier = "advanced";
        wait(100);

        const note = findChild(win.contentItem, "noteBedAlways");
        verify(note !== null);
        win.settings.showExplanations = true;
        tryVerify(() => note.visible);
        win.settings.showExplanations = false;
        tryVerify(() => !note.visible);
        win.settings.showExplanations = true;
    }

    function test_codecChangeWarningGatesThePromotion() {
        const win = makeWindow();
        win.everHadSource = true;
        win.tier = "advanced";
        EncoderController.atmosEnabled = false;
        EncoderController.applyChannelPreset("5.1");
        EncoderController.codecIndex = 0;
        win.settings.warnCodecChange = true;
        wait(100);

        // Emitting toggled() runs the same handler a real click does,
        // without needing the row scrolled into view (toggle() alone flips
        // the checked state but deliberately does NOT emit the interactive
        // signal, and the handler reads the model's state, not the visual).
        const rearBox = findChild(win.contentItem, "extra-rear");
        verify(rearBox !== null);
        rearBox.toggled();

        // The promotion waits behind the confirm: nothing has changed yet.
        compare(EncoderController.codecIndex, 0);
        compare(EncoderController.channelShapeName, "5.1");

        let continueButton = null;
        tryVerify(() => {
            continueButton = findChild(win.contentItem, "codecWarnContinue");
            return continueButton !== null && continueButton.visible;
        });
        continueButton.clicked();
        compare(EncoderController.codecIndex, 1);
        compare(EncoderController.channelShapeName, "7.1");

        // With the preference off the same action is immediate.
        win.settings.warnCodecChange = false;
        EncoderController.toggleExtra("rear");
        compare(EncoderController.channelShapeName, "5.1");
    }

    function test_preferencesDialogSavesOnSaveAndDiscardsOnCancel() {
        const win = makeWindow();
        win.settings.showCli = true;

        win.prefsDialog.open();
        let saveButton = null;
        tryVerify(() => {
            saveButton = findChild(win.contentItem, "prefsSaveButton");
            return saveButton !== null && saveButton.visible;
        });
        win.prefsDialog.cliVisible = false;
        saveButton.clicked();
        compare(win.settings.showCli, false);

        // The command bar genuinely follows the setting.
        const bar = findChild(win.contentItem, "commandBar");
        verify(bar !== null);
        tryVerify(() => !bar.visible);

        // Cancel is a real cancel: the working copy's edit goes nowhere.
        win.prefsDialog.open();
        let cancelButton = null;
        tryVerify(() => {
            cancelButton = findChild(win.contentItem, "prefsCancelButton");
            return cancelButton !== null && cancelButton.visible;
        });
        win.prefsDialog.cliVisible = true;
        cancelButton.clicked();
        compare(win.settings.showCli, false);

        win.settings.showCli = true;
    }

    function test_sessionSaveAndRestoreRoundTripsSourcesAndAssignments() {
        const win = makeWindow();
        EncoderController.atmosEnabled = false;
        EncoderController.loadSourceFile(stereoUrl);
        tryCompare(EncoderController, "sourceReady", true);
        EncoderController.applyChannelPreset("5.1");
        EncoderController.setAssignment(0, 0, "C");
        EncoderController.setAssignment(0, 1, "none");

        win.settings.restoreSession = true;
        win.saveSession();
        verify(win.settings.sessionSources.indexOf("roundtrip-stereo") >= 0);

        // Wipe the live state entirely, then restore from what was saved.
        EncoderController.removeSource(0);
        compare(EncoderController.sourceModel.length, 0);

        win.restoreSession();
        tryCompare(EncoderController, "sourceReady", true);
        compare(EncoderController.sourceModel.length, 1);
        compare(EncoderController.channelShapeName, "5.1");
        compare(EncoderController.assignmentRows[0].destToken, "C");
        compare(EncoderController.assignmentRows[1].destToken, "none");
        compare(EncoderController.assignmentRows[1].touched, true);
        compare(EncoderController.unassignedWarnings.length, 0);
    }
}
