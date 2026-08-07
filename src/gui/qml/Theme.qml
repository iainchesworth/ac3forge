pragma Singleton

import QtQuick

// Single source of truth for colours, spacing and type, so every page and
// component stays visually consistent (and a dark/light switch later is a
// one-file change).
QtObject {
    readonly property color background: "#12151a"
    readonly property color surface: "#1b2029"
    readonly property color surfaceAlt: "#232936"
    readonly property color border: "#2f3644"
    readonly property color text: "#e6e9ef"
    readonly property color textMuted: "#9aa4b6"
    readonly property color accent: "#4c9aff"
    readonly property color accentText: "#08101f"
    readonly property color good: "#4cc38a"
    readonly property color bad: "#f2555a"

    readonly property int gap: 12
    readonly property int pad: 18
    readonly property int radius: 10

    readonly property int fontNormal: 14
    readonly property int fontSmall: 12
    readonly property int fontTitle: 22
}
