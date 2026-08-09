# Format & channels

The Format tab is the one tab always present, in both Basic and Advanced mode — it's where codec,
layout and bit rate get chosen.

## Codec, presets, bit rate, container

Four controls in a row: **Codec** (AC-3 / E-AC-3), a row of layout **presets** (5.1, 7.1, 5.1.4,
7.1.4, 5.2), **Bit rate**, and **Container** (elementary stream or Matroska). Presets are starting
points, not the model — they set the bed, LFE, and extras together, but the channel picker below
is what the encode plan actually reads:

![E-AC-3, 7.1.4 preset, all extras enabled](screenshots/format-eac3-714.png)

The plan strip above the tabs updates live: `E-AC-3 · 7.1.4 · 192 kbps · .ec3`, with a summary
(`12 speakers from 12 coded channels · 2 dependent substreams`) and a routing note (`The source
is already 7.1.4; every channel is carried straight through.`) beneath it.

## Channels: bed and extras

Two tiers, side by side:

1. **Bed — pick one.** Seven buttons: `1/0` through `3/2`, plus an LFE checkbox. This is the
   Table 5.8 layout every AC-3 and E-AC-3 stream carries no matter what else is added.
2. **Extras — added to the bed.** Six checkboxes, each Dolby Digital Plus only: front wide, rear
   surround, ceiling front, ceiling middle, ceiling rear, and a second LFE. Paired checkboxes
   toggle together (you can't add a left ceiling channel without its right pair). A budget counter
   (`12 of 16 channel positions used`) tracks how much of Table E2.5's channel space is spent.

Switching the codec to plain AC-3 disables every extra — Dolby Digital carries any of the seven
beds, with or without LFE, but no extras, which is why 3/2+LFE (5.1) is its widest layout.
Everything wider needs the dependent substreams that only E-AC-3 has — see
[Encoding E-AC-3](../library/encoding-eac3.md) for what that means underneath.

## Basic vs. Advanced on this tab

**Basic** adds a **Loudness** card here (DRC profile and dialnorm only — the rest of
[Metadata](metadata.md) lives on its own tab in Advanced). **Advanced** adds a
**Passthrough to a receiver** card instead, letting you send the encode straight to an IEC 61937
device once it's done:

![Advanced mode, post-run: full channel meters, Passthrough card, completed run in the strip](screenshots/format-advanced-postrun.png)

The passthrough device dropdown lists render endpoints and whether each can bitstream at all — an
E-AC-3 stream is refused there outright, since the packer only emits AC-3 bursts (data type 1).
See [Live capture & session](live-session.md) for the live equivalent of this card, and
[Platform notes](../platforms/windows.md) for which platforms have this hardware-confirmed.

## Next

- [Coding tools](coding-tools.md) — Annex E tools, Advanced + E-AC-3 only.
- [Metadata](metadata.md) — the rest of the loudness/downmix picture, Advanced only.
- [Objects & motion](objects-and-motion.md) — turning this same bed into an Atmos carrier.
