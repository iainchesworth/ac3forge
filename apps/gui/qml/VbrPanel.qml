import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// Variable bit rate: E-AC-3, file output only (see EncoderController::
// vbrAvailable()'s own comment on why object mode and a live session are
// excluded). Shared between the Format tab (Advanced/Expert) and the Guided
// wizard's own Rate mode step - one set of bindings rather than two copies
// that could drift, since both read and write the exact same controller
// state.
ColumnLayout {
    visible: EncoderController.vbrAvailable
    spacing: Theme.gap

    // The Preferences "show the plain-language notes beside controls" knob -
    // bound by whichever page instantiates this panel.
    property bool showExplanations: true

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.gap

        Text {
            text: qsTr("Rate mode")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
        }
        SegmentedControl {
            objectName: "rateModeControl"
            model: [{ value: "cbr", label: qsTr("Constant") },
                    { value: "vbr", label: qsTr("Variable") }]
            currentValue: EncoderController.vbrEnabled ? "vbr" : "cbr"
            onSelected: (value) => {
                EncoderController.vbrEnabled = value === "vbr";
                EncoderController.formatDefaultsTouched = true;
            }
        }
        Item { Layout.fillWidth: true }
    }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.gap
        visible: EncoderController.vbrEnabled
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("Quality")
                color: Theme.neutral600
                font.pixelSize: 10
            }
            Item { Layout.fillWidth: true }
            Text {
                text: qsTr("%1 / 100").arg(EncoderController.vbrQuality)
                color: Theme.text
                font.pixelSize: 11
                font.family: Theme.monoFamily
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            Text {
                text: qsTr("0 · smallest")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
            Slider {
                objectName: "vbrQualitySlider"
                Layout.fillWidth: true
                from: 0
                to: 100
                stepSize: 1
                value: EncoderController.vbrQuality
                onMoved: {
                    EncoderController.vbrQuality = Math.round(value);
                    EncoderController.formatDefaultsTouched = true;
                }
            }
            Text {
                text: qsTr("100 · best")
                color: Theme.textMuted
                font.pixelSize: Theme.fontSmall
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            CheckBox {
                id: vbrMinCheck
                objectName: "vbrMinCheck"
                text: qsTr("Set a minimum bit rate")
                checked: EncoderController.vbrMinEnabled
                onToggled: EncoderController.vbrMinEnabled = checked
            }
            SpinBox {
                objectName: "vbrMinSpin"
                from: 32
                to: 6144
                stepSize: 32
                enabled: vbrMinCheck.checked
                value: EncoderController.vbrMinKbps
                onValueModified: EncoderController.vbrMinKbps = value
            }

            CheckBox {
                id: vbrMaxCheck
                objectName: "vbrMaxCheck"
                text: qsTr("Set a maximum bit rate")
                checked: EncoderController.vbrMaxEnabled
                onToggled: EncoderController.vbrMaxEnabled = checked
            }
            SpinBox {
                objectName: "vbrMaxSpin"
                from: 32
                to: 6144
                stepSize: 32
                enabled: vbrMaxCheck.checked
                value: EncoderController.vbrMaxKbps
                onValueModified: EncoderController.vbrMaxKbps = value
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("Bounds are optional — unticked means no bound at all, not a default one. Currently %1 · %2.")
                  .arg(EncoderController.vbrMinEnabled
                       ? qsTr("≥ %1 kbps").arg(EncoderController.vbrMinKbps)
                       : qsTr("no floor"))
                  .arg(EncoderController.vbrMaxEnabled
                       ? qsTr("≤ %1 kbps").arg(EncoderController.vbrMaxKbps)
                       : qsTr("no ceiling"))
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }

        Text {
            visible: showExplanations
            Layout.fillWidth: true
            text: qsTr("Quality is encoder-relative, not a fixed target — bit cost rises steeply above roughly half the range, so a high quality with no maximum will often refuse real programme material outright. Bit rate above still feeds the coupling/spectral-extension frequency defaults, not a target rate.")
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            text: qsTr("ac3cli vbr token:  %1").arg(EncoderController.vbrToken)
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
        }
    }
}
