import QtQuick
import QtQuick.Layouts

import Ac3Forge

// Two square plan views side by side: the loudspeaker ring seen from above
// (ear level) and, since a flat ring cannot show a ceiling layer, a second
// ring for the height channels a wide E-AC-3 plan can carry. Each speaker
// brightens with its own level; the ear-level plan also draws the energy
// vector the analysis layer computes - the direction a listener would place
// the sound, and how tightly it is focused there. The ceiling plan has no
// vector of its own: with at most a handful of height channels there is
// nothing a single arrow would usefully summarise.
RowLayout {
    id: root

    spacing: 16

    // One pass over channelLevels, split by ring, computed once per publish
    // rather than once per delegate. A channel with no direction at all
    // (LFE, LFE2) never appears on either ring.
    readonly property var earSpeakers: split().ear
    readonly property var ceilingSpeakers: split().ceiling

    function split() {
        const ear = [];
        const ceiling = [];
        const names = EncoderController.channelNames;
        const levels = EncoderController.channelLevels;
        for (let i = 0; i < names.length; i++) {
            const level = i < levels.length ? levels[i] : ({});
            if (level.directional !== true) {
                continue;
            }
            const entry = {
                name: names[i],
                azimuth: level.azimuthDeg !== undefined ? level.azimuthDeg : 0,
                fraction: level.rms !== undefined ? level.rms : 0,
            };
            (level.ceiling === true ? ceiling : ear).push(entry);
        }
        return { ear: ear, ceiling: ceiling };
    }

    // Front/side/rear read off azimuth magnitude; every ceiling speaker is
    // "rear" colour regardless of where it sits on its own ring, per the
    // handoff ("neutral-500 for rears and ceiling"). The centre position -
    // azimuth 0 - is the one accent-coloured speaker on either ring.
    function speakerColor(entry, ceiling) {
        if (entry.azimuth === 0) {
            return Theme.accent;
        }
        if (ceiling) {
            return Theme.neutral500;
        }
        const magnitude = Math.abs(entry.azimuth);
        if (magnitude < 70) {
            return Theme.text;
        }
        if (magnitude < 130) {
            return Theme.neutral600;
        }
        return Theme.neutral500;
    }

    // ---- ear level -----------------------------------------------------
    ColumnLayout {
        spacing: 7

        Text {
            text: qsTr("EAR LEVEL")
            font.pixelSize: 10
            font.letterSpacing: 1
            color: Theme.textMuted
        }

        Item {
            id: earPlan
            Layout.preferredWidth: 140
            Layout.preferredHeight: 140

            readonly property real cx: width / 2
            readonly property real cy: height / 2
            readonly property real ringRadius: Math.min(width, height) / 2 - 18

            function screenX(azimuthDeg, radius) {
                return cx - Math.sin(azimuthDeg * Math.PI / 180) * radius;
            }
            function screenY(azimuthDeg, radius) {
                return cy - Math.cos(azimuthDeg * Math.PI / 180) * radius;
            }

            Rectangle {
                anchors.fill: parent
                color: Theme.neutral100
                border.color: Theme.divider
                border.width: 1
            }
            Rectangle {
                // crosshair, vertical
                x: earPlan.cx; width: 1
                y: 0; height: parent.height
                color: Theme.neutral300
            }
            Rectangle {
                // crosshair, horizontal
                y: earPlan.cy; height: 1
                x: 0; width: parent.width
                color: Theme.neutral300
            }

            // The listener, at the centre.
            Rectangle {
                anchors.centerIn: parent
                width: 22
                height: 22
                radius: 11
                color: "transparent"
                border.color: Theme.neutral400
                border.width: 1
            }

            Rectangle {
                id: earVector
                readonly property var field: EncoderController.soundfield
                readonly property real azimuth: field && field.azimuthDeg !== undefined
                                                ? field.azimuthDeg : 0
                readonly property real magnitude: field && field.magnitude !== undefined
                                                  ? field.magnitude : 0
                visible: field && field.active === true

                x: earPlan.cx
                y: earPlan.cy - height / 2
                width: magnitude * earPlan.ringRadius
                height: 2
                color: Theme.accent
                transformOrigin: Item.Left
                rotation: Math.atan2(-Math.cos(azimuth * Math.PI / 180),
                                     -Math.sin(azimuth * Math.PI / 180)) * 180 / Math.PI
                Behavior on width { NumberAnimation { duration: 60 } }
                Behavior on rotation { RotationAnimation { duration: 90; direction: RotationAnimation.Shortest } }
            }

            Repeater {
                model: root.earSpeakers

                delegate: Rectangle {
                    required property var modelData

                    width: 10
                    height: 10
                    x: earPlan.screenX(modelData.azimuth, earPlan.ringRadius) - width / 2
                    y: earPlan.screenY(modelData.azimuth, earPlan.ringRadius) - height / 2
                    color: root.speakerColor(modelData, false)
                    opacity: 0.5 + 0.5 * modelData.fraction
                    Behavior on opacity { NumberAnimation { duration: 60 } }
                }
            }
        }

        Text {
            readonly property int count: root.earSpeakers.length
            readonly property var field: EncoderController.soundfield
            text: field && field.active === true
                  ? qsTr("%1 speakers · vector %2° front")
                    .arg(count).arg(Math.round(field.azimuthDeg || 0))
                  : qsTr("%1 speakers · silent").arg(count)
            font.family: "monospace"
            font.pixelSize: 10
            color: Theme.textMuted
        }
    }

    // ---- ceiling ---------------------------------------------------------
    ColumnLayout {
        spacing: 7
        visible: root.ceilingSpeakers.length > 0

        Text {
            text: qsTr("CEILING")
            font.pixelSize: 10
            font.letterSpacing: 1
            color: Theme.textMuted
        }

        Item {
            id: ceilingPlan
            Layout.preferredWidth: 140
            Layout.preferredHeight: 140

            readonly property real cx: width / 2
            readonly property real cy: height / 2
            readonly property real ringRadius: Math.min(width, height) / 2 - 18

            function screenX(azimuthDeg, radius) {
                return cx - Math.sin(azimuthDeg * Math.PI / 180) * radius;
            }
            function screenY(azimuthDeg, radius) {
                return cy - Math.cos(azimuthDeg * Math.PI / 180) * radius;
            }

            Rectangle {
                anchors.fill: parent
                color: Theme.neutral100
                border.color: Theme.divider
                border.width: 1
            }
            Rectangle {
                x: ceilingPlan.cx; width: 1
                y: 0; height: parent.height
                color: Theme.neutral300
            }
            Rectangle {
                y: ceilingPlan.cy; height: 1
                x: 0; width: parent.width
                color: Theme.neutral300
            }

            // A flat ring cannot show height, so the ceiling plan draws the
            // 74% dashed circle the handoff calls for instead - a visual cue
            // that this ring means something different from the one beside it.
            Canvas {
                anchors.fill: parent
                onPaint: {
                    const ctx = getContext("2d");
                    ctx.reset();
                    ctx.strokeStyle = Theme.neutral300;
                    ctx.lineWidth = 1;
                    ctx.setLineDash([3, 3]);
                    ctx.beginPath();
                    ctx.arc(width / 2, height / 2, Math.min(width, height) * 0.37, 0, 2 * Math.PI);
                    ctx.stroke();
                }
            }

            Repeater {
                model: root.ceilingSpeakers

                delegate: Rectangle {
                    required property var modelData

                    width: 10
                    height: 10
                    x: ceilingPlan.screenX(modelData.azimuth, ceilingPlan.ringRadius) - width / 2
                    y: ceilingPlan.screenY(modelData.azimuth, ceilingPlan.ringRadius) - height / 2
                    color: root.speakerColor(modelData, true)
                    opacity: 0.5 + 0.5 * modelData.fraction
                    Behavior on opacity { NumberAnimation { duration: 60 } }
                }
            }
        }

        Text {
            readonly property int count: root.ceilingSpeakers.length
            readonly property bool active: root.ceilingSpeakers.some(
                                                (s) => s.fraction > 0.02)
            text: active ? qsTr("%1 height · active").arg(count)
                         : qsTr("%1 height · silent").arg(count)
            font.family: "monospace"
            font.pixelSize: 10
            color: Theme.textMuted
        }
    }
}
