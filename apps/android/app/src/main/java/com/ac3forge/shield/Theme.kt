package com.ac3forge.shield

import android.graphics.Color

/**
 * The demo's one shared palette/spacing scale - [MainActivity], [RoomView],
 * and [ChannelMeterView] all draw their own chrome by hand (plain [View]s
 * and raw [android.graphics.Canvas] calls, no XML styles/themes.xml to
 * centralize this the normal Android way), so without a single shared
 * source every screen drifted its own slightly-different grey/blue - this
 * is that source. Values, not semantics: this file owns "what color is
 * accent," not "what does accent mean" - each call site still decides how
 * to use them.
 *
 * A dark, near-black base (this app runs on a TV in a room dim enough to
 * see a receiver's own front-panel display) with one consistent accent hue
 * for anything "live"/informational, so the stats readout, panel titles,
 * and speaker meter all read as one family instead of three separately-
 * tuned overlays.
 */
object Theme {
    val colorBackground = Color.rgb(8, 9, 11)
    val colorSurface = Color.rgb(22, 24, 28)
    val colorSurfaceBorder = Color.rgb(40, 44, 50)

    val colorTextPrimary = Color.rgb(238, 241, 245)
    val colorTextSecondary = Color.rgb(150, 158, 168)
    val colorTextMuted = Color.rgb(96, 102, 112)

    // The one accent hue for anything live/informational - stats readout,
    // panel titles, speaker meter bars. Previously each picked its own
    // similar-but-not-quite-matching blue.
    val colorAccent = Color.rgb(94, 190, 255)
    val colorAccentDim = Color.rgb(48, 92, 130)
    // The D-pad axis-mode readout's own hue - kept distinct from colorAccent
    // so "what mode am I in" and "what's the encoder doing" read as two
    // different kinds of information, not accidentally the same color.
    val colorWarn = Color.rgb(255, 179, 71)

    // One shared object-color source (RoomView previously owned this list
    // alone) - index 0 is always the interactive lead, matching
    // live_cursor.cpp's kInteractiveObjects/kObjects ordering.
    val objectColors = intArrayOf(
        Color.rgb(255, 99, 91),   // lead - warm coral/red, stands apart from the cool accent hue
        Color.rgb(84, 199, 130),  // ambient 1 - green
        Color.rgb(94, 170, 255),  // ambient 2 - blue
    )

    const val cornerRadiusLarge = 28f
    const val cornerRadiusSmall = 16f
    const val spacingUnit = 24f
}
