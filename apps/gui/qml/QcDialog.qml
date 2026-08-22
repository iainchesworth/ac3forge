import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3Forge

// "QC a stream" — roadmap C3. Opening an already-encoded .ac3/.ec3 and
// checking it against its own embedded metadata is a fundamentally
// different shape to every other surface in this window: open → decode →
// measure → report, with no source, no plan and no encoder anywhere in the
// path. It lives here, as its own modal reachable from the header, rather
// than as a tab beside Format/Coding tools/Metadata — see docs/gui/qc.md
// for the full reasoning. Modelled directly on PreferencesDialog's own
// shape (a Dialog on the standard backdrop, driving a singleton controller
// exactly the way every other panel drives EncoderController), so this
// needed no new visual language, just QcController in place of
// EncoderController.
Dialog {
    id: root
    objectName: "qcDialog"

    modal: true
    anchors.centerIn: parent
    width: Math.min(820, parent ? parent.width - 60 : 820)
    height: Math.min(720, parent ? parent.height - 60 : 720)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    FileDialog {
        id: qcFileDialog
        title: qsTr("Choose an AC-3 / E-AC-3 stream")
        nameFilters: [qsTr("AC-3 / E-AC-3 (*.ac3 *.ec3)"), qsTr("All files (*)")]
        onAccepted: QcController.measureFile(selectedFile)
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("QC a stream")
                font.pixelSize: 18
                font.weight: Font.ExtraBold
                font.family: Theme.headingFamily
                color: Theme.text
            }
            Button {
                objectName: "qcCloseButton"
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Decodes an already-encoded file and measures it the same way “ac3cli qc” does — the stream's own claims, checked against what is actually in it, not the source that made it.")
            font.pixelSize: 12
            color: Theme.neutral700
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3

            Button {
                objectName: "qcChooseFileButton"
                text: qsTr("Choose file…")
                enabled: !QcController.busy
                onClicked: qcFileDialog.open()
            }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: QcController.filePath.length > 0 ? QcController.filePath : qsTr("No file chosen yet")
                color: Theme.neutral700
                font.pixelSize: 12
                font.family: Theme.monoFamily
            }
            BusyIndicator {
                objectName: "qcBusyIndicator"
                visible: QcController.busy
                running: QcController.busy
                implicitWidth: 24
                implicitHeight: 24
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3
            Text {
                text: qsTr("DELIVERY PRESET")
                font.pixelSize: 10
                font.letterSpacing: 1.2
                color: Theme.textMuted
            }
            // Mirrors ac3cli qc's own preset=<name>|all: "All" reports every
            // gate's pass/fail with no single band to draw on the meters
            // below; picking one preset both narrows the verdict list to it
            // AND feeds its target/tolerance/ceiling into the meters as the
            // band/ceiling lines - see QcController::programmes()'s own
            // comment on why presets has exactly one entry once this is
            // anything but "All".
            SegmentedControl {
                objectName: "qcPresetControl"
                model: [
                    { value: "0", label: qsTr("All") },
                    { value: "1", label: qsTr("EBU R 128 s2") },
                    { value: "2", label: qsTr("ATSC A/85") },
                    { value: "3", label: qsTr("Netflix") },
                ]
                currentValue: String(QcController.presetIndex)
                onSelected: (value) => QcController.presetIndex = parseInt(value)
            }
        }

        Text {
            objectName: "qcErrorText"
            Layout.fillWidth: true
            visible: QcController.error.length > 0
            wrapMode: Text.WordWrap
            text: QcController.error
            color: Theme.bad
            font.pixelSize: 12
        }

        Text {
            visible: !QcController.hasResult && QcController.error.length === 0 && !QcController.busy
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: qsTr("Choose a file above to measure it against these gates.")
            color: Theme.textMuted
            font.pixelSize: 12
            verticalAlignment: Text.AlignTop
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: QcController.hasResult
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: parent ? parent.width : 0
                spacing: Theme.space4

                Text {
                    objectName: "qcSummaryText"
                    Layout.fillWidth: true
                    text: QcController.summaryLine
                    font.pixelSize: 12
                    font.family: Theme.monoFamily
                    font.weight: Font.DemiBold
                    color: Theme.text
                }

                Repeater {
                    objectName: "qcProgrammes"
                    model: QcController.programmes

                    delegate: Card {
                        id: programmeCard
                        required property var modelData
                        Layout.fillWidth: true
                        title: programmeCard.modelData.label.length > 0
                               ? qsTr("Programme %1").arg(programmeCard.modelData.label)
                               : qsTr("Programme")

                        readonly property var soloPreset: programmeCard.modelData.presets.length === 1
                                                           ? programmeCard.modelData.presets[0] : null

                        QcGateMeter {
                            objectName: "qcLoudnessMeter"
                            Layout.fillWidth: true
                            label: qsTr("Integrated loudness")
                            unit: "LKFS"
                            minValue: -40
                            maxValue: 0
                            hasValue: programmeCard.modelData.hasLoudness
                            value: programmeCard.modelData.integratedLkfs
                            bandLow: programmeCard.soloPreset
                                     ? programmeCard.soloPreset.targetLkfs - programmeCard.soloPreset.toleranceLu
                                     : NaN
                            bandHigh: programmeCard.soloPreset
                                      ? programmeCard.soloPreset.targetLkfs + programmeCard.soloPreset.toleranceLu
                                      : NaN
                            pass: programmeCard.soloPreset ? programmeCard.soloPreset.loudnessPass : true
                        }
                        QcGateMeter {
                            objectName: "qcLraMeter"
                            Layout.fillWidth: true
                            label: qsTr("Loudness range")
                            unit: "LU"
                            minValue: 0
                            maxValue: 20
                            hasValue: programmeCard.modelData.hasLra
                            value: programmeCard.modelData.lra
                        }
                        QcGateMeter {
                            objectName: "qcTruePeakMeter"
                            Layout.fillWidth: true
                            label: qsTr("True peak")
                            unit: "dBTP"
                            minValue: -40
                            maxValue: 3
                            hasValue: programmeCard.modelData.hasTruePeak
                            value: programmeCard.modelData.truePeakDbtp
                            ceilingValue: programmeCard.soloPreset ? programmeCard.soloPreset.maxTruePeakDbtp : NaN
                            pass: programmeCard.soloPreset ? programmeCard.soloPreset.truePeakPass : true
                        }

                        ColumnLayout {
                            objectName: "qcDialnormBlock"
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.space2
                            spacing: 2

                            Text {
                                objectName: "qcDialnormLine"
                                text: qsTr("dialnorm %1  (claims dialogue at %2 LKFS)")
                                          .arg(programmeCard.modelData.dialnorm)
                                          .arg(programmeCard.modelData.claimedLkfs.toFixed(2))
                                font.pixelSize: 11
                                font.family: Theme.monoFamily
                                color: Theme.text
                            }
                            Text {
                                objectName: "qcDeltaLine"
                                visible: programmeCard.modelData.hasLoudness
                                text: qsTr("delta %1 dB  (measured − claimed; derived dialnorm would be %2%3)")
                                          .arg(programmeCard.modelData.deltaDb.toFixed(2))
                                          .arg(programmeCard.modelData.impliedDialnorm)
                                          .arg(programmeCard.modelData.dialnormMatches ? qsTr(" — matches") : qsTr(""))
                                font.pixelSize: 11
                                font.family: Theme.monoFamily
                                color: Theme.text
                            }
                            Text {
                                text: programmeCard.modelData.hasCompr
                                      ? qsTr("compr present, %1 dB").arg(programmeCard.modelData.comprDb.toFixed(2))
                                      : qsTr("compr absent")
                                font.pixelSize: 11
                                font.family: Theme.monoFamily
                                color: Theme.neutral700
                            }
                        }

                        ColumnLayout {
                            objectName: "qcPresetRows"
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.space2
                            spacing: 4

                            Repeater {
                                model: programmeCard.modelData.presets

                                delegate: RowLayout {
                                    id: presetRow
                                    required property var modelData
                                    objectName: "qcPresetRow-" + presetRow.modelData.id
                                    Layout.fillWidth: true
                                    spacing: Theme.space3

                                    Text {
                                        Layout.preferredWidth: 140
                                        text: presetRow.modelData.name
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        color: Theme.text
                                    }
                                    Text {
                                        Layout.preferredWidth: 110
                                        text: presetRow.modelData.loudnessPass ? qsTr("loudness PASS") : qsTr("loudness FAIL")
                                        color: presetRow.modelData.loudnessPass ? Theme.good : Theme.bad
                                        font.pixelSize: 11
                                        font.family: Theme.monoFamily
                                    }
                                    Text {
                                        Layout.preferredWidth: 120
                                        text: presetRow.modelData.truePeakPass ? qsTr("true peak PASS") : qsTr("true peak FAIL")
                                        color: presetRow.modelData.truePeakPass ? Theme.good : Theme.bad
                                        font.pixelSize: 11
                                        font.family: Theme.monoFamily
                                    }
                                    Rectangle {
                                        objectName: "qcVerdictChip-" + presetRow.modelData.id
                                        Layout.preferredWidth: 56
                                        Layout.preferredHeight: 18
                                        color: presetRow.modelData.pass ? Theme.good : Theme.bad
                                        Text {
                                            anchors.centerIn: parent
                                            text: presetRow.modelData.pass ? qsTr("PASS") : qsTr("FAIL")
                                            color: Theme.bg
                                            font.pixelSize: 10
                                            font.weight: Font.Bold
                                        }
                                    }
                                    Item { Layout.fillWidth: true }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
