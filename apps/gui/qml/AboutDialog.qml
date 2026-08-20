import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Ac3Forge

// About — reachable from Main.qml's control row, beside Preferences.
// appVersionDetails is a context property main.cpp sets once at startup
// from ac3::version_details() (build-time-immutable text: version, git
// commit/branch/dirty state, build target) - no C++ round trip needed here
// for something that never changes at runtime. Modelled directly on
// PreferencesDialog.qml's shape (modal, centered, squared border) rather
// than introducing a second dialog style.
Dialog {
    id: root

    modal: true
    anchors.centerIn: parent
    width: Math.min(560, parent ? parent.width - 60 : 560)
    padding: Theme.space6
    title: ""

    background: Rectangle {
        color: Theme.bg
        border.color: Theme.text
        border.width: 2
    }

    component AboutKicker: Text {
        Layout.topMargin: Theme.space2
        font.pixelSize: 10
        font.letterSpacing: 1.2
        color: Theme.textMuted
    }
    component AboutBody: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: Theme.textMuted
        onLinkActivated: (link) => Qt.openUrlExternally(link)
    }

    contentItem: ColumnLayout {
        spacing: Theme.space4

        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.space4

            Image {
                source: "qrc:/icons/ac3forge-256.png"
                Layout.preferredWidth: 64
                Layout.preferredHeight: 64
                smooth: true
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Theme.space1

                Text {
                    text: qsTr("ac3forge")
                    font.pixelSize: 22
                    font.family: Theme.headingFamily
                    font.weight: Font.ExtraBold
                    color: Theme.text
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Clean-room AC-3 / E-AC-3 encoder — ATSC A/52, ETSI TS 103 420")
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    color: Theme.neutral700
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

        AboutKicker { text: qsTr("VERSION") }
        Text {
            objectName: "aboutVersionText"
            Layout.fillWidth: true
            text: appVersionDetails
            font.family: Theme.monoFamily
            font.pixelSize: 12
            color: Theme.text
            wrapMode: Text.WordWrap
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 2; color: Theme.divider }

        AboutKicker { text: qsTr("LICENSE") }
        AboutBody {
            textFormat: Text.RichText
            text: qsTr("ac3forge is free software: you can redistribute it and/or modify it "
                        + "under the terms of the GNU General Public License as published by "
                        + "the Free Software Foundation, either version 3 of the License, or "
                        + "(at your option) any later version. It is distributed WITHOUT ANY "
                        + "WARRANTY; see the <a href=\"https://www.gnu.org/licenses/gpl-3.0.html\">"
                        + "GNU General Public License</a> for details.")
        }
        AboutBody {
            // The Archivo font this dialog (and the whole app) renders in is
            // bundled under a separate license - apps/gui/fonts/OFL.txt ships
            // beside the faces because SIL OFL 1.1 requires attribution to
            // travel with the font; this is that attribution.
            text: qsTr("Includes the Archivo typeface, licensed under the SIL Open Font License 1.1.")
        }

        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Button {
                objectName: "aboutCloseButton"
                text: qsTr("Close")
                highlighted: true
                onClicked: root.close()
            }
        }
    }
}
