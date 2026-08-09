import QtQuick

import Ac3Forge

// A small horizontal group of mutually exclusive text options - the
// ".seg"/".seg-opt" pattern the handoff uses throughout (Basic/Advanced,
// File/Live capture, Coded/Rendered, Author a path/Drive it live). Zero
// radius, one active fill in accent, everything else reads as plain text.
Row {
    id: root

    // [{ value: "basic", label: "Basic" }, ...]
    property var model: []
    property string currentValue: ""
    property int segHeight: 28
    property int fontSize: 13
    signal selected(string value)

    spacing: 0

    Repeater {
        model: root.model

        delegate: Rectangle {
            id: seg
            required property var modelData
            readonly property bool active: modelData.value === root.currentValue

            height: root.segHeight
            implicitWidth: label.implicitWidth + 18
            color: active ? Theme.accent : "transparent"
            border.color: Theme.divider
            border.width: 1

            Text {
                id: label
                anchors.centerIn: parent
                text: modelData.label
                color: seg.active ? Theme.bg : Theme.text
                font.pixelSize: root.fontSize
            }

            MouseArea {
                anchors.fill: parent
                onClicked: root.selected(seg.modelData.value)
            }
        }
    }
}
