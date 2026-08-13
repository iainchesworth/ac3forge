import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// The first-run screen: what the window shows until a source has ever been
// chosen. Everything else follows from the source — the layouts on offer,
// the routing, the meters — so the one job here is bringing audio in.
RowLayout {
    id: root

    signal chooseFile()
    signal captureLive()
    signal openTestSignal()

    spacing: Theme.space8

    // Left: the ask.
    ColumnLayout {
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.alignment: Qt.AlignTop
        spacing: Theme.space4

        Text {
            text: qsTr("FIRST RUN")
            font.pixelSize: 10
            font.letterSpacing: 1.5
            color: Theme.accent700
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Bring in some audio.")
            font.pixelSize: 52
            font.family: Theme.headingFamily
            font.weight: Font.ExtraBold
            wrapMode: Text.WordWrap
            color: Theme.text
        }
        Text {
            Layout.fillWidth: true
            text: qsTr("Everything else follows from the source: the layouts on offer, the routing, the meters. Nothing is configured until there is something to configure.")
            font.pixelSize: 15
            wrapMode: Text.WordWrap
            color: Theme.neutral700
        }

        Item { Layout.preferredHeight: Theme.space4 }

        Repeater {
            model: [
                { key: "file", label: qsTr("Choose a WAV file…"), primary: true },
                { key: "live", label: qsTr("Capture from a device — microphone or loopback"), primary: false },
                { key: "test", label: qsTr("Open the bundled 5.1 test signal"), primary: false },
            ]
            delegate: Rectangle {
                required property var modelData
                objectName: "firstRun-" + modelData.key

                Layout.fillWidth: true
                Layout.preferredHeight: 52
                color: modelData.primary ? Theme.accent
                                         : (firstRunHover.hovered ? Theme.neutral200 : "transparent")
                border.color: modelData.primary ? Theme.accent : Theme.divider
                border.width: 1

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: Theme.space4
                    text: modelData.label
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    color: modelData.primary ? Theme.bg : Theme.text
                }
                HoverHandler { id: firstRunHover }
                TapHandler {
                    onTapped: {
                        if (modelData.key === "file") root.chooseFile();
                        else if (modelData.key === "live") root.captureLive();
                        else root.openTestSignal();
                    }
                }
            }
        }
    }

    // Right: what this window does, one sentence per part.
    Rectangle {
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        Layout.fillHeight: true
        color: Theme.neutral100

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Theme.space6
            spacing: Theme.space4

            Text {
                text: qsTr("WHAT THIS WINDOW DOES")
                font.pixelSize: 10
                font.letterSpacing: 1.5
                color: Theme.textMuted
            }

            Repeater {
                model: [
                    { n: "01", title: qsTr("The signal stays on the left"),
                      body: qsTr("Source, meters and the room: always on screen, never scrolled away while you configure.") },
                    { n: "02", title: qsTr("The stream is built on the right"),
                      body: qsTr("Format, coding tools, metadata and objects, in four panels rather than one 1,950 px column.") },
                    { n: "03", title: qsTr("Encoding is a run, not a moment"),
                      body: qsTr("Every encode lands in a run list with its settings, its result and the exact ac3cli line that reproduces it.") },
                ]
                delegate: ColumnLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: Theme.space2

                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.space3
                        Text {
                            text: modelData.n
                            font.pixelSize: 11
                            font.family: Theme.monoFamily
                            color: Theme.accent700
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Text {
                                text: modelData.title
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                color: Theme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                text: modelData.body
                                wrapMode: Text.WordWrap
                                font.pixelSize: 12
                                color: Theme.neutral700
                            }
                        }
                    }
                }
            }

            Item { Layout.fillHeight: true }

            Text {
                Layout.fillWidth: true
                text: qsTr("Advanced coding tools and broadcast metadata start hidden. Switch Controls to Advanced or Expert at any time.")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.textMuted
            }
        }
    }
}
