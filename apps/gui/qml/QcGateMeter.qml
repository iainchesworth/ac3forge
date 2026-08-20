import QtQuick
import QtQuick.Layouts

import Ac3Forge

// One measured value drawn against an optional delivery gate — a target
// tolerance band (the loudness gates) or a one-sided ceiling (true peak) —
// modelled on ChannelMeter's own track/fill shape but built to show a
// spec's own numbers alongside the measurement rather than a scale from
// silence to full output. See docs/gui/qc.md.
RowLayout {
    id: root

    property string label: ""
    property string unit: ""
    property bool hasValue: false
    property real value: 0
    property real minValue: -40
    property real maxValue: 0
    // A target tolerance band (the loudness gates): both NaN when this
    // meter has no band to draw (no preset selected, or this meter tracks
    // something no preset gates, e.g. loudness range).
    property real bandLow: NaN
    property real bandHigh: NaN
    // A one-sided ceiling (the true-peak gate): NaN when there is none.
    property real ceilingValue: NaN
    // Whether the CURRENT value is inside the band / under the ceiling —
    // meaningless, and never read, while gated is false.
    property bool pass: true
    readonly property bool gated: !isNaN(root.bandLow) || !isNaN(root.ceilingValue)

    spacing: 6

    Text {
        Layout.preferredWidth: 130
        text: root.label
        color: Theme.text
        font.pixelSize: 11
        font.family: Theme.monoFamily
        horizontalAlignment: Text.AlignRight
    }

    Rectangle {
        id: track
        objectName: "qcGateTrack"
        Layout.fillWidth: true
        Layout.preferredHeight: 16
        color: Theme.neutral200
        clip: true

        function frac(v) {
            return Math.max(0, Math.min(1, (v - root.minValue) / (root.maxValue - root.minValue)));
        }

        // The tolerance band, drawn first so the fill and ceiling sit above
        // it.
        Rectangle {
            visible: !isNaN(root.bandLow) && !isNaN(root.bandHigh)
            x: track.frac(root.bandLow) * track.width
            y: 0
            width: Math.max(1, (track.frac(root.bandHigh) - track.frac(root.bandLow)) * track.width)
            height: parent.height
            color: Theme.neutral400
            opacity: 0.5
        }

        // The measured value's own fill, from the scale's floor up to the
        // value — reads the same way ChannelMeter's own fill bar does.
        // Neutral (Theme.good) while this meter has nothing to gate against,
        // otherwise the pass/fail two-state colour every other good/bad
        // signal in this app already uses (see ChannelMeter's CLIP box).
        Rectangle {
            objectName: "qcGateFill"
            visible: root.hasValue
            x: 0
            y: 0
            width: track.frac(root.value) * track.width
            height: parent.height
            color: root.gated ? (root.pass ? Theme.good : Theme.bad) : Theme.neutral800
        }

        // The ceiling itself — a thin line at the limit, always drawn in the
        // "bad" tone since crossing it is what it exists to flag.
        Rectangle {
            visible: !isNaN(root.ceilingValue)
            x: track.frac(root.ceilingValue) * track.width - 1
            y: 0
            width: 2
            height: parent.height
            color: Theme.bad
        }
    }

    Text {
        objectName: "qcGateValueText"
        Layout.preferredWidth: 96
        text: root.hasValue ? (root.value.toFixed(2) + " " + root.unit) : qsTr("n/a")
        color: root.gated && root.hasValue ? (root.pass ? Theme.good : Theme.bad) : Theme.text
        font.pixelSize: 11
        font.family: Theme.monoFamily
        horizontalAlignment: Text.AlignRight
    }
}
