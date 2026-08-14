pragma Singleton

import QtQuick

import Ac3Forge

// Single source of truth for colour, spacing and type, so every page and
// component stays visually consistent. Tokens follow the Modernist design
// system: zero radius everywhere, an Archivo type scale, and colour ramps in
// two families (neutral and accent), each 100 (nearest the background) to
// 900 (highest contrast against it).
//
// There are several PALETTES now, each defining BOTH modes by hand. Dark
// used to be derived by mechanically inverting the light ramp's HSL
// lightness, and it looked like it: near-white accent tints inverted into
// murky red-blacks and the fully saturated accent stayed glaring against
// near-black. A dark palette needs raised lightness floors, softened
// saturation and warm (not pure-black) surfaces - colour choices, not
// arithmetic - so each palette carries a hand-tuned dark ramp instead.
//
// The "system" palette takes its accent from the DESKTOP's own accent
// colour (SystemTheme.accentColor - QPalette::Accent, which Qt's platform
// themes fill natively on Windows/macOS/KDE and fall back to Highlight
// elsewhere) and derives its accent ramp from that one colour; its
// neutrals are the pure-grey set, so any hue the OS supplies sits cleanly.
//
// Every component binds to a token name (Theme.neutral800, Theme.accent...)
// and gets whichever palette and mode are active; nothing outside this file
// knows how many palettes there are, and nothing outside this file may know
// either.
QtObject {
    id: theme

    // "system" follows the OS colour scheme; Preferences overrides and
    // persists it (appSettings.theme).
    property string preference: "system"
    readonly property bool dark: preference === "dark"
                                  || (preference === "system"
                                      && Application.styleHints.colorScheme === Qt.Dark)

    // Which palette draws the app - "signal" (the design system's red),
    // "ink" (cool blue), "console" (studio amber) or "system" (the desktop
    // accent). Preferences overrides and persists it (appSettings.palette).
    property string paletteChoice: "signal"

    // ---- the palettes ------------------------------------------------------
    // Order inside each mode: bg, surface, text, accent, then the two ramps.
    // 100 sits nearest the background in BOTH modes (a tint in light, a deep
    // shade in dark) and 900 farthest, so a component's rung choice keeps
    // its contrast intent whichever mode is active.
    readonly property var _palettes: ({
        signal: {
            light: {
                bg: "#f3f2f2", surface: "#eae9e9", text: "#201e1d", accent: "#ec3013",
                n100: "#f8f4f4", n200: "#eae7e7", n300: "#d7d3d3", n400: "#bab6b6",
                n500: "#9b9797", n600: "#7d7979", n700: "#605d5d", n800: "#444141",
                n900: "#2d2b2b",
                a100: "#fff2ef", a200: "#ffe0d9", a300: "#ffc4b8", a400: "#ff9783",
                a500: "#ff563c", a600: "#dd2b0f", a700: "#ae1800", a800: "#7c1405",
                a900: "#4d170e"
            },
            dark: {
                bg: "#151313", surface: "#1d1a1a", text: "#ece7e5", accent: "#ff5c40",
                n100: "#1c1919", n200: "#272423", n300: "#353130", n400: "#4b4645",
                n500: "#6b6563", n600: "#8b8583", n700: "#aba4a1", n800: "#cfc8c5",
                n900: "#e8e2df",
                a100: "#2c150f", a200: "#3e1c14", a300: "#58261b", a400: "#8c3520",
                a500: "#cc4629", a600: "#ff5c40", a700: "#ff8368", a800: "#ffac9a",
                a900: "#ffd5cb"
            }
        },
        ink: {
            light: {
                bg: "#f4f4f5", surface: "#ebebed", text: "#1c1d20", accent: "#2f54d0",
                n100: "#f7f7f8", n200: "#e9e9eb", n300: "#d5d5d8", n400: "#b7b7bb",
                n500: "#98989d", n600: "#7a7a80", n700: "#5d5d63", n800: "#414147",
                n900: "#2b2b30",
                a100: "#eef2fe", a200: "#dbe4fd", a300: "#bccdfa", a400: "#8aa5f2",
                a500: "#4f74e4", a600: "#2f54d0", a700: "#1f3ca6", a800: "#1a2f7c",
                a900: "#172452"
            },
            dark: {
                bg: "#131417", surface: "#1a1c20", text: "#e7e9ee", accent: "#7d9bff",
                n100: "#191b1f", n200: "#23262b", n300: "#31343a", n400: "#464a52",
                n500: "#666b74", n600: "#868b94", n700: "#a6abb4", n800: "#cbcfd6",
                n900: "#e4e7ec",
                a100: "#161d33", a200: "#1d2745", a300: "#263662", a400: "#35529e",
                a500: "#5375d6", a600: "#7d9bff", a700: "#9db4ff", a800: "#becdff",
                a900: "#dde5ff"
            }
        },
        console: {
            light: {
                bg: "#f5f3ef", surface: "#ece9e3", text: "#211e18", accent: "#b25e00",
                n100: "#f8f6f2", n200: "#ebe8e2", n300: "#d8d4cc", n400: "#bbb6ac",
                n500: "#9c978d", n600: "#7e7970", n700: "#615d55", n800: "#45413b",
                n900: "#2e2b26",
                a100: "#fdf3e4", a200: "#fae3c2", a300: "#f4cb90", a400: "#e8a54d",
                a500: "#cc7d14", a600: "#b25e00", a700: "#8a4700", a800: "#613300",
                a900: "#3d2100"
            },
            dark: {
                bg: "#161311", surface: "#1e1a17", text: "#ece7df", accent: "#f0a03c",
                n100: "#1c1815", n200: "#272220", n300: "#35302c", n400: "#4b4540",
                n500: "#6b655e", n600: "#8b857d", n700: "#aba49b", n800: "#cfc8bf",
                n900: "#e8e1d8",
                a100: "#2b1e0d", a200: "#3c2a12", a300: "#573d1a", a400: "#8c6122",
                a500: "#c9882d", a600: "#f0a03c", a700: "#ffb765", a800: "#ffd097",
                a900: "#ffe7c7"
            }
        }
    })

    // The system palette's accent ramp, derived from whatever single colour
    // the desktop supplies. Guardrails, not arithmetic inversion: lightness
    // rungs are FIXED per mode (so the ramp's contrast intent holds for any
    // hue) and saturation is clamped, softened near the background where a
    // strong chroma would read as a stain.
    function _systemAccentRamp(base, wantDark) {
        const h = base.hslHue < 0 ? 0 : base.hslHue;
        const s = Math.max(0.25, Math.min(base.hslSaturation, 0.95));
        function at(lightness, satScale) {
            return Qt.hsla(h, Math.min(s * satScale, 0.95), lightness, 1.0);
        }
        if (wantDark) {
            return {
                accent: at(Math.min(Math.max(base.hslLightness, 0.58), 0.68), 0.9),
                a100: at(0.13, 0.6), a200: at(0.17, 0.65), a300: at(0.24, 0.7),
                a400: at(0.36, 0.8), a500: at(0.50, 0.9), a600: at(0.60, 0.9),
                a700: at(0.70, 0.9), a800: at(0.80, 0.85), a900: at(0.89, 0.8)
            };
        }
        return {
            accent: at(Math.min(Math.max(base.hslLightness, 0.32), 0.50), 1.0),
            a100: at(0.96, 0.8), a200: at(0.90, 0.8), a300: at(0.80, 0.85),
            a400: at(0.66, 0.95), a500: at(0.52, 1.0), a600: at(0.42, 1.0),
            a700: at(0.33, 1.0), a800: at(0.24, 1.0), a900: at(0.16, 1.0)
        };
    }

    // The active token set. The system palette wears ink's neutrals (the
    // pure-grey set - any OS hue sits cleanly on them) under the derived
    // accent ramp.
    readonly property var _active: {
        if (paletteChoice === "system") {
            const greys = dark ? _palettes.ink.dark : _palettes.ink.light;
            const ramp = _systemAccentRamp(SystemTheme.accentColor, dark);
            return {
                bg: greys.bg, surface: greys.surface, text: greys.text,
                accent: ramp.accent,
                n100: greys.n100, n200: greys.n200, n300: greys.n300,
                n400: greys.n400, n500: greys.n500, n600: greys.n600,
                n700: greys.n700, n800: greys.n800, n900: greys.n900,
                a100: ramp.a100, a200: ramp.a200, a300: ramp.a300,
                a400: ramp.a400, a500: ramp.a500, a600: ramp.a600,
                a700: ramp.a700, a800: ramp.a800, a900: ramp.a900
            };
        }
        const chosen = _palettes[paletteChoice] || _palettes.signal;
        return dark ? chosen.dark : chosen.light;
    }

    // ---- resolved tokens ---------------------------------------------------
    readonly property color bg: _active.bg
    readonly property color surface: _active.surface
    readonly property color text: _active.text
    readonly property color accent: _active.accent
    // Text at 40% alpha, same recipe as the CSS token: always legible
    // against whatever "text" resolves to this mode, because it IS that
    // colour.
    readonly property color divider: Qt.rgba(text.r, text.g, text.b, 0.4)

    readonly property color neutral100: _active.n100
    readonly property color neutral200: _active.n200
    readonly property color neutral300: _active.n300
    readonly property color neutral400: _active.n400
    readonly property color neutral500: _active.n500
    readonly property color neutral600: _active.n600
    readonly property color neutral700: _active.n700
    readonly property color neutral800: _active.n800
    readonly property color neutral900: _active.n900

    readonly property color accent100: _active.a100
    readonly property color accent200: _active.a200
    readonly property color accent300: _active.a300
    readonly property color accent400: _active.a400
    readonly property color accent500: _active.a500
    readonly property color accent600: _active.a600
    readonly property color accent700: _active.a700
    readonly property color accent800: _active.a800
    readonly property color accent900: _active.a900

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
    // Ink-tinted in light, ambient darkness in dark - both are just the
    // shadow colour at different alphas, since QML shadows have no colour
    // role of their own to swap per palette.
    readonly property color shadowColor: dark ? Qt.rgba(0, 0, 0, 0.5)
                                              : Qt.rgba(0.176, 0.169, 0.169, 0.14)
    readonly property int shadowBlurSm: 2
    readonly property int shadowBlurMd: 10
    readonly property int shadowBlurLg: 32

    // ---- type -----------------------------------------------------------
    readonly property int fontTitle: 22
    readonly property int fontNormal: 14
    readonly property int fontSmall: 12

    // The handoff's faces, resolved against what this machine actually has:
    // Archivo (weight 800 headings) with the platform UI face as the
    // fallback, and a fixed-width face for every numeric readout so digits
    // do not shift as they count. Resolved once - Qt.fontFamilies() is a
    // full enumeration and has no business running per binding.
    readonly property string headingFamily: {
        const installed = Qt.fontFamilies();
        return installed.indexOf("Archivo") >= 0 ? "Archivo" : Application.font.family;
    }
    readonly property string monoFamily: {
        const installed = Qt.fontFamilies();
        if (installed.indexOf("Cascadia Mono") >= 0) return "Cascadia Mono";
        if (installed.indexOf("Consolas") >= 0) return "Consolas";
        return "monospace";
    }

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
