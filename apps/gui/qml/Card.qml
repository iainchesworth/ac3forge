import QtQuick
import QtQuick.Layouts

import Ac3Forge

// A titled panel. Children are laid out vertically inside `content`.
Rectangle {
    id: root

    property alias title: heading.text
    default property alias content: column.data

    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    radius: Theme.radius
    Layout.fillWidth: true
    implicitHeight: layout.implicitHeight + Theme.pad * 2

    ColumnLayout {
        id: layout
        anchors.fill: parent
        anchors.margins: Theme.pad
        spacing: Theme.gap

        Text {
            id: heading
            color: Theme.textMuted
            font.pixelSize: Theme.fontSmall
            font.bold: true
            font.capitalization: Font.AllUppercase
            visible: text.length > 0
        }

        ColumnLayout {
            id: column
            Layout.fillWidth: true
            spacing: Theme.gap
        }
    }
}
