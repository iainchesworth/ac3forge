import QtQuick

import Ac3Forge

// The loudspeaker ring seen from above, listener at the centre facing up.
// Each speaker brightens with its own level; the arrow is the energy vector
// the analysis layer computes — the direction a listener would place the
// sound, and how tightly it is focused there.
Item {
    id: root

    implicitWidth: 176
    implicitHeight: 176

    readonly property real cx: width / 2
    readonly property real cy: height / 2
    readonly property real ringRadius: Math.min(width, height) / 2 - 22
    readonly property var field: EncoderController.soundfield
    readonly property int lfeIndex: EncoderController.channelNames.indexOf("LFE")
    // An empty object rather than null for the "no such channel" case: a null
    // var binding compiles to QVariant::fromValue<std::nullptr_t>, whose
    // early return trips /W4 /WX inside Qt's own header.
    readonly property var lfeEntry: lfeIndex >= 0
                                    && lfeIndex < EncoderController.channelLevels.length
                                    ? EncoderController.channelLevels[lfeIndex] : ({})
    readonly property real lfeLevel: lfeEntry.rms !== undefined ? lfeEntry.rms : 0

    // Azimuth is degrees counterclockwise from front (ITU-R BS.775). On
    // screen, front is up and counterclockwise runs to the left.
    function screenX(azimuthDeg, radius) {
        return cx - Math.sin(azimuthDeg * Math.PI / 180) * radius;
    }
    function screenY(azimuthDeg, radius) {
        return cy - Math.cos(azimuthDeg * Math.PI / 180) * radius;
    }

    Rectangle {
        anchors.centerIn: parent
        width: root.ringRadius * 2
        height: width
        radius: width / 2
        color: "transparent"
        border.color: Theme.border
        border.width: 1
    }

    // The LFE is drawn as a halo at the listener rather than a point on the
    // ring: it is the one channel with no direction at all.
    Rectangle {
        anchors.centerIn: parent
        width: 22 + 26 * root.lfeLevel
        height: width
        radius: width / 2
        color: "transparent"
        border.color: Theme.accent
        border.width: 1
        opacity: 0.15 + 0.55 * root.lfeLevel
        visible: root.lfeIndex >= 0
        Behavior on width { NumberAnimation { duration: 60 } }
    }

    Rectangle {
        x: root.cx - 2
        y: root.cy - 2
        width: 4
        height: 4
        radius: 2
        color: Theme.textMuted
    }

    // The energy vector. Drawn from the centre with transformOrigin at its
    // left edge, so rotation pivots on the listener.
    Rectangle {
        id: vector
        readonly property real azimuth: root.field && root.field.azimuthDeg !== undefined
                                        ? root.field.azimuthDeg : 0
        readonly property real magnitude: root.field && root.field.magnitude !== undefined
                                          ? root.field.magnitude : 0
        visible: root.field && root.field.active === true

        x: root.cx
        y: root.cy - height / 2
        width: magnitude * root.ringRadius
        height: 3
        radius: 1.5
        color: Theme.accent
        transformOrigin: Item.Left
        rotation: Math.atan2(-Math.cos(azimuth * Math.PI / 180),
                             -Math.sin(azimuth * Math.PI / 180)) * 180 / Math.PI
        Behavior on width { NumberAnimation { duration: 60 } }
        Behavior on rotation { RotationAnimation { duration: 90; direction: RotationAnimation.Shortest } }
    }

    Rectangle {
        readonly property real tipRadius: vector.magnitude * root.ringRadius
        visible: vector.visible
        x: root.screenX(vector.azimuth, tipRadius) - 5
        y: root.screenY(vector.azimuth, tipRadius) - 5
        width: 10
        height: 10
        radius: 5
        color: Theme.accent
    }

    Repeater {
        model: EncoderController.channelNames

        delegate: Item {
            required property int index
            required property string modelData

            readonly property var level: index < EncoderController.channelLevels.length
                                         ? EncoderController.channelLevels[index] : ({})
            readonly property bool directional: level.directional === true
            readonly property real fraction: level.rms !== undefined ? level.rms : 0
            readonly property real azimuth: level.azimuthDeg !== undefined
                                            ? level.azimuthDeg : 0

            visible: directional
            x: root.screenX(azimuth, root.ringRadius) - width / 2
            y: root.screenY(azimuth, root.ringRadius) - height / 2
            width: 16
            height: 16

            Rectangle {
                anchors.centerIn: parent
                width: 8 + 8 * parent.fraction
                height: width
                radius: width / 2
                color: Theme.good
                opacity: 0.25 + 0.75 * parent.fraction
                Behavior on width { NumberAnimation { duration: 60 } }
            }

            Text {
                // Labels sit further out along the same bearing, so they never
                // land on top of the marker they belong to.
                x: root.screenX(parent.azimuth, root.ringRadius + 15) - root.screenX(parent.azimuth, root.ringRadius) - width / 2 + parent.width / 2
                y: root.screenY(parent.azimuth, root.ringRadius + 15) - root.screenY(parent.azimuth, root.ringRadius) - height / 2 + parent.height / 2
                text: parent.modelData
                color: Theme.textMuted
                font.pixelSize: 10
                font.bold: true
            }
        }
    }
}
