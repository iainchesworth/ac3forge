import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// DRC profile, dialnorm and the measure checkbox - "Loudness" in the
// handoff's own naming, and the one group of metadata controls that
// appears in Basic as well as Advanced (both have sane defaults, but
// dialogue level is a real creative choice). Basic hosts it on Format;
// Advanced moves it onto Metadata alongside Downmix - never both at once,
// so it is never shown twice.
ColumnLayout {
    Layout.fillWidth: true
    spacing: Theme.gap

    GridLayout {
        Layout.fillWidth: true
        columns: 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.gap

        Text {
            text: qsTr("DRC profile")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
        }
        ComboBox {
            Layout.fillWidth: true
            enabled: !EncoderController.busy
            model: EncoderController.drcNames
            currentIndex: EncoderController.drcIndex
            onActivated: EncoderController.drcIndex = currentIndex
        }

        Text {
            text: qsTr("dialnorm")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            SpinBox {
                from: 1
                to: 31
                enabled: !EncoderController.busy && !EncoderController.measureDialnorm
                value: EncoderController.dialnorm
                onValueModified: EncoderController.dialnorm = value
            }
            CheckBox {
                text: qsTr("Measure it from the programme")
                // Auto-measurement needs each programme measured on its own
                // (see the Programme 2 block below) - encodeChannels refuses
                // it for dual mono rather than measuring the wrong thing, so
                // the control is disabled here instead of offering something
                // that would fail at encode time.
                enabled: !EncoderController.busy && !EncoderController.dualMono
                checked: EncoderController.measureDialnorm
                onToggled: EncoderController.measureDialnorm = checked

                ToolTip.visible: !enabled && hovered
                ToolTip.text: qsTr("Not yet supported for dual mono — set both programmes' dialnorm by hand.")
            }
        }
    }

    Text {
        Layout.fillWidth: true
        text: qsTr("dialnorm says where dialogue sits below full scale (§5.4.2.8). Measuring derives it from BS.1770-4 gated loudness over the whole programme; getting it wrong is not cosmetic, since a levelled system plays the difference.")
        color: Theme.textMuted
        font.pixelSize: Theme.fontSmall
        wrapMode: Text.WordWrap
    }

    // Programme 2's own dialnorm (§5.4.2.16) - dual mono only. Ch1 and Ch2
    // never share a downmix to average across (§E1.3), so each programme
    // states its own dialogue level rather than reusing the one above.
    GridLayout {
        Layout.fillWidth: true
        Layout.topMargin: Theme.gap
        columns: 2
        columnSpacing: Theme.gap
        rowSpacing: Theme.gap
        visible: EncoderController.dualMono

        Text {
            text: qsTr("dialnorm — programme 2")
            color: Theme.text
            font.pixelSize: Theme.fontNormal
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.gap

            SpinBox {
                from: 1
                to: 31
                enabled: !EncoderController.busy && !EncoderController.measureDialnorm2
                value: EncoderController.dialnorm2
                onValueModified: EncoderController.dialnorm2 = value
            }
            CheckBox {
                text: qsTr("Measure it from the programme")
                enabled: false
                checked: EncoderController.measureDialnorm2
                onToggled: EncoderController.measureDialnorm2 = checked

                ToolTip.visible: hovered
                ToolTip.text: qsTr("Not yet supported for dual mono — set both programmes' dialnorm by hand.")
            }
        }
    }
}
