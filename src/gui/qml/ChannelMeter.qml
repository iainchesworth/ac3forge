import QtQuick
import QtQuick.Layouts

import Ac3Forge

// One channel's level: an RMS bar under a peak marker and a slower hold
// marker, on a scale that runs linearly in decibels. The numbers and their
// positions on the bar both arrive from the C++ analysis layer — nothing here
// converts anything, so a bar and a printed level always agree.
RowLayout {
    id: root

    // One entry of EncoderController.channelLevels; empty while no layout is
    // loaded, hence the defaults on every read below.
    property var level: ({})
    property string channelName: ""
    // The LFE has no direction and no business being drawn as one.
    readonly property bool directional: level.directional === true
    readonly property real peakDb: level.peakDb !== undefined ? level.peakDb : -120
    readonly property bool clipped: level.clipped === true
    // A channel the routing puts nothing into. It reads -inf for a legitimate
    // reason — the source has nothing that belongs there — which is worth
    // telling apart from a channel that should be carrying audio and is not.
    readonly property bool fed: level.fed !== false

    // Green until the last few decibels, where a mastering engineer starts
    // paying attention, then amber, then red once a sample has actually hit
    // full scale.
    readonly property color barColor: clipped || peakDb > -1.0
                                      ? Theme.bad
                                      : (peakDb > -6.0 ? Theme.warn : Theme.good)

    spacing: 8

    Text {
        // Wide enough for the longest coded-channel name: a bed channel a
        // dependent substream replaces is marked "Ls (bed)", because a 7.1
        // display would otherwise show "Ls" twice with different levels and
        // no way to tell which reading belonged to which.
        Layout.preferredWidth: 58
        text: root.channelName
        color: root.directional ? Theme.text : Theme.textMuted
        opacity: root.fed ? 1.0 : 0.45
        font.pixelSize: Theme.fontSmall
        font.bold: true
        elide: Text.ElideLeft
        horizontalAlignment: Text.AlignRight
    }

    Rectangle {
        id: track
        Layout.fillWidth: true
        Layout.preferredHeight: 14
        radius: Theme.radius
        color: Theme.surfaceAlt
        border.color: Theme.border
        border.width: 1
        opacity: root.fed ? 1.0 : 0.45
        clip: true

        readonly property real inner: width - 2

        Rectangle {
            x: 1
            y: 1
            height: parent.height - 2
            width: track.inner * (root.level.rms !== undefined ? root.level.rms : 0)
            radius: Theme.radius
            color: root.barColor
            opacity: 0.55
            Behavior on width { NumberAnimation { duration: 40 } }
        }

        // The peak sits above its own RMS: a bright edge rather than a fill,
        // so a transient stays visible against the body of the signal.
        Rectangle {
            x: 1 + Math.max(0, track.inner * (root.level.peak !== undefined ? root.level.peak : 0) - 2)
            y: 1
            width: 2
            height: parent.height - 2
            color: root.barColor
            visible: root.peakDb > EncoderController.meterFloorDb
        }

        // The hold marker lags the peak down, so the loudest moment of the
        // last second or so stays readable after the sound has gone.
        Rectangle {
            x: 1 + Math.max(0, track.inner * (root.level.hold !== undefined ? root.level.hold : 0) - 1)
            y: 1
            width: 1
            height: parent.height - 2
            color: Theme.text
            visible: (root.level.holdDb !== undefined ? root.level.holdDb : -120)
                     > EncoderController.meterFloorDb
        }
    }

    Text {
        Layout.preferredWidth: 46
        text: root.peakDb <= EncoderController.meterFloorDb
              ? "-∞" : root.peakDb.toFixed(1)
        color: root.clipped ? Theme.bad : Theme.textMuted
        font.pixelSize: Theme.fontSmall
        font.family: "monospace"
        horizontalAlignment: Text.AlignRight
    }

    Rectangle {
        Layout.preferredWidth: 30
        Layout.preferredHeight: 14
        radius: Theme.radius
        color: root.clipped ? Theme.bad : "transparent"
        border.color: root.clipped ? Theme.bad : Theme.border
        border.width: 1

        Text {
            anchors.centerIn: parent
            text: qsTr("CLIP")
            color: root.clipped ? Theme.accentText : Theme.border
            font.pixelSize: 8
            font.bold: true
        }
    }
}
