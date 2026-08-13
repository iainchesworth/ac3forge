import QtQuick
import QtQuick.Layouts

import Ac3Forge

// One numbered block of the left rail — the handoff's "01 INPUT" pattern: a
// mono ordinal in accent, an uppercase label, and a 2px rule filling the
// remaining width, with the block's content beneath.
ColumnLayout {
    id: root

    property string ordinal: "01"
    property string label: ""
    default property alias content: body.data

    spacing: Theme.space3

    RowLayout {
        Layout.fillWidth: true
        spacing: Theme.space2

        Text {
            text: root.ordinal
            color: Theme.accent700
            font.pixelSize: 11
            font.family: Theme.monoFamily
        }
        Text {
            text: root.label
            color: Theme.text
            font.pixelSize: 11
            font.letterSpacing: 1.3
            font.weight: Font.DemiBold
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 2
            color: Theme.divider
        }
    }

    ColumnLayout {
        id: body
        Layout.fillWidth: true
        spacing: Theme.space3
    }
}
