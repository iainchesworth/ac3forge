import QtQuick
import QtTest

import Ac3Forge

// The palette machinery: selectable palettes with hand-tuned light AND dark
// ramps (dark used to be a mechanical HSL inversion of light, which is why
// it looked like a black-red motif), and a "system" palette deriving its
// accent ramp from the desktop's own accent colour (SystemTheme).
TestCase {
    id: testCase
    name: "ThemePalettes"
    when: windowShown

    Component {
        id: mainWindowComponent
        Main {}
    }

    // Theme is a SINGLETON shared with every other test file - leave it as
    // the suite found it, whatever this file did.
    function init() {
        Theme.preference = "light";
        Theme.paletteChoice = "signal";
    }
    function cleanup() {
        Theme.preference = "system";
        Theme.paletteChoice = "signal";
    }

    function test_palettesSwapEveryTokenFamily() {
        const signalAccent = String(Theme.accent);
        const signalSurface = String(Theme.accent100);
        const signalNeutral = String(Theme.neutral800);

        Theme.paletteChoice = "ink";
        verify(String(Theme.accent) !== signalAccent);
        verify(String(Theme.accent100) !== signalSurface);
        // Neutrals swap too - ink's greys are cool where signal's are warm.
        verify(String(Theme.neutral800) !== signalNeutral);

        Theme.paletteChoice = "console";
        verify(String(Theme.accent) !== signalAccent);

        // An unknown name falls back to signal rather than a broken palette.
        Theme.paletteChoice = "no-such-palette";
        compare(String(Theme.accent), signalAccent);
    }

    function test_darkIsHandTunedNotAnInversion() {
        // The old derivation mirrored 1-L, so light's near-white accent-100
        // (#fff2ef, L≈0.97) became a near-black L≈0.03 stain. The hand-tuned
        // dark ramp keeps 100 as a SURFACE tint: well away from both black
        // and the light value.
        // Captured as PRIMITIVES on purpose: a `const c = Theme.accent100`
        // is a live value-type REFERENCE in Qt 6 - it re-reads the property
        // after the theme flips, and both sides of the comparison end up
        // equal by construction.
        Theme.preference = "light";
        const light100 = String(Theme.accent100);
        Theme.preference = "dark";
        const dark100 = String(Theme.accent100);
        const dark100Lightness = Theme.accent100.hslLightness;

        verify(dark100 !== light100);
        verify(dark100Lightness > 0.05);   // not the inversion's near-black
        verify(dark100Lightness < 0.35);   // still a dark-mode surface
        // And the dark background is a hand-picked near-black under
        // near-white text - the readable pairing the inversion only ever
        // approximated.
        verify(Theme.bg.hslLightness < 0.15);
        verify(Theme.text.hslLightness > 0.8);
    }

    function test_systemPaletteDerivesItsRampFromTheDesktopAccent() {
        Theme.paletteChoice = "system";

        // Whatever colour the platform supplies (offscreen gives Qt's
        // fallback), the derived ramp must be usable: a real accent, and a
        // ramp ordered 100 (near the background) to 900 (far from it).
        verify(Theme.accent.a === 1);
        verify(String(Theme.accent100) !== String(Theme.accent900));
        Theme.preference = "light";
        verify(Theme.accent100.hslLightness > Theme.accent900.hslLightness);
        Theme.preference = "dark";
        verify(Theme.accent100.hslLightness < Theme.accent900.hslLightness);

        // The hue is the desktop's own (when the desktop colour carries any
        // saturation to have a hue at all).
        const base = SystemTheme.accentColor;
        if (base.hslSaturation > 0.05) {
            const baseHue = base.hslHue < 0 ? 0 : base.hslHue;
            const accentHue = Theme.accent.hslHue < 0 ? 0 : Theme.accent.hslHue;
            verify(Math.abs(accentHue - baseHue) < 0.02
                   || Math.abs(accentHue - baseHue) > 0.98);   // hue wraps
        }
    }

    function test_prefsPaletteRoundTripsAndAppliesOnSave() {
        const win = createTemporaryObject(mainWindowComponent, testCase);
        verify(win !== null);

        win.prefsDialog.open();
        let saveButton = null;
        tryVerify(() => {
            saveButton = findChild(win.contentItem, "prefsSaveButton");
            return saveButton !== null && saveButton.visible;
        });
        win.prefsDialog.paletteChoice = "ink";
        saveButton.clicked();
        compare(win.settings.palette, "ink");
        // onApplied pushes it into the live Theme, same as the theme mode.
        compare(Theme.paletteChoice, "ink");

        // Cancel is a real cancel for the palette too.
        win.prefsDialog.open();
        let cancelButton = null;
        tryVerify(() => {
            cancelButton = findChild(win.contentItem, "prefsCancelButton");
            return cancelButton !== null && cancelButton.visible;
        });
        win.prefsDialog.paletteChoice = "console";
        cancelButton.clicked();
        compare(win.settings.palette, "ink");

        win.settings.palette = "signal";
        Theme.paletteChoice = "signal";
    }
}
