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
                text: qsTr("measure")
                enabled: !EncoderController.busy
                checked: EncoderController.measureDialnorm
                onToggled: EncoderController.measureDialnorm = checked
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
}
