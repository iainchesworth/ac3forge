# Loading a source

The left rail — "the signal" — is where audio comes in. It has three cards, always visible
regardless of which tab is active on the right.

## Source

**Choose WAV…** opens a file picker; the chosen path (middle-elided) and a summary line appear
beneath it once loaded — sample rate, channel count, and duration:

![A 12-channel WAV loaded: 48000 Hz, 12 channels, 0:08](screenshots/loading-a-source-loaded.png)

The card reports the channel *count*, not a layout name — the output layout is chosen
independently on the [Format tab](format-and-channels.md) and need not match the source. A source
narrower than the chosen output layout leaves the missing channels silent; a wider one folds down
per §7.8 using the centre/surround downmix levels on the [Metadata tab](metadata.md).

## Live capture

A dropdown of capture endpoints — microphones and playback-device loopbacks, the system default
marked `[default]` — plus **Refresh** and a **Record…** button that becomes a highlighted **Stop**
while recording, with a live elapsed-time readout beneath it. A **Monitor** checkbox and device
picker, an **Also write to disk** checkbox, and a **Start live session…** button sit below that —
covered in full on [Live capture & session](live-session.md).

A capture endpoint feeds the encoder the same way a file does — same format, same layout, same
metadata — its channels are just routed onto whatever layout is selected on the Format tab, live,
instead of read from disk.

!!! note "Platform backend"
    Live capture needs the platform's audio backend (WASAPI on Windows, ALSA on Linux). See
    [Platform notes](../platforms/windows.md) for what's actually hardware-confirmed on each OS —
    the card reports itself unavailable on a build with no backend, rather than failing to load.

## Channel levels

Once a source is loaded, this card shows one meter row per **coded** channel — not per speaker —
named and ordered as A/52 Table 5.8 defines them, with a −60…0 dB tick scale and a soundfield view
(EAR LEVEL / CEILING) to the right:

![Channel meters populated after a run, 7.1.4 E-AC-3](screenshots/channel-levels-live.png)

A **Coded / Rendered** toggle switches between the coded-channel view above and what those
channels actually render to on playback — the two differ whenever a dependent substream replaces
part of the bed rather than adding to it (§E3.8.2), which is why a 7.1.4 layout meters 14 coded
channels for 12 speakers. During a run, a red dot and the word **live** replace the "peak and RMS
over the whole signal" label while metering updates in real time; once the run finishes, the bars
show the final peak/RMS for the whole file, with per-channel **CLIP** indicators.

A channel nothing feeds is drawn at reduced opacity and reads `-∞`, so "correctly silent" stays
distinguishable from "meter wired to nothing."

## Next

[Format & channels](format-and-channels.md) — choosing what this source actually gets encoded
into.
