import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts

import Ac3Forge

// The TrueHD (MLP) lossless lab — the workbench's second codec FAMILY as
// its own standalone dialog, for the same reason QcDialog and
// ObjectInspectorDialog are: pick a WAV → encode losslessly → see it
// verified bit-exact is a different workflow shape to every tab beside it
// (all of which configure a perceptual encode with a bitrate still to
// choose — TrueHD has no bitrate at all). Modelled on QcDialog's scaffold,
// driving TruehdController the way that one drives QcController. See
// truehd_controller.hpp's own header comment for why this is not a third
// codecIndex in EncoderController.
Dialog {
    id: root
    objectName: "truehdDialog"

    modal: true
    anchors.centerIn: parent
    width: Math.min(680, parent ? parent.width - 60 : 680)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    FileDialog {
        id: sourceDialog
        title: qsTr("Choose an integer PCM WAV (16- or 24-bit)")
        nameFilters: [qsTr("WAV (*.wav)"), qsTr("All files (*)")]
        onAccepted: TruehdController.setSource(selectedFile)
    }

    FileDialog {
        id: outputDialog
        title: qsTr("Save TrueHD (MLP) stream as")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("MLP access-unit stream (*.mlp)")]
        defaultSuffix: "mlp"
        onAccepted: TruehdController.setOutput(selectedFile)
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                text: qsTr("TrueHD lossless")
                font.pixelSize: 18
                font.weight: Font.ExtraBold
                font.family: Theme.headingFamily
                color: Theme.text
            }
            Button {
                objectName: "truehdCloseButton"
                text: qsTr("Close")
                onClicked: root.close()
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Integer PCM in, bit-exact PCM back out — every encode is decoded "
                       + "and diffed against the source before it reports success. "
                       + "Objects mode carries each channel as a Dolby Atmos dynamic "
                       + "object with its position metadata riding in-stream.")
            font.pixelSize: 12
            color: Theme.textMuted
        }

        // ---- source ------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3
            Button {
                objectName: "truehdSourceButton"
                text: qsTr("Choose WAV…")
                enabled: !TruehdController.busy
                onClicked: sourceDialog.open()
            }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: TruehdController.sourceReady
                      ? TruehdController.sourceInfo
                      : (TruehdController.sourcePath.length > 0
                         ? TruehdController.sourcePath : qsTr("No source loaded"))
                font.pixelSize: 12
                font.family: Theme.monoFamily
                color: TruehdController.sourceReady ? Theme.text : Theme.textMuted
            }
        }

        // ---- mode --------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3
            Switch {
                id: objectsSwitch
                objectName: "truehdObjectsSwitch"
                text: qsTr("Atmos objects")
                enabled: !TruehdController.busy
                checked: TruehdController.objectsMode
                onToggled: TruehdController.objectsMode = checked
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: objectsSwitch.checked
                      ? qsTr("Every channel becomes a dynamic object — a discrete "
                             + "lossless channel plus OAMD position metadata.")
                      : qsTr("Channels carried exactly as they are in the source.")
                font.pixelSize: 11
                color: Theme.textMuted
            }
        }

        // ---- output ------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3
            Button {
                objectName: "truehdOutputButton"
                text: qsTr("Save as…")
                enabled: !TruehdController.busy && TruehdController.sourceReady
                onClicked: outputDialog.open()
            }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: TruehdController.outputPath.length > 0
                      ? TruehdController.outputPath : qsTr("Choose a source first")
                font.pixelSize: 12
                font.family: Theme.monoFamily
                color: Theme.textMuted
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.divider }

        // ---- run + report ------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space3
            Button {
                objectName: "truehdEncodeButton"
                text: TruehdController.busy ? qsTr("Working…") : qsTr("Encode")
                enabled: !TruehdController.busy && TruehdController.sourceReady
                         && TruehdController.outputPath.length > 0
                onClicked: TruehdController.encode()
            }
            BusyIndicator {
                running: TruehdController.busy
                visible: TruehdController.busy
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
            }
        }

        Text {
            objectName: "truehdResultText"
            Layout.fillWidth: true
            visible: TruehdController.hasResult
            wrapMode: Text.WordWrap
            text: TruehdController.resultInfo
            font.pixelSize: 12
            font.family: Theme.monoFamily
            color: Theme.text
        }

        Text {
            objectName: "truehdErrorText"
            Layout.fillWidth: true
            visible: TruehdController.error.length > 0
            wrapMode: Text.WordWrap
            text: TruehdController.error
            font.pixelSize: 12
            color: Theme.bad
        }
    }
}
