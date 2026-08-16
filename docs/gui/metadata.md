# Metadata

Expert tier only — Advanced folds the Loudness half of this onto the
[Format tab](format-and-channels.md#codec-presets-bit-rate-container) instead and leaves the rest
at their defaults. Guided has no separate Loudness step of its own; instead it applies its own
[loudness contract](index.md#the-loudness-contract) automatically, unless the fields here have
already been edited by hand. Downmix, Heavy compression and Mixing metadata are Expert-only in
every tier.

![Metadata tab: Loudness, Downmix, Heavy compression, Mixing metadata](screenshots/metadata-tab.png)

## Loudness

- **DRC profile** — `none` plus five profiles (`film-standard`, `film-light`, `music-standard`,
  `music-light`, `speech`), §7.7.1.
- **dialnorm** — a 1–31 spin box, disabled by a **measure** checkbox that derives it instead from
  BS.1770-4 gated loudness over the whole programme (§5.4.2.8). Getting it wrong isn't cosmetic —
  a levelled playback system plays the difference.
- **DRC profile / dialnorm — programme 2** — appear only with a
  [`1+1` dual-mono bed](format-and-channels.md#dual-mono) selected. Ch1 and Ch2 are two unrelated
  programmes with no shared downmix to average anything across (§7.7.2.2), so each states its own
  DRC curve and dialogue level — programme 2's DRC is **not** inherited from programme 1's; a plan
  that wants both programmes compressed alike sets both explicitly. dialnorm2's **measure**
  checkbox works the same as dialnorm's — `dialnorm=auto`/`dialnorm2=auto` each measure their own
  programme's coded channel independently, never a blend of the two.

## Downmix

**Centre downmix** and **Surround downmix** dropdowns — Table 5.9 / Table 5.10 — control how a
wide source folds down to a narrower speaker layout.

## Heavy compression

A checkbox that reveals a **ceiling** spin box (in tenths of a dB, so the −0.5 dBFS default
survives) and a **dialogue** spin box — §7.7.2's peak-limited mono downmix, at syncframe
resolution. A second, identically-shaped **Heavy compression — programme 2** card appears beside
it under a `1+1` dual-mono bed, for the same reason DRC gets its own programme-2 copy above: Ch2's
own compr2 bounds Ch2's own signal, never Ch1's.

## Mixing metadata

E-AC-3 only. A checkbox reveals a preferred stereo downmix mode and an LFE mix level — the
`mixmdate` group, Table E1.2.

Every field on this tab maps directly onto the [Metadata](../library/metadata.md) library page's
config structs, and onto the [CLI's metadata options](../cli/metadata-options.md)
(`drc=`, `dialnorm=`, `cmixlev=`, `heavy`, `mixmeta`, …) — the values entered here and the tokens
on the command line are the same data, just two ways to set it.

## Next

[Objects & motion](objects-and-motion.md) — the Objects tab, where a plain bed becomes an Atmos
carrier.
