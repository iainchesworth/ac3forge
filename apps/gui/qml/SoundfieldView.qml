import QtQuick
import QtQuick.Layouts

import Ac3Forge

// Two square plan views side by side: the loudspeaker ring seen from above
// (ear level) and, since a flat ring cannot show a ceiling layer, a second
// ring for the height channels a wide E-AC-3 plan can carry. A dot is SOLID
// when the assignments feed its position and HOLLOW when the stream carries
// it silent; each also brightens with its own live level. The ear-level plan
// draws the energy vector the analysis layer computes. The LFE is stated,
// not drawn — it has no direction, so it has no place on a plan.
//
// The speaker lists derive from channelMeta, which changes only when the
// LAYOUT does — never per meter tick. Only each dot's brightness reads the
// 30 Hz channelLevels, by index, so a publish re-evaluates a few number
// bindings instead of tearing every delegate down and building it again
// (which is what binding a Repeater model to channelLevels used to do, and
// exactly the jank the handoff's "the scene has to hold 60 fps" line is
// about).
ColumnLayout {
    id: root

    spacing: 10

    readonly property var earSpeakers: {
        const meta = EncoderController.channelMeta;
        const ear = [];
        for (let i = 0; i < meta.length; i++) {
            const m = meta[i];
            if (m.directional === true && m.ceiling !== true && m.replaced !== true) {
                ear.push({ index: i, azimuth: m.azimuthDeg, fed: m.fed === true });
            }
        }
        return ear;
    }
    readonly property var ceilingSpeakers: {
        const meta = EncoderController.channelMeta;
        const ceiling = [];
        for (let i = 0; i < meta.length; i++) {
            const m = meta[i];
            if (m.directional === true && m.ceiling === true) {
                ceiling.push({ index: i, azimuth: m.azimuthDeg, fed: m.fed === true });
            }
        }
        return ceiling;
    }
    readonly property int lfeCount: {
        const meta = EncoderController.channelMeta;
        let n = 0;
        for (let i = 0; i < meta.length; i++) {
            if (meta[i].directional !== true) n++;
        }
        return n;
    }

    function fedCaption(list) {
        if (EncoderController.atmosEnabled) {
            return qsTr("objects panned in");
        }
        let fed = 0;
        for (let i = 0; i < list.length; i++) {
            if (list[i].fed) fed++;
        }
        if (fed === 0) return qsTr("silent");
        if (fed === list.length) return qsTr("all fed");
        return qsTr("%1 fed").arg(fed);
    }

    // Front/side/rear read off azimuth magnitude; every ceiling speaker is
    // neutral-500 per the handoff. The centre position — azimuth 0 — is the
    // one accent-coloured speaker on either ring.
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

    RowLayout {
        spacing: 16

        // ---- ear level ---------------------------------------------------
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
                // Half the rail each, square - the plans scale with the rail
                // instead of pinning themselves at a 150 px thumbnail.
                Layout.preferredWidth: (root.width - 16) / 2
                Layout.preferredHeight: (root.width - 16) / 2

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
                    x: earPlan.cx; width: 1
                    y: 0; height: parent.height
                    color: Theme.neutral300
                }
                Rectangle {
                    y: earPlan.cy; height: 1
                    x: 0; width: parent.width
                    color: Theme.neutral300
                }

                // The 74% ring the dots sit on.
                Rectangle {
                    anchors.centerIn: parent
                    width: earPlan.ringRadius * 2
                    height: earPlan.ringRadius * 2
                    radius: earPlan.ringRadius
                    color: "transparent"
                    border.color: Theme.neutral300
                    border.width: 1
                }

                // The listener, at the centre.
                Rectangle {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    radius: 10
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

                    // Plain bindings, deliberately with NO Behavior — a
                    // Behavior around a bound property breaks the binding
                    // (see ChannelMeter's identical note), and the ~30 Hz
                    // publish cadence steps smoothly enough on its own.
                    x: earPlan.cx
                    y: earPlan.cy - height / 2
                    width: magnitude * earPlan.ringRadius
                    height: 2
                    color: Theme.accent
                    transformOrigin: Item.Left
                    rotation: Math.atan2(-Math.cos(azimuth * Math.PI / 180),
                                         -Math.sin(azimuth * Math.PI / 180)) * 180 / Math.PI
                }

                Repeater {
                    model: root.earSpeakers

                    delegate: Rectangle {
                        required property var modelData
                        // Live brightness, by index — the one per-tick read.
                        readonly property var live: EncoderController.channelLevels[modelData.index]

                        width: 10
                        height: 10
                        x: earPlan.screenX(modelData.azimuth, earPlan.ringRadius) - width / 2
                        y: earPlan.screenY(modelData.azimuth, earPlan.ringRadius) - height / 2
                        color: modelData.fed ? root.speakerColor(modelData, false) : "transparent"
                        border.color: modelData.fed ? "transparent" : Theme.neutral500
                        border.width: modelData.fed ? 0 : 1
                        // Direct binding, no Behavior: 30 Hz updates are
                        // already smooth for an opacity twinkle, and a
                        // Behavior over a bound property is broken anyway.
                        opacity: 0.5 + 0.5 * (live !== undefined && live.rms !== undefined ? live.rms : 0)
                    }
                }
            }

            Text {
                readonly property int count: root.earSpeakers.length
                readonly property var field: EncoderController.soundfield
                text: field && field.active === true
                      ? qsTr("%1 speakers · vector %2° front")
                        .arg(count).arg(Math.round(field.azimuthDeg || 0))
                      : qsTr("%1 speakers · %2").arg(count).arg(root.fedCaption(root.earSpeakers))
                font.family: Theme.monoFamily
                font.pixelSize: 10
                color: Theme.textMuted
            }
        }

        // ---- ceiling -------------------------------------------------------
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
                Layout.preferredWidth: (root.width - 16) / 2
                Layout.preferredHeight: (root.width - 16) / 2

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

                // A flat ring cannot show height, so the ceiling plan draws
                // its ring dashed instead — a visual cue that this ring means
                // something different from the one beside it.
                Canvas {
                    id: ceilingRing
                    anchors.fill: parent
                    onPaint: {
                        const ctx = getContext("2d");
                        ctx.reset();
                        ctx.strokeStyle = String(Theme.neutral300);
                        ctx.lineWidth = 1;
                        ctx.setLineDash([3, 3]);
                        ctx.beginPath();
                        ctx.arc(width / 2, height / 2, ceilingPlan.ringRadius, 0, 2 * Math.PI);
                        ctx.stroke();
                    }
                    // Repaint on a theme flip: onPaint reads a Theme colour,
                    // and a Canvas has no dependency tracking of its own.
                    Connections {
                        target: Theme
                        function onDarkChanged() { ceilingRing.requestPaint(); }
                        function onPaletteChoiceChanged() { ceilingRing.requestPaint(); }
                    }
                }

                Repeater {
                    model: root.ceilingSpeakers

                    delegate: Rectangle {
                        required property var modelData
                        readonly property var live: EncoderController.channelLevels[modelData.index]

                        width: 10
                        height: 10
                        x: ceilingPlan.screenX(modelData.azimuth, ceilingPlan.ringRadius) - width / 2
                        y: ceilingPlan.screenY(modelData.azimuth, ceilingPlan.ringRadius) - height / 2
                        color: modelData.fed ? root.speakerColor(modelData, true) : "transparent"
                        border.color: modelData.fed ? "transparent" : Theme.neutral500
                        border.width: modelData.fed ? 0 : 1
                        opacity: 0.5 + 0.5 * (live !== undefined && live.rms !== undefined ? live.rms : 0)
                    }
                }
            }

            Text {
                text: qsTr("%1 height · %2")
                      .arg(root.ceilingSpeakers.length)
                      .arg(root.fedCaption(root.ceilingSpeakers))
                font.family: Theme.monoFamily
                font.pixelSize: 10
                color: Theme.textMuted
            }
        }
    }

    // The LFE is stated, not drawn — no direction, no place on a plan.
    RowLayout {
        spacing: 8
        visible: root.lfeCount > 0

        Rectangle {
            Layout.preferredWidth: 16
            Layout.preferredHeight: 5
            color: Theme.text
        }
        Text {
            text: root.lfeCount > 1
                  ? qsTr("two independent low-frequency channels · no direction")
                  : qsTr("one low-frequency channel · no direction")
            font.family: Theme.monoFamily
            font.pixelSize: 10
            color: Theme.textMuted
        }
    }

    // What solid and hollow mean, in words, per the handoff.
    Text {
        Layout.fillWidth: true
        text: EncoderController.atmosEnabled
              ? qsTr("Solid dots are bed positions a source feeds. Objects are not here — they move, so they live in the room view on the Objects tab.")
              : qsTr("Solid dots are fed by a source. Hollow dots are positions the stream carries silent.")
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.textMuted
    }
}
