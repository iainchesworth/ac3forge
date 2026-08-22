import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3Forge

// "Inspect objects" — the decode-side counterpart to the Objects tab's
// authoring room view (Main.qml). Eac3Decoder now genuinely parses OAMD
// positions and reconstructs JOC object audio (see decoder.hpp's own
// DecodedSubstream::object_metadata/object_audio comments) but nothing in
// this window ever showed what it found — every existing Atmos/OAMD/JOC
// surface here (SoundfieldView, this dialog's own AssignmentPanel/
// GuidedWizard cousins) is encode-side only, authoring a plan still to come.
// This is read-only: open → decode → scrub/play the room, audition an
// object's own recovered audio. No plan, no source, no encoder anywhere in
// the path — the same fundamentally different shape QcDialog.qml's own
// header comment describes for QC-ing a stream, which is why this lives
// beside it as its own modal rather than folded into the Objects tab.
//
// The plan/elevation rectangles below deliberately match the Objects tab's
// own ROOM — PLAN/ROOM — ELEVATION views pixel-for-pixel in layout language
// (same axes, same front/rear/ceiling/floor labels, same room-anchored
// coordinate system, §4.2.1) so a reader who already knows that view reads
// this one for free. The one visual difference is the interaction: that
// view drags a marker to author a position, this one only ever plays back
// literal decoded data, so there is no MouseArea here at all.
Dialog {
    id: root
    objectName: "objectInspectorDialog"

    modal: true
    anchors.centerIn: parent
    width: Math.min(900, parent ? parent.width - 60 : 900)
    height: Math.min(760, parent ? parent.height - 60 : 760)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    // Scrub position into ObjectDecodeController.frames - a plain index,
    // not a time, since decoded frames are literal per-access-unit samples
    // with no authored-path interpolation between them (unlike the Objects
    // tab's own evaluateObjectPath()).
    property int frameIndex: 0
    property bool playing: false

    readonly property var currentFrame:
        ObjectDecodeController.frameCount > 0
        ? ObjectDecodeController.frames[Math.min(root.frameIndex, ObjectDecodeController.frameCount - 1)]
        : null
    readonly property var currentObjects: currentFrame ? currentFrame.objects : []

    // Every decoded frame is one syncframe, kSamplesPerFrame/sample_rate
    // apart - close enough to a constant step for a preview scrubber's own
    // pacing, without needing per-frame timestamps from the model.
    readonly property real stepIntervalMs: 32

    onVisibleChanged: if (!visible) { root.playing = false; ObjectDecodeController.stopAudition(); }

    Connections {
        target: ObjectDecodeController
        function onResultChanged() {
            root.frameIndex = 0;
            root.playing = false;
        }
    }

    Timer {
        id: playTimer
        interval: root.stepIntervalMs
        repeat: true
        running: root.playing && ObjectDecodeController.frameCount > 1
        onTriggered: {
            if (root.frameIndex >= ObjectDecodeController.frameCount - 1) {
                root.playing = false;
                root.frameIndex = 0;
            } else {
                root.frameIndex += 1;
            }
        }
    }

    FileDialog {
        id: objFileDialog
        title: qsTr("Choose an E-AC-3 stream")
        nameFilters: [qsTr("AC-3 / E-AC-3 (*.ac3 *.ec3)"), qsTr("All files (*)")]
        onAccepted: ObjectDecodeController.inspectFile(selectedFile)
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("Inspect objects")
                font.pixelSize: 18
                font.weight: Font.ExtraBold
                font.family: Theme.headingFamily
                color: Theme.text
            }
            Button {
                objectName: "oiCloseButton"
                text: qsTr("Close")
                onClicked: root.close()
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Decodes an already-encoded E-AC-3 file and shows the Dolby Atmos object metadata (OAMD) and per-object audio (JOC) actually recovered from it — not what a source or a plan says, what the bitstream itself carries.")
            font.pixelSize: 12
            color: Theme.neutral700
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3

            Button {
                objectName: "oiChooseFileButton"
                text: qsTr("Choose file…")
                enabled: !ObjectDecodeController.busy
                onClicked: objFileDialog.open()
            }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: ObjectDecodeController.filePath.length > 0
                      ? ObjectDecodeController.filePath : qsTr("No file chosen yet")
                color: Theme.neutral700
                font.pixelSize: 12
                font.family: Theme.monoFamily
            }
            BusyIndicator {
                objectName: "oiBusyIndicator"
                visible: ObjectDecodeController.busy
                running: ObjectDecodeController.busy
                implicitWidth: 24
                implicitHeight: 24
            }
        }

        Text {
            objectName: "oiErrorText"
            Layout.fillWidth: true
            visible: ObjectDecodeController.error.length > 0
            wrapMode: Text.WordWrap
            text: ObjectDecodeController.error
            color: Theme.bad
            font.pixelSize: 12
        }

        Text {
            visible: !ObjectDecodeController.hasResult && ObjectDecodeController.error.length === 0
                     && !ObjectDecodeController.busy
            Layout.fillWidth: true
            Layout.fillHeight: true
            text: qsTr("Choose an E-AC-3 file above to see the objects decoded out of it.")
            color: Theme.textMuted
            font.pixelSize: 12
            verticalAlignment: Text.AlignTop
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: ObjectDecodeController.hasResult
            contentWidth: availableWidth
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                width: parent ? parent.width : 0
                spacing: Theme.space4

                Text {
                    objectName: "oiSummaryText"
                    Layout.fillWidth: true
                    text: ObjectDecodeController.summaryLine
                    font.pixelSize: 12
                    font.family: Theme.monoFamily
                    font.weight: Font.DemiBold
                    color: Theme.text
                }

                // ---- scrub / play -------------------------------------------
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space3

                    Button {
                        objectName: "oiPlayButton"
                        text: root.playing ? qsTr("Pause") : qsTr("Play")
                        enabled: ObjectDecodeController.frameCount > 1
                        onClicked: root.playing = !root.playing
                    }
                    Slider {
                        id: scrub
                        objectName: "oiScrubSlider"
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(0, ObjectDecodeController.frameCount - 1)
                        stepSize: 1
                        // Bound only for the initial render - Slider writes
                        // its own `value` as the user drags, which would
                        // permanently break a plain `value: root.frameIndex`
                        // binding the same way any imperative assignment to
                        // a bound property does (see QML Behavior-on-bound-
                        // property notes elsewhere in this app). The
                        // Connections below re-syncs explicitly on every
                        // frameIndex change instead, so playback (which
                        // moves frameIndex from the Timer, not from a drag)
                        // keeps moving the thumb even after the user has
                        // dragged it once.
                        value: root.frameIndex
                        onMoved: {
                            root.playing = false;
                            root.frameIndex = Math.round(value);
                        }
                    }
                    Connections {
                        target: root
                        function onFrameIndexChanged() { scrub.value = root.frameIndex; }
                    }
                    Text {
                        objectName: "oiFrameLabel"
                        Layout.preferredWidth: 130
                        horizontalAlignment: Text.AlignRight
                        text: root.currentFrame
                              ? qsTr("%1 s · frame %2/%3")
                                    .arg(root.currentFrame.time.toFixed(2))
                                    .arg(root.frameIndex + 1)
                                    .arg(ObjectDecodeController.frameCount)
                              : ""
                        font.pixelSize: 10
                        font.family: Theme.monoFamily
                        color: Theme.textMuted
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.space6

                    // ---- room: plan + elevation -----------------------------
                    ColumnLayout {
                        Layout.preferredWidth: 340
                        Layout.alignment: Qt.AlignTop
                        spacing: Theme.space2

                        Text {
                            text: qsTr("ROOM — PLAN (top-down)")
                            color: Theme.neutral600
                            font.pixelSize: 10
                        }
                        Rectangle {
                            id: room
                            objectName: "oiRoomPlan"
                            Layout.preferredWidth: 340
                            Layout.preferredHeight: 260
                            color: Theme.neutral100
                            border.color: Theme.divider
                            border.width: 1

                            Rectangle {
                                anchors.horizontalCenter: parent.horizontalCenter
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: 1
                                color: Theme.neutral300
                            }
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.right: parent.right
                                height: 1
                                color: Theme.neutral300
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 6
                                text: qsTr("front")
                                color: Theme.neutral500
                                font.pixelSize: 9
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.bottom: parent.bottom
                                anchors.margins: 6
                                text: qsTr("rear")
                                color: Theme.neutral500
                                font.pixelSize: 9
                            }

                            Repeater {
                                objectName: "oiPlanMarkers"
                                model: root.currentObjects

                                delegate: Rectangle {
                                    id: planMarker
                                    required property var modelData
                                    required property int index
                                    readonly property bool auditioning:
                                        index === ObjectDecodeController.auditioningIndex

                                    width: auditioning ? 16 : 12
                                    height: auditioning ? 16 : 12
                                    color: auditioning ? Theme.accent : Theme.neutral800
                                    x: planMarker.modelData.x * room.width - width / 2
                                    y: planMarker.modelData.y * room.height - height / 2

                                    Text {
                                        anchors.left: parent.right
                                        anchors.leftMargin: 3
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: String(planMarker.index + 1)
                                        color: Theme.textMuted
                                        font.pixelSize: 9
                                        font.family: Theme.monoFamily
                                    }
                                }
                            }
                        }

                        Text {
                            Layout.topMargin: Theme.space2
                            text: qsTr("ROOM — ELEVATION (side-on)")
                            color: Theme.neutral600
                            font.pixelSize: 10
                        }
                        Rectangle {
                            id: elevation
                            objectName: "oiElevation"
                            Layout.preferredWidth: 340
                            Layout.preferredHeight: 130
                            color: Theme.neutral100
                            border.color: Theme.divider
                            border.width: 1

                            readonly property real earY: height * 0.66
                            function zToY(z) {
                                return z >= 0 ? earY - z * (earY - 14)
                                              : earY + (-z) * ((height - 10) - earY);
                            }

                            Rectangle {
                                x: 0; width: parent.width
                                y: 14; height: 1
                                color: Theme.neutral300
                            }
                            Text {
                                x: 4; y: 2
                                text: qsTr("ceiling")
                                color: Theme.neutral500
                                font.pixelSize: 9
                            }
                            Rectangle {
                                x: 0; width: parent.width
                                y: elevation.earY; height: 1
                                color: Theme.neutral300
                            }
                            Text {
                                x: 4; y: elevation.earY - 12
                                text: qsTr("ear level")
                                color: Theme.neutral500
                                font.pixelSize: 9
                            }
                            Text {
                                x: 4; y: parent.height - 13
                                text: qsTr("front")
                                color: Theme.neutral500
                                font.pixelSize: 9
                            }
                            Text {
                                x: parent.width - implicitWidth - 4
                                y: parent.height - 13
                                text: qsTr("rear")
                                color: Theme.neutral500
                                font.pixelSize: 9
                            }

                            Repeater {
                                objectName: "oiElevationMarkers"
                                model: root.currentObjects

                                delegate: Rectangle {
                                    id: elevationMarker
                                    required property var modelData
                                    required property int index
                                    readonly property bool auditioning:
                                        index === ObjectDecodeController.auditioningIndex

                                    width: auditioning ? 16 : 12
                                    height: auditioning ? 16 : 12
                                    color: auditioning ? Theme.accent : Theme.neutral800
                                    x: elevationMarker.modelData.y * elevation.width - width / 2
                                    y: elevation.zToY(elevationMarker.modelData.z) - height / 2

                                    Text {
                                        anchors.left: parent.right
                                        anchors.leftMargin: 3
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: String(elevationMarker.index + 1)
                                        color: Theme.textMuted
                                        font.pixelSize: 9
                                        font.family: Theme.monoFamily
                                    }
                                }
                            }
                        }
                    }

                    // ---- object list + audition -----------------------------
                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: Theme.space2

                        Text {
                            text: qsTr("OBJECTS")
                            color: Theme.neutral600
                            font.pixelSize: 10
                        }

                        Repeater {
                            objectName: "oiObjectRows"
                            model: root.currentObjects

                            delegate: RowLayout {
                                id: objectRow
                                required property var modelData
                                required property int index
                                objectName: "oiObjectRow-" + objectRow.index
                                Layout.fillWidth: true
                                spacing: Theme.space3

                                readonly property bool auditioning:
                                    objectRow.index === ObjectDecodeController.auditioningIndex

                                Rectangle {
                                    Layout.preferredWidth: 10
                                    Layout.preferredHeight: 10
                                    color: objectRow.auditioning ? Theme.accent : Theme.neutral800
                                }
                                Text {
                                    Layout.preferredWidth: 60
                                    text: qsTr("obj %1").arg(objectRow.index + 1)
                                    font.pixelSize: 12
                                    font.family: Theme.monoFamily
                                    color: Theme.text
                                }
                                Text {
                                    Layout.preferredWidth: 130
                                    text: qsTr("x %1  y %2  z %3")
                                              .arg(objectRow.modelData.x.toFixed(2))
                                              .arg(objectRow.modelData.y.toFixed(2))
                                              .arg(objectRow.modelData.z.toFixed(2))
                                    font.pixelSize: 10
                                    font.family: Theme.monoFamily
                                    color: Theme.neutral700
                                }
                                Text {
                                    Layout.preferredWidth: 70
                                    text: qsTr("%1 dB").arg(objectRow.modelData.gainDb.toFixed(1))
                                    font.pixelSize: 10
                                    font.family: Theme.monoFamily
                                    color: Theme.neutral700
                                }
                                Item { Layout.fillWidth: true }
                                Button {
                                    objectName: "oiAuditionButton-" + objectRow.index
                                    text: objectRow.auditioning ? qsTr("Stop") : qsTr("Audition")
                                    flat: true
                                    onClicked: ObjectDecodeController.auditionObject(objectRow.index)
                                }
                            }
                        }

                        Text {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.space2
                            wrapMode: Text.WordWrap
                            text: qsTr("Positions and gain are OAMD, read straight off this frame's own metadata. Audition plays JOC's reconstructed audio for that one object — a parametric estimate, not the original source (see docs/library/spatial-and-atmos.md).")
                            font.pixelSize: 10
                            color: Theme.neutral500
                        }
                    }
                }
            }
        }
    }
}
