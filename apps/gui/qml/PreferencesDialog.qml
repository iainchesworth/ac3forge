import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3Forge

// Preferences — a modal on the standard backdrop, squared corners, three
// columns: appearance, what happens when the app opens (plus files and
// runs), and the defaults a new encode starts from (plus capture and the
// command line). Values live in the Settings object Main.qml owns
// (`settings`); this dialog edits a working copy and writes it back on Save,
// so Cancel genuinely cancels. Every row here is wired to real behaviour —
// nothing is a checkbox for a feature that does not exist.
Dialog {
    id: root

    // Main.qml's Settings instance. Save fires the Dialog's own applied()
    // signal (declaring a fresh `signal applied()` here would collide with
    // that inherited signal - Qt flags it as an invalid override).
    property var settings

    modal: true
    anchors.centerIn: parent
    width: Math.min(1100, parent ? parent.width - 60 : 1100)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    // The working copy.
    property string themeChoice: "system"
    property string paletteChoice: "signal"
    property string controlsChoice: "guided"
    property string meterChoice: "coded"
    property bool explanationsChoice: true
    property bool warnCodecChoice: false
    property bool restoreSessionChoice: true
    property bool restoreScreenChoice: false
    property string outputFolderChoice: ""
    property string namePatternChoice: "{source}.{ext}"
    property bool keepPartialChoice: true
    property bool cliVisible: true
    property int containerChoice: 0
    property bool vbrDefault: false
    property int bitrateChoice: 448
    property int vbrQualityChoice: 75
    property int drcChoice: 0
    property bool measureChoice: true
    property bool autoMonitorChoice: true
    property bool askRecordNameChoice: false

    onAboutToShow: {
        themeChoice = settings.theme;
        paletteChoice = settings.palette;
        controlsChoice = settings.controlsOnOpen;
        meterChoice = settings.meterMode;
        explanationsChoice = settings.showExplanations;
        warnCodecChoice = settings.warnCodecChange;
        restoreSessionChoice = settings.restoreSession;
        restoreScreenChoice = settings.restoreScreen;
        outputFolderChoice = settings.outputFolder;
        namePatternChoice = settings.namePattern;
        keepPartialChoice = settings.keepPartial;
        cliVisible = settings.showCli;
        containerChoice = settings.defaultContainerIndex;
        vbrDefault = settings.defaultVbr;
        bitrateChoice = settings.defaultBitrateKbps;
        vbrQualityChoice = settings.defaultVbrQuality;
        drcChoice = settings.defaultDrcIndex;
        measureChoice = settings.defaultMeasureDialnorm;
        autoMonitorChoice = settings.autoMonitor;
        askRecordNameChoice = settings.askRecordName;
    }

    FolderDialog {
        id: outputFolderDialog
        title: qsTr("Choose the output folder")
        onAccepted: root.outputFolderChoice = selectedFolder.toString()
    }

    component PrefsKicker: Text {
        font.pixelSize: 10
        font.letterSpacing: 1.5
        color: Theme.textMuted
    }
    component PrefsLabel: Text {
        font.pixelSize: 12
        color: Theme.text
    }
    component PrefsNote: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.textMuted
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        Text {
            text: qsTr("Preferences")
            font.pixelSize: 20
            font.family: Theme.headingFamily
            font.weight: Font.ExtraBold
            color: Theme.text
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space8

            // ---- Appearance ------------------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3

                PrefsKicker { text: qsTr("APPEARANCE") }

                PrefsLabel { text: qsTr("Theme") }
                SegmentedControl {
                    model: [
                        { value: "light", label: qsTr("Light") },
                        { value: "dark", label: qsTr("Dark") },
                        { value: "system", label: qsTr("System") },
                    ]
                    currentValue: root.themeChoice
                    onSelected: (value) => root.themeChoice = value
                }
                PrefsNote {
                    text: qsTr("Light and dark are each hand-tuned per palette; the level thresholds never move.")
                }

                Item { Layout.preferredHeight: Theme.space2 }

                PrefsLabel { text: qsTr("Palette") }
                ComboBox {
                    objectName: "prefsPalette"
                    Layout.fillWidth: true
                    model: [
                        { value: "signal", label: qsTr("Signal — the design system's red") },
                        { value: "ink", label: qsTr("Ink — a cooler blue") },
                        { value: "console", label: qsTr("Console — studio amber") },
                        { value: "system", label: qsTr("System — the desktop's accent colour") },
                    ]
                    textRole: "label"
                    valueRole: "value"
                    currentIndex: {
                        const values = ["signal", "ink", "console", "system"];
                        return Math.max(0, values.indexOf(root.paletteChoice));
                    }
                    onActivated: root.paletteChoice = currentValue
                }
                PrefsNote {
                    text: qsTr("System follows the accent colour the desktop exposes — Windows, macOS and KDE provide one natively; elsewhere it falls back to the highlight colour. Changing it in the OS Settings lands here live.")
                }

                Item { Layout.preferredHeight: Theme.space2 }

                PrefsLabel { text: qsTr("Meters — show") }
                SegmentedControl {
                    model: [
                        { value: "coded", label: qsTr("Every coded channel") },
                        { value: "rendered", label: qsTr("Only speakers a receiver drives") },
                    ]
                    currentValue: root.meterChoice
                    onSelected: (value) => root.meterChoice = value
                }
                PrefsNote {
                    text: qsTr("A stream can carry channels no speaker plays — silent beds under objects, or positions a dependent substream replaces. Showing every coded channel tells you what is in the file; showing rendered speakers tells you what a listener hears.")
                }

                Item { Layout.preferredHeight: Theme.space2 }

                PrefsKicker { text: qsTr("EXPLANATIONS") }
                CheckBox {
                    text: qsTr("Show the plain-language notes beside controls")
                    checked: root.explanationsChoice
                    onToggled: root.explanationsChoice = checked
                    font.pixelSize: 12
                }
                CheckBox {
                    text: qsTr("Warn before a choice changes the codec")
                    checked: root.warnCodecChoice
                    onToggled: root.warnCodecChoice = checked
                    font.pixelSize: 12
                }
                PrefsNote {
                    text: qsTr("The codec always follows the channels either way — the warning only makes the moment it changes a deliberate one.")
                }
            }

            // ---- When ac3forge opens / files and runs ----------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3

                PrefsKicker { text: qsTr("WHEN AC3FORGE OPENS") }

                PrefsLabel { text: qsTr("Controls") }
                ComboBox {
                    Layout.fillWidth: true
                    model: [
                        { value: "guided", label: qsTr("Guided — one step at a time") },
                        { value: "advanced", label: qsTr("Advanced — all the format controls") },
                        { value: "expert", label: qsTr("Expert — coding tools and broadcast metadata") },
                        { value: "last", label: qsTr("Whatever I used last") },
                    ]
                    textRole: "label"
                    valueRole: "value"
                    currentIndex: {
                        const values = ["guided", "advanced", "expert", "last"];
                        return Math.max(0, values.indexOf(root.controlsChoice));
                    }
                    onActivated: root.controlsChoice = currentValue
                }
                PrefsNote {
                    text: qsTr("Guided asks a question per step and applies the constraints for you. Advanced shows the format, channels and objects at once. Expert adds the Annex E tools and the metadata panel.")
                }

                CheckBox {
                    text: qsTr("Reopen the last session's sources and assignments")
                    checked: root.restoreSessionChoice
                    onToggled: root.restoreSessionChoice = checked
                    font.pixelSize: 12
                }
                CheckBox {
                    text: qsTr("Start on the last screen I was on")
                    checked: root.restoreScreenChoice
                    onToggled: root.restoreScreenChoice = checked
                    font.pixelSize: 12
                }

                Item { Layout.preferredHeight: Theme.space2 }

                PrefsKicker { text: qsTr("FILES AND RUNS") }

                PrefsLabel { text: qsTr("Output folder") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space2

                    Text {
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        text: root.outputFolderChoice.length > 0
                              ? root.outputFolderChoice.replace("file:///", "")
                              : qsTr("Beside the first source")
                        elide: Text.ElideMiddle
                        font.pixelSize: 12
                        font.family: Theme.monoFamily
                        color: Theme.text
                    }
                    Button {
                        text: qsTr("Choose…")
                        onClicked: outputFolderDialog.open()
                    }
                    Button {
                        visible: root.outputFolderChoice.length > 0
                        text: qsTr("Reset")
                        flat: true
                        onClicked: root.outputFolderChoice = ""
                    }
                }

                PrefsLabel { text: qsTr("Name new files") }
                TextField {
                    objectName: "prefsNamePattern"
                    Layout.fillWidth: true
                    text: root.namePatternChoice
                    font.family: Theme.monoFamily
                    font.pixelSize: 12
                    onTextEdited: root.namePatternChoice = text
                }
                PrefsNote {
                    text: qsTr("{source} is the first source's own name, {ext} the suffix the plan derives — a pattern with neither would name every encode identically.")
                }

                CheckBox {
                    text: qsTr("Keep partial output when a run fails")
                    checked: root.keepPartialChoice
                    onToggled: root.keepPartialChoice = checked
                    font.pixelSize: 12
                }
                PrefsNote {
                    text: qsTr("A failed or cancelled run's frames are kept beside the intended output as <name>.partial.<ext> — named, never silently discarded.")
                }
            }

            // ---- Defaults / capture / command line -------------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3

                PrefsKicker { text: qsTr("DEFAULTS FOR A NEW ENCODE") }

                PrefsLabel { text: qsTr("Container") }
                ComboBox {
                    Layout.fillWidth: true
                    model: EncoderController.containerNames
                    currentIndex: root.containerChoice
                    onActivated: root.containerChoice = currentIndex
                }

                PrefsLabel { text: qsTr("Rate mode") }
                SegmentedControl {
                    model: [
                        { value: "cbr", label: qsTr("Constant") },
                        { value: "vbr", label: qsTr("Variable") },
                    ]
                    currentValue: root.vbrDefault ? "vbr" : "cbr"
                    onSelected: (value) => root.vbrDefault = value === "vbr"
                }

                PrefsLabel { text: qsTr("Bit rate") }
                ComboBox {
                    id: prefsBitrateBox
                    Layout.fillWidth: true
                    model: EncoderController.bitrates
                    displayText: qsTr("%1 kbps").arg(root.bitrateChoice)
                    delegate: ItemDelegate {
                        required property var modelData
                        width: prefsBitrateBox.width
                        text: qsTr("%1 kbps").arg(modelData)
                        onClicked: {
                            root.bitrateChoice = modelData;
                            prefsBitrateBox.popup.close();
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    PrefsLabel { text: qsTr("VBR quality") }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: root.vbrQualityChoice
                        font.pixelSize: 12
                        font.family: Theme.monoFamily
                        color: Theme.text
                    }
                }
                Slider {
                    objectName: "prefsVbrQuality"
                    Layout.fillWidth: true
                    from: 0
                    to: 100
                    stepSize: 1
                    value: root.vbrQualityChoice
                    onMoved: root.vbrQualityChoice = Math.round(value)
                }

                PrefsLabel { text: qsTr("DRC profile") }
                ComboBox {
                    Layout.fillWidth: true
                    model: EncoderController.drcNames
                    currentIndex: root.drcChoice
                    onActivated: root.drcChoice = currentIndex
                }

                CheckBox {
                    text: qsTr("Measure loudness and set dialnorm from it")
                    checked: root.measureChoice
                    onToggled: root.measureChoice = checked
                    font.pixelSize: 12
                }
                PrefsNote {
                    text: qsTr("The codec is not a default — it follows the channels you pick, and there is no default channel layout for the same reason. Variable rate applies to Dolby Digital Plus files only.")
                }

                Item { Layout.preferredHeight: Theme.space2 }

                PrefsKicker { text: qsTr("CAPTURE") }
                CheckBox {
                    text: qsTr("Start monitoring as soon as a device is chosen")
                    checked: root.autoMonitorChoice
                    onToggled: root.autoMonitorChoice = checked
                    font.pixelSize: 12
                }
                CheckBox {
                    text: qsTr("Ask for a filename before recording")
                    checked: root.askRecordNameChoice
                    onToggled: root.askRecordNameChoice = checked
                    font.pixelSize: 12
                }
                PrefsNote {
                    text: qsTr("Left unticked, Record writes straight to the output folder under a timestamped take name — the run strip and status line always say where.")
                }

                Item { Layout.preferredHeight: Theme.space2 }

                PrefsKicker { text: qsTr("COMMAND LINE") }
                CheckBox {
                    text: qsTr("Keep the ac3cli line visible")
                    checked: root.cliVisible
                    onToggled: root.cliVisible = checked
                    font.pixelSize: 12
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3

            Item { Layout.fillWidth: true }

            Button {
                objectName: "prefsCancelButton"
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            Button {
                objectName: "prefsSaveButton"
                text: qsTr("Save")
                highlighted: true
                onClicked: {
                    settings.theme = root.themeChoice;
                    settings.palette = root.paletteChoice;
                    settings.controlsOnOpen = root.controlsChoice;
                    settings.meterMode = root.meterChoice;
                    settings.showExplanations = root.explanationsChoice;
                    settings.warnCodecChange = root.warnCodecChoice;
                    settings.restoreSession = root.restoreSessionChoice;
                    settings.restoreScreen = root.restoreScreenChoice;
                    settings.outputFolder = root.outputFolderChoice;
                    settings.namePattern = root.namePatternChoice.length > 0
                                           ? root.namePatternChoice : "{source}.{ext}";
                    settings.keepPartial = root.keepPartialChoice;
                    settings.showCli = root.cliVisible;
                    settings.defaultContainerIndex = root.containerChoice;
                    settings.defaultVbr = root.vbrDefault;
                    settings.defaultBitrateKbps = root.bitrateChoice;
                    settings.defaultVbrQuality = root.vbrQualityChoice;
                    settings.defaultDrcIndex = root.drcChoice;
                    settings.defaultMeasureDialnorm = root.measureChoice;
                    settings.autoMonitor = root.autoMonitorChoice;
                    settings.askRecordName = root.askRecordNameChoice;
                    root.applied();
                    root.accept();
                }
            }
        }
    }
}
