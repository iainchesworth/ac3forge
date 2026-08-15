# Commands

The full, real usage text — copied verbatim from a build of `ac3cli`, not retyped:

```text
ac3forge — clean-room AC-3 / E-AC-3 (ATSC A/52) encoder/decoder

Usage:
  ac3cli --version    print version and git provenance, then exit
  ac3cli silence      <out.ac3> [seconds] [bitrate_kbps]
  ac3cli sine         <out.ac3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]
  ac3cli orbit        <out.ac3> [seconds] [bitrate_kbps] [orbit_seconds]
  ac3cli atmos        <out.ec3> [seconds] [bitrate_kbps] [objects] [orbit_seconds] [mode]
  ac3cli atmos-path   <out.ec3> <paths.txt> [seconds] [bitrate_kbps] [objects] (objects driven by an authored keyframe file instead of the built-in orbit)
  ac3cli atmos-encode <in.wav> <out.ec3> [bitrate_kbps] [objects] [paths.txt] (every source channel as an object; optional: authored per-object motion from a keyframe file (same format as atmos-path), objects it doesn't mention keep their default placement)
  ac3cli record       <out.ac3> [seconds] [bitrate_kbps] [device_index]
  ac3cli live         <out.ac3|out.ec3> <capture_device> [seconds] [bitrate_kbps] [monitor_device] [passthrough_device] [mode] (capture -> encode -> live monitor and/or passthrough)
  ac3cli encode       <in.wav> <out.ac3> [bitrate_kbps] [layout] [in2.wav] (in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= for more than one source)
  ac3cli eac3-silence <out.ec3> [seconds] [bitrate_kbps] [layout]
  ac3cli eac3-sine    <out.ec3> [seconds] [bitrate_kbps] [freq_hz] [amp_pct] [layout]
  ac3cli eac3-encode  <in.wav> <out.ec3> [bitrate_kbps] [tools] [layout] [vbr] [in2.wav] (in2.wav: layout 1+1's Ch2, when Ch1 is a separate mono file; or use src=/map= for more than one source)
  ac3cli decode       <in.ac3|in.ec3> <out.wav>               (AC-3 or E-AC-3; bsid decides)
  ac3cli levels       <in.wav|in.ac3|in.ec3>                  (per-channel peak/RMS report)
  ac3cli loudness     <in.wav>                                (BS.1770-4 loudness -> dialnorm)
  ac3cli spdif        <in.ac3> <out.wav>                      (IEC 61937 wrap as playable PCM16 WAV)
  ac3cli mkv          <in.ac3|in.ec3> <out.mkv>               (wrap as a playable Matroska file)
  ac3cli devices                                              (input and loopback capture endpoints)
  ac3cli outputs                                              (render endpoints + AC-3/E-AC-3 passthrough support)
  ac3cli play         <in.ac3|in.ec3> [device_index]          (exclusive-mode IEC 61937 passthrough; bsid decides AC-3 vs E-AC-3)
  ac3cli monitor      <in.ac3|in.ec3> [device_index]          (decode and play on an ordinary (non-bitstreamed) output)
```

## By category

### Synthesis — generate a stream from nothing

No source file needed; useful for smoke-testing a build or a receiver without recording
anything first.

| Command | What it writes |
|---|---|
| `silence` | Silent AC-3 |
| `sine` | A tone, one per speaker, AC-3. Append `c` to `[layout]` (e.g. `stereoc`) to turn on channel coupling. |
| `orbit` | AC-3 with a synthetic panned source circling the room (exercises the [spatial layer](../library/spatial-and-atmos.md) — plain bed panning, no object metadata) |
| `atmos` | E-AC-3 with synthetic orbiting Atmos objects — a 5.1 bed plus JOC + OAMD side data (TS 103 420) |
| `atmos-path` | Same, but object motion comes from an authored keyframe file instead of the built-in orbit |

```bash
ac3cli eac3-sine out.ec3 5 384 1000 50 714
```

Five seconds of a 1 kHz tone at 50% amplitude, 384 kbps, one tone per speaker across all 14
coded channels of a 7.1.4 layout.

### File encoding — real audio in, a stream out

| Command | What it does |
|---|---|
| `encode` | WAV → AC-3. Without `[layout]`, follows the source channel count (1→mono, 2→stereo, 3–6→5.1, 8→7.1, 10→5.1.4, 12→7.1.4). |
| `eac3-encode` | WAV → E-AC-3, with the Annex E `tools:` token and an optional `vbr:` token available (see [Metadata options](metadata-options.md)) |
| `atmos-encode` | WAV → E-AC-3 Atmos, every source channel becomes its own object; optional `[paths.txt]` drives per-object motion from an authored keyframe file the same way `atmos-path` does, keyed by WAV channel index — an object it doesn't mention keeps its default (fanned-out) placement |

```bash
ac3cli encode in.wav out.ac3 448 couple
```

448 kbps, channel coupling on, layout inferred from the WAV's channel count.

`1+1` (dual mono — two independent programmes, never inferred from a channel count, so it always
has to be named explicitly) takes its two channels either as one two-channel file or as two mono
ones:

```bash
ac3cli encode both.wav out.ac3 192 1+1                    # Ch1/Ch2 = channels 0/1 of both.wav
ac3cli encode narration_en.wav out.ac3 192 1+1 narration_fr.wav  # Ch1, Ch2 as separate files
```

See [Metadata options](metadata-options.md) for `dialnorm2=` — Ch2's own dialnorm, alongside the
usual `dialnorm=`.

`encode`, `eac3-encode` and `atmos-encode` all take `-` in place of `<in.wav>` or the output path
to mean stdin or stdout, so a pipeline never has to touch a temporary file:

```bash
ac3cli encode - - 448 couple < in.wav > out.ac3
```

The status text these commands normally print (frame count, routing, per-channel levels) goes to
stderr instead of stdout whenever the output side is `-`, so it never ends up inside the piped
stream.

### Decoding & inspection

| Command | What it does |
|---|---|
| `decode` | AC-3 or E-AC-3 → WAV; `bsid` in the stream decides which decoder runs |
| `levels` | Per-channel peak/RMS report — takes a WAV or an encoded stream |
| `loudness` | BS.1770-4 gated loudness on a WAV, reported as the `dialnorm` it implies |

```bash
ac3cli decode out.ec3 out.wav
```

`decode` takes `-` for either path too, the same convention the encoding commands above use:

```bash
ac3cli decode - - < out.ac3 > out.wav
```

### Containers

| Command | What it does |
|---|---|
| `spdif` | Wraps AC-3 as IEC 61937 bursts inside a playable PCM16 WAV — for feeding a receiver through an ordinary audio path |
| `mkv` | Wraps AC-3 or E-AC-3 as Matroska, reading format/packet boundaries/sample rate/channel count from the bitstream itself so the container can't be told the wrong ones |

### Live & hardware

Needs the platform's capture/passthrough backend — see [Platform notes](../platforms/windows.md)
for what's actually confirmed against real hardware on each OS.

| Command | What it does |
|---|---|
| `devices` | Lists capture endpoints (microphones, playback-device loopbacks) |
| `outputs` | Lists render endpoints and whether each supports AC-3/E-AC-3 passthrough |
| `record` | Captures from a device straight to an AC-3 file, metering live |
| `play` | Exclusive-mode IEC 61937 passthrough of an existing file — `bsid` decides AC-3 vs. E-AC-3 |
| `monitor` | Decodes an existing file and plays it on an ordinary, non-bitstreamed output — the shared-mode preview path. For an Atmos-mode stream, this plays the 5.1 **bed**: the in-repo decoder's E-AC-3 scope is A/52 Annex E syntax, not TS 103 420's object layer, so this is what a legacy decoder hears, not unmixed objects. |
| `live` | Capture → encode → optional live monitor and/or IEC 61937 passthrough, running continuously, still writing the file `record` always has; optionally a second, clock-conformed capture device via `capture2=` |

`live`'s device arguments: `monitor_device`/`passthrough_device` take `-2` (default, leaves that
leg off), `-1` (the default render endpoint), or an index from `outputs`. Either or both legs may
run alongside the file `live` always writes.

`live mode` (also shared with `atmos`): `channels` (default) carries stereo straight through;
`atmos` pans every captured channel into a 5.1 bed as its own object, moving it every frame the
same way `atmos`'s synthetic orbit does — the hook a real live position source drops into once
one exists.

`live capture2=<index>`: the `capture_device` positional stays the session's clock master, paced
exactly as it always has been; `capture2=` adds a second, independently-clocked device (see
[Metadata options](metadata-options.md#live-options-live-capture2) for the full grammar) whose
stream is resampled to track the master, with the measured drift printed at session end.

## Next

[Metadata options](metadata-options.md) — the options every encoding command in this table
accepts after its positional arguments, plus the full `layout`, `tools:` and `vbr` grammars.
