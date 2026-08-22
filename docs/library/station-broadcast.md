# A worked scene: the station broadcast

[`examples/station_broadcast.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/station_broadcast.cpp)
is the library's fully worked Atmos example: a complete 115-second scene —
synthesis, motion, mix, and encode — in one compiled program with no binary
assets. Everything the other pages introduce piecemeal appears here doing a
job: `KeyframePath` flies the ships, `ObjectPlacement` gains perform the edit,
`lfe_send` opens the wormhole, and the same scene is encoded twice, with the
EMDF container and without it.

The premise is borrowed lovingly from the title sequence of *Star Trek:
Deep Space Nine* — and from the fan edit that re-mixed its theme as if the
music were coming *from* the station. Every sound is diegetic: the anthem is
a radio broadcast from the station, and everything else that happens in the
scene happens *to* that broadcast.

## The cue sheet

Times are scene-relative. (Against the reference videos: the original title
sequence starts at 0:01, the diegetic fan edit at about 0:14.)

| Cue | Scene | Object work |
|---|---|---|
| 0:00 | A comet drifts across the black, left to right | `x` 0.02 → 0.98 across the front of the room |
| 0:02 | The station's broadcast fades up | Far front-centre (0.5, 0.04), gain 0.05 — tinny, distant, mono |
| 0:14 | The station pans into view | Gain steps to 0.30 as the picture finds the source |
| 0:26 | Cargo jalopy flypast | Two co-moving objects (engine + its own radio) cross from rear-right; at closest approach the radio's 0.75 drowns the station's 0.30 |
| 0:38 | Cut to a station close-up | The broadcast's bandpass opens to nearly clean and its gain doubles — same object, the *mix* performs the edit |
| 0:52 | A runabout undocks and sweeps overhead | `z` reaches 0.65: real height metadata for a renderer with tops; the 5.1 bed folds it onto the ring |
| 1:01 | The anthem surges back for the reprise | The cue's own measured form: full restatement at 1:01, climax plateau from 1:19 |
| 1:12 | A second runabout crosses the port side | Rear-left to a front-left docking |
| 1:24 | A maintenance pod welds on an upper pylon | Static object, front-right, raised |
| 1:43 | The wormhole opens behind the station | A subsonic core almost entirely on `lfe_send` — the only route to the LFE — plus two shimmer objects that split and wrap up and over the room; the score's final cadence lands on the flash at 1:44, as in the original |
| 1:51 | ...and swallows itself | Pitch and gains collapse back to front-centre |

Ten objects, one authored `KeyframePath` each, evaluated at the *end* of each
frame — the same convention as `ac3cli atmos-path`, because both metadata
layers interpolate to that point.

## What it adds beyond `atmos_objects.cpp`

- **Authored motion, not orbits.** Every path is a hand-written keyframe
  table; distance lives in the authored gains, so the "swamping" of the
  broadcast by a passing ship is just two gain arcs crossing.
- **Doppler, outside the library.** The example derives radial velocity from
  its own paths and bends its oscillators before the samples ever reach the
  encoder. The library deliberately has no opinion here: essences are audio,
  placement is metadata.
- **Both fallback strategies, side by side.** The same scene is encoded with
  objects (`out.ec3`) and with `emit_object_metadata = false`
  (`out_bed51.ec3`). The 5.1 mix is identical; the second file exists for
  decoders that validate `emdf_protection` and refuse the whole stream
  rather than fall back — see the
  [two honest limitations](../concepts/atmos-joc.md#two-honest-limitations).
- **The bed as a deliverable.** `encoder.bed()` — the thing a legacy decoder
  hears — is captured per frame and written as `out_bed.wav`, plus an Lo/Ro
  fold-down `out_stereo.wav` for headphones.

## Running it

```
station_broadcast                          # smoke test: 0:24–0:34 in memory
station_broadcast out [seconds] [wav]      # full render, writes four files
```

To listen immediately: `out_stereo.wav` in any player, or the E-AC-3 through
FFmpeg (`ffplay out_bed51.ec3`). Consumer Dolby hardware should be fed the
`bed51` variant for the reason above.

The built-in anthem is a **synthesizer cover** of the *Deep Space Nine* main
title (Dennis McCarthy) — the seasons 1–3 arrangement, made by ear in C major
for the example's own voices: the solo horn call over a pad, the solo-trumpet
fanfare, and a final open-fifth cadence timed, as in the original, to the
wormhole. The middle section has never been transcribed anywhere, so it is
recomposed from the theme's own material. Note well: distributing a cover
requires the appropriate licence wherever *you* distribute it — this
repository's author has secured their own position, and forks must consider
theirs. The third argument replaces the built-in cover with a WAV you have
the rights to; it is mixed to mono, resampled, and played from scene time
0:02 through the same transmitter chain, so the 0:38 bandwidth-opening cut
still happens to *your* recording.

---

See also: [Spatial & Atmos objects](spatial-and-atmos.md) — the API this
scene is built on; [Atmos & JOC](../concepts/atmos-joc.md) — what survives
of the object layer, and where.
