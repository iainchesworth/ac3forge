import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// Preferences — a modal on the standard backdrop, squared corners, three
// columns: appearance, what happens when the app opens, and the defaults a
// new encode starts from. Values live in the Settings object Main.qml owns
// (`settings`); this dialog edits a working copy and writes it back on Save,
// so Cancel genuinely cancels.
Dialog {
    id: root

    // Main.qml's Settings instance and the hooks that apply a saved value to
    // the live session.
    property var settings
    signal applied()

    modal: true
    anchors.centerIn: parent
    width: Math.min(1040, parent ? parent.width - 80 : 1040)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    // The working copy.
    property string themeChoice: "system"
    property string controlsChoice: "guided"
    property string meterChoice: "coded"
    property bool cliVisible: true
    property int containerChoice: 0
    property bool vbrDefault: false
    property int bitrateChoice: 448
    property int vbrQualityChoice: 75
    property int drcChoice: 0
    property bool measureChoice: true

    onAboutToShow: {
        themeChoice = settings.theme;
        controlsChoice = settings.controlsOnOpen;
        meterChoice = settings.meterMode;
        cliVisible = settings.showCli;
        containerChoice = settings.defaultContainerIndex;
        vbrDefault = settings.defaultVbr;
        bitrateChoice = settings.defaultBitrateKbps;
        vbrQualityChoice = settings.defaultVbrQuality;
        drcChoice = settings.defaultDrcIndex;
        measureChoice = settings.defaultMeasureDialnorm;
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

                Text { text: qsTr("APPEARANCE"); font.pixelSize: 10; font.letterSpacing: 1.5; color: Theme.textMuted }

                Text { text: qsTr("Theme"); font.pixelSize: 12; color: Theme.text }
                SegmentedControl {
                    model: [
                        { value: "light", label: qsTr("Light") },
                        { value: "dark", label: qsTr("Dark") },
                        { value: "system", label: qsTr("System") },
                    ]
                    currentValue: root.themeChoice
                    onSelected: (value) => root.themeChoice = value
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Meter colours invert with the theme; the level thresholds do not move.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                }

                Item { Layout.preferredHeight: Theme.space2 }

                Text { text: qsTr("Meters — show"); font.pixelSize: 12; color: Theme.text }
                SegmentedControl {
                    model: [
                        { value: "coded", label: qsTr("Every coded channel") },
                        { value: "rendered", label: qsTr("Only driven speakers") },
                    ]
                    currentValue: root.meterChoice
                    onSelected: (value) => root.meterChoice = value
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("A stream can carry channels a receiver never drives — silent bed rows behind a dependent substream. Coded shows them; Rendered hides them.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                }
            }

            // ---- When ac3forge opens --------------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3

                Text { text: qsTr("WHEN AC3FORGE OPENS"); font.pixelSize: 10; font.letterSpacing: 1.5; color: Theme.textMuted }

                Text { text: qsTr("Controls"); font.pixelSize: 12; color: Theme.text }
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
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Guided asks one question per step and explains every constraint. The other two show the same state as panels; nothing is a separate mode.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                }

                Item { Layout.preferredHeight: Theme.space2 }

                Text { text: qsTr("COMMAND LINE"); font.pixelSize: 10; font.letterSpacing: 1.5; color: Theme.textMuted }
                CheckBox {
                    text: qsTr("Keep the ac3cli line visible")
                    checked: root.cliVisible
                    onToggled: root.cliVisible = checked
                    font.pixelSize: 12
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Every encode the window can produce is reachable from a command line; the bar at the foot shows the exact one.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                }
            }

            // ---- Defaults for a new encode ---------------------------------
            ColumnLayout {
                Layout.fillWidth: true
                Layout.preferredWidth: 1
                Layout.alignment: Qt.AlignTop
                spacing: Theme.space3

                Text { text: qsTr("DEFAULTS FOR A NEW ENCODE"); font.pixelSize: 10; font.letterSpacing: 1.5; color: Theme.textMuted }

                Text { text: qsTr("Container"); font.pixelSize: 12; color: Theme.text }
                ComboBox {
                    Layout.fillWidth: true
                    model: EncoderController.containerNames
                    currentIndex: root.containerChoice
                    onActivated: root.containerChoice = currentIndex
                }

                Text { text: qsTr("Rate mode"); font.pixelSize: 12; color: Theme.text }
                SegmentedControl {
                    model: [
                        { value: "cbr", label: qsTr("Constant") },
                        { value: "vbr", label: qsTr("Variable") },
                    ]
                    currentValue: root.vbrDefault ? "vbr" : "cbr"
                    onSelected: (value) => root.vbrDefault = value === "vbr"
                }

                Text { text: qsTr("Bit rate"); font.pixelSize: 12; color: Theme.text }
                ComboBox {
                    Layout.fillWidth: true
                    model: EncoderController.bitrates
                    displayText: qsTr("%1 kbps").arg(root.bitrateChoice)
                    delegate: ItemDelegate {
                        required property var modelData
                        width: parent ? parent.width : 0
                        text: qsTr("%1 kbps").arg(modelData)
                        onClicked: root.bitrateChoice = modelData
                    }
                }

                Text { text: qsTr("DRC profile"); font.pixelSize: 12; color: Theme.text }
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
                Text {
                    Layout.fillWidth: true
                    text: qsTr("The codec is not a default — it follows the channels you pick. No default channel layout either, for the same reason.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 11
                    color: Theme.textMuted
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("Cancel")
                onClicked: root.reject()
            }
            Button {
                objectName: "prefsSaveButton"
                text: qsTr("Save")
                highlighted: true
                onClicked: {
                    settings.theme = root.themeChoice;
                    settings.controlsOnOpen = root.controlsChoice;
                    settings.meterMode = root.meterChoice;
                    settings.showCli = root.cliVisible;
                    settings.defaultContainerIndex = root.containerChoice;
                    settings.defaultVbr = root.vbrDefault;
                    settings.defaultBitrateKbps = root.bitrateChoice;
                    settings.defaultVbrQuality = root.vbrQualityChoice;
                    settings.defaultDrcIndex = root.drcChoice;
                    settings.defaultMeasureDialnorm = root.measureChoice;
                    root.applied();
                    root.accept();
                }
            }
        }
    }
}
