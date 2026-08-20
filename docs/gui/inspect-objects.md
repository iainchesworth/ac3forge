# Inspect objects

The decode-side counterpart to [QC a stream](qc.md), and to this window's own Objects tab. Objects
& motion (see [Objects & motion](objects-and-motion.md)) *authors* a plan still to come — where an
object should sit, how it should move. This dialog does the opposite: it opens an
**already-encoded** `.ec3` file and shows what Dolby Atmos object metadata (OAMD) and per-object
audio (JOC) the decoder actually recovered from it, with no source, no plan and no encoder involved
anywhere in the path — the same "distinct surface, reachable from the header" shape
[QC a stream](qc.md#opening-it) already uses, for the same reason.

## Opening it

**Inspect objects…**, in the header beside **QC a stream…** and **Preferences**. It opens
regardless of what (if anything) is loaded in the main workbench.

**Choose file…** opens a standard file picker filtered to `*.ac3`/`*.ec3` (plus **All files**).
Picking one starts the decode immediately, off the window's own event loop so the dialog stays
responsive while a long file decodes. An AC-3 file (`bsid` ≤ 8) is refused with a plain explanation
— object audio is an E-AC-3/Annex E tool only, so a plain AC-3 stream has nothing here to show. An
E-AC-3 file that decodes fine but carries no OAMD at all (an ordinary bed, no Atmos) is refused the
same way rather than shown as an empty report.

## The report

Once a file decodes, the dialog fills with:

- **A summary line** — codec, object shape, sample rate, decoded-frame count and duration, e.g.
  `E-AC-3 · 3 dynamic object(s) + LFE · 48000 Hz · 62 frame(s) · 1.98 s`.
- **A scrub bar** — **Play**/**Pause** and a slider across every access unit that carried OAMD.
  Scrubbing (or playing) moves which frame's positions the room views and the object list below
  show. This is literal decoded data, one snapshot per frame — there is no authored-path
  interpolation between frames the way the Objects tab's own path preview has, because nothing here
  was ever authored.
- **Room — plan and elevation** — the same two views, drawn in the same room-anchored coordinate
  system (§4.2.1: `x`/`y` in `[0, 1]`, `z` in `[-1, 1]`) and the same layout language as the Objects
  tab's own **ROOM — PLAN**/**ROOM — ELEVATION** (see [Objects & motion](objects-and-motion.md)), so
  a reader who already knows that view reads this one for free. The one difference is interaction:
  that view drags a marker to *place* an object; this one only ever plays back what the stream
  *already says*, so there is nothing to drag. Each decoded object is a numbered dot; the one
  currently being auditioned (below) is highlighted.
- **Object list** — one row per dynamic object: its live `x`/`y`/`z` and gain (dB) at the current
  scrub frame, and an **Audition** button.

## Audition

Each object's own **Audition** button plays that object's JOC-reconstructed audio through an
ordinary output — the same shared-mode playback path (`ac3::audio::MonitorSink`) the Objects tab's
own motion preview uses — so it can be judged by ear, not just by position. Only one object
auditions at a time; **Audition** on the row already playing becomes **Stop**. This is a parametric
reconstruction, not the original source audio recovered losslessly — see
[Spatial audio & Dolby Atmos](../library/spatial-and-atmos.md) for what JOC can and cannot pull
apart.

## What it does not do

This reads a stream that already exists; it has no connection to the workbench's own encode plan,
the Objects tab, or the run strip. There is no way to jump from a finished Atmos run's own run chip
straight into this dialog yet — open the file it wrote like any other.
