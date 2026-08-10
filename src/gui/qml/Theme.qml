pragma Singleton

import QtQuick

// Single source of truth for colour, spacing and type, so every page and
// component stays visually consistent. Tokens follow the Modernist design
// system: zero radius everywhere, an Archivo type scale, and colour ramps in
// two families (neutral and accent), each 100 (lightest) to 900 (darkest).
//
// Dark is DERIVED from the light values below by inverting each colour's HSL
// lightness (see invert()) rather than hand-tuned as a second palette - both
// themes must come from the same tokens.
// Every component binds to a token name (Theme.neutral800, Theme.accent...)
// and gets whichever theme is active; nothing outside this file knows there
// are two palettes; nothing outside this file may know either.
QtObject {
    id: theme

    // "system" follows the OS colour scheme. A future Preferences dialog
    // (README "Screens 2-4") lets this be overridden and persisted; the knob
    // exists now so that override has somewhere to write.
    property string preference: "system"
    readonly property bool dark: preference === "dark"
                                  || (preference === "system"
                                      && Application.styleHints.colorScheme === Qt.Dark)

    // Mirrors 1 - L, keeping hue and saturation: a light near-white becomes a
    // dark near-black, a mid-lightness accent is left alone. This is a
    // mechanical derivation, not a colour choice - see the file comment.
    function invert(c) {
        return Qt.hsla(c.hslHue < 0 ? 0 : c.hslHue, c.hslSaturation, 1.0 - c.hslLightness, c.a);
    }
    function pick(light) {
        return theme.dark ? theme.invert(light) : light;
    }

    // ---- Modernist tokens, light values (the handoff's own palette) -------
    readonly property color _bgLight: "#f3f2f2"
    readonly property color _surfaceLight: "#eae9e9"
    readonly property color _textLight: "#201e1d"
    readonly property color _accentLight: "#ec3013"

    readonly property color _neutral100Light: "#f8f4f4"
    readonly property color _neutral200Light: "#eae7e7"
    readonly property color _neutral300Light: "#d7d3d3"
    readonly property color _neutral400Light: "#bab6b6"
    readonly property color _neutral500Light: "#9b9797"
    readonly property color _neutral600Light: "#7d7979"
    readonly property color _neutral700Light: "#605d5d"
    readonly property color _neutral800Light: "#444141"
    readonly property color _neutral900Light: "#2d2b2b"

    readonly property color _accent100Light: "#fff2ef"
    readonly property color _accent200Light: "#ffe0d9"
    readonly property color _accent300Light: "#ffc4b8"
    readonly property color _accent400Light: "#ff9783"
    readonly property color _accent500Light: "#ff563c"
    readonly property color _accent600Light: "#dd2b0f"
    readonly property color _accent700Light: "#ae1800"
    readonly property color _accent800Light: "#7c1405"
    readonly property color _accent900Light: "#4d170e"

    // ---- resolved for the active theme -------------------------------------
    readonly property color bg: pick(_bgLight)
    readonly property color surface: pick(_surfaceLight)
    readonly property color text: pick(_textLight)
    readonly property color accent: pick(_accentLight)
    // Text at 40% alpha, same recipe as the CSS token
    // (color-mix(in srgb, #201e1d 40%, transparent)): always legible against
    // whatever "text" resolves to this theme, because it IS that colour.
    readonly property color divider: Qt.rgba(text.r, text.g, text.b, 0.4)

    readonly property color neutral100: pick(_neutral100Light)
    readonly property color neutral200: pick(_neutral200Light)
    readonly property color neutral300: pick(_neutral300Light)
    readonly property color neutral400: pick(_neutral400Light)
    readonly property color neutral500: pick(_neutral500Light)
    readonly property color neutral600: pick(_neutral600Light)
    readonly property color neutral700: pick(_neutral700Light)
    readonly property color neutral800: pick(_neutral800Light)
    readonly property color neutral900: pick(_neutral900Light)

    readonly property color accent100: pick(_accent100Light)
    readonly property color accent200: pick(_accent200Light)
    readonly property color accent300: pick(_accent300Light)
    readonly property color accent400: pick(_accent400Light)
    readonly property color accent500: pick(_accent500Light)
    readonly property color accent600: pick(_accent600Light)
    readonly property color accent700: pick(_accent700Light)
    readonly property color accent800: pick(_accent800Light)
    readonly property color accent900: pick(_accent900Light)

    // ---- spacing and radius -------------------------------------------------
    // 4 / 8 / 12 / 16 / 24 / 32, per the handoff. Zero radius everywhere is
    // deliberate: do not round anything.
    readonly property int space1: 4
    readonly property int space2: 8
    readonly property int space3: 12
    readonly property int space4: 16
    readonly property int space6: 24
    readonly property int space8: 32
    readonly property int radius: 0

    // ---- elevation ------------------------------------------------------
    // Only the Preferences dialog uses one of these (not yet built). Ink-
    // tinted in light, ambient darkness in dark - both are just the shadow
    // colour at different alphas, since QML shadows have no colour role of
    // their own to invert.
    readonly property color shadowColor: dark ? Qt.rgba(0, 0, 0, 0.5)
                                              : Qt.rgba(0.176, 0.169, 0.169, 0.14)
    readonly property int shadowBlurSm: 2
    readonly property int shadowBlurMd: 10
    readonly property int shadowBlurLg: 32

    // ---- type -----------------------------------------------------------
    readonly property int fontTitle: 22
    readonly property int fontNormal: 14
    readonly property int fontSmall: 12

    // ---- legacy names --------------------------------------------------
    // Kept so Card.qml, ChannelMeter.qml, SoundfieldView.qml and the not-yet-
    // rebuilt Main.qml cards keep compiling and reading sensibly against the
    // new tokens without being touched in this change - the shell rebuild
    // (checkpoint 2) and the meter rework (checkpoint 4) retire these in
    // favour of the token names directly.
    readonly property color background: bg
    readonly property color surfaceAlt: neutral200
    readonly property color border: divider
    readonly property int gap: space3
    readonly property int pad: space4
    // color-mix(in srgb, var(--color-text) 55%, transparent), the CSS
    // ".text-muted" recipe.
    readonly property color textMuted: Qt.rgba(text.r, text.g, text.b, 0.55)
    // The colour drawn on an accent fill, e.g. a filled CLIP box's label -
    // .btn-primary in the CSS pairs an accent background with bg-coloured text.
    readonly property color accentText: bg
    // The new meter design (checkpoint 4) is a plain neutral-800 fill that
    // turns accent past -6 dBFS/clip, with no separate amber step - mapping
    // the old three-way ternary onto exactly those two colours now means
    // ChannelMeter.qml already reads that way without being touched here.
    readonly property color good: neutral800
    readonly property color warn: accent
    readonly property color bad: accent
}
