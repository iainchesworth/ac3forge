import QtQuick
import QtQuick.Layouts

import Ac3Forge

// One channel's level, in the handoff's meter-row grid: name (56) · track ·
// dB readout (50) · CLIP box (30). The numbers and their positions on the bar
// both arrive from the C++ analysis layer — nothing here converts anything,
// so a bar and a printed level always agree.
RowLayout {
    id: root

    // One entry of EncoderController.channelLevels; empty while no layout is
    // loaded, hence the defaults on every read below. The row's own identity
    // (its name, whether the routing feeds it) comes from channelMeta via
    // the two properties beneath, which only change when the layout does.
    property var level: ({})
    property string channelName: ""
    // A channel the routing puts nothing into. It reads -inf for a legitimate
    // reason — the source has nothing that belongs there — which is worth
    // telling apart from a channel that should be carrying audio and is not.
    property bool fed: true

    readonly property real peakDb: level.peakDb !== undefined ? level.peakDb : -120
    readonly property bool clipped: level.clipped === true

    // The handoff's two-state fill: neutral ink until the last few decibels,
    // accent once a peak crosses −6 dBFS or a sample has clipped — and only
    // for a channel actually fed; silence has no business glowing.
    readonly property color barColor: fed && (clipped || peakDb > -6.0)
                                      ? Theme.accent : Theme.neutral800

    spacing: 6

    Text {
        Layout.preferredWidth: 56
        text: root.channelName
        color: Theme.text
        opacity: root.fed ? 1.0 : 0.45
        font.pixelSize: 11
        font.family: Theme.monoFamily
        elide: Text.ElideLeft
        horizontalAlignment: Text.AlignRight
    }

    Rectangle {
        id: track
        Layout.fillWidth: true
        Layout.preferredHeight: 13
        color: Theme.neutral200
        opacity: root.fed ? 1.0 : 0.45
        clip: true

        // A plain binding, deliberately with NO Behavior: a Behavior wrapped
        // around a bound property silently breaks the binding once these
        // delegates are stable (it only ever appeared to work while the old
        // meter tore its delegates down on every publish). The C++ side
        // already paces publishes at ~30 Hz, which steps smoothly enough
        // that an easing layer earns nothing here.
        Rectangle {
            x: 0
            y: 0
            height: parent.height
            width: track.width * (root.fed && root.level.rms !== undefined ? root.level.rms : 0)
            color: root.barColor
        }

        // The peak sits above its own RMS: a bright edge rather than a fill,
        // so a transient stays visible against the body of the signal.
        Rectangle {
            x: Math.max(0, track.width * (root.level.peak !== undefined ? root.level.peak : 0) - 2)
            y: 0
            width: 2
            height: parent.height
            color: root.barColor
            visible: root.fed && root.peakDb > EncoderController.meterFloorDb
        }

        // The hold marker lags the peak down, so the loudest moment of the
        // last second or so stays readable after the sound has gone.
        Rectangle {
            x: Math.max(0, track.width * (root.level.hold !== undefined ? root.level.hold : 0) - 2)
            y: 0
            width: 2
            height: parent.height
            color: Theme.text
            visible: root.fed
                     && (root.level.holdDb !== undefined ? root.level.holdDb : -120)
                        > EncoderController.meterFloorDb
        }
    }

    Text {
        Layout.preferredWidth: 50
        text: !root.fed || root.peakDb <= EncoderController.meterFloorDb
              ? "-∞" : root.peakDb.toFixed(1)
        color: root.clipped ? Theme.accent700 : Theme.text
        opacity: root.fed ? 1.0 : 0.45
        font.pixelSize: 11
        font.family: Theme.monoFamily
        horizontalAlignment: Text.AlignRight
    }

    Rectangle {
        Layout.preferredWidth: 30
        Layout.preferredHeight: 13
        color: root.clipped ? Theme.accent : "transparent"
        border.color: root.clipped ? Theme.accent : Theme.neutral300
        border.width: 1
        // An unfed channel cannot clip - its CLIP box dims with the rest of
        // the row instead of implying a judgement is being made.
        opacity: root.fed ? 1.0 : 0.45

        Text {
            anchors.centerIn: parent
            text: qsTr("CLIP")
            color: root.clipped ? Theme.bg : Theme.neutral500
            font.pixelSize: 8
            font.bold: true
        }
    }
}
