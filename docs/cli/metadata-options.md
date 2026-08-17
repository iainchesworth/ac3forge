# Options & grammars

Encoding commands in [Commands](commands.md) take these after their positional arguments, in any
order. Not every command honors every option, though the parser accepts them anywhere: `silence`
takes none at all; `record` and `live` honor only `fast-mdct=off`, `container=` and (`live`
only) `capture2=`, and accept but ignore the metadata options (`drc=`, `dialnorm=`, `heavy`,
`cmixlev=`, …); `atmos`, `atmos-path` and `atmos-encode` all apply `dialnorm=<n>`, `fast-mdct=off`
and the object-signing flags below, and `dialnorm=auto` is silently inert on `atmos`/`atmos-path`
— of the three Atmos commands, only `atmos-encode` measures:

```text
metadata options (any order, after the positional arguments):
  drc=<profile>     §7.7.1 dynamic range control per block
                    film-standard | film-light | music-standard | music-light | speech
  heavy             §7.7.2 heavy compression: a peak ceiling in the
                    mono downmix, at syncframe resolution
  ceiling=<dBFS>    that ceiling (default -0.5)
  dialogue=<dBFS>   where heavy compression puts dialogue (default -20)
  drc2=<profile>    Ch2's own DRC profile, layout 1+1 only (§7.7.1) - not
                    inherited from drc=, set both to compress both programmes alike
  heavy2            Ch2's own heavy compression, layout 1+1 only (§7.7.2.2)
  ceiling2=<dBFS>   that ceiling for Ch2 (default -0.5)
  dialogue2=<dBFS>  where Ch2's heavy compression puts dialogue (default -20)
  dialnorm=auto     measure BS.1770 loudness and derive dialnorm (§5.4.2.8)
  dialnorm=<1..31>  set it directly (default 31)
  dialnorm2=auto | <1..31>   Ch2's own dialnorm, layout 1+1 only (§5.4.2.16, default 31)
  cmixlev=-3|-4.5|-6      centre downmix level (Table 5.9)
  surmixlev=-3|-6|off     surround downmix level (Table 5.10)
  mixmeta           E-AC-3 only: emit the mixmdate group (Table E1.2)
  lfemix=<0..31>|off      E-AC-3 LFE mix level, 10-code dB (§E2.3.1.11)
  dmixmod=ltrt|loro|none  preferred stereo downmix (Table D2.2)
  couple            enable channel coupling - honored by 'encode' and 'sine' ('sine' can also
                    spell it as a 'c' layout suffix); E-AC-3 coupling is the tools argument's
                    cpl token instead
  keep-partial      encode/eac3-encode/atmos-encode: if the run fails partway, keep whatever
                    frames were already encoded (named beside the intended output as
                    <name>.partial.<ext>) instead of discarding them - off by default, matching
                    the GUI's own keep-partial-output preference
  fast-mdct=off     force the direct §8.2.3.2 forward MDCT instead of the default §7.9.4 fast
                    path (identical streams to within ~3e-12 max relative coefficient error;
                    the direct form is the validation oracle) - applies wherever this command
                    encodes, incl. atmos/record/live/eac3-sine/eac3-encode; eac3-encode's
                    [tools] positional can also reach this field via a bare nofastmdct token,
                    which wins if both are given; bare fast-mdct (the old opt-in) is a no-op

qc options (qc; any order, after the positional arguments):
  preset=<name>     gate the measurement against a named delivery spec
                    ebu-r128-s2 | atsc-a85 | netflix
  preset=all        gate against every preset above
                    omitted: measure and report only, no gate
```

For `decode`, `drc=<scale>` instead applies §7.7.1 partial compression (`0` = ignore, `1` = as
encoded), and bare `heavy` prefers `compr` where the stream carries it — the decode-time meaning
of these two tokens is deliberately the mirror of their encode-time meaning. That applies to
AC-3 decode only: the E-AC-3 decode path takes no options, so both tokens are silently inert on
`.ec3` input.

See [Metadata](../library/metadata.md) for what each of these fields actually is at the library
level (`dynrng`, `compr`, `dialnorm`, downmix levels) — the CLI tokens above map directly onto
that page's config fields.

## The `tools:` token (`eac3-encode`)

Annex E coding tools, `+`-joined:

```text
tools:  Annex E coding tools, '+'-joined — none | cpl | spx | aht | tpn |
        nofastmdct | all
        (cpl:N / spx:N pin a band edge, aht:N the gain mode, ecpl selects
        enhanced coupling instead of standard, tpn selects transient
        pre-noise processing)
        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);
        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;
        atten:N pins the SPX notch depth, noatten removes it;
        ecpl only takes effect alongside cpl (e.g. cpl+ecpl);
        nofastmdct forces the direct-form forward MDCT instead of the
        default §7.9.4 fast path — the fast MDCT is not a coding tool
        (nothing in the bitstream's syntax changes), so 'none' and 'all'
        both leave it alone, and the older opt-in spelling 'fastmdct'
        still parses as a no-op
```

The tool set is the fourth positional argument, not an `=` option. Example:
`ac3cli eac3-encode in.wav out.ec3 192 cpl+spx:5+aht:0` turns on coupling (auto band edge),
spectral extension pinned to band 5, and AHT with GAQ off; `cpl+ecpl+tpn` in the same slot turns
on enhanced coupling (auto band edge) and transient pre-noise processing together — `ecpl` and
`tpn` are independent tools, not alternatives to each other or to `spx`/`aht`, so any
combination the tools argument accepts is legal here. `all` does not currently imply `ecpl` or
`tpn`; name them explicitly to get either.

## The `vbr` token (`eac3-encode` only)

```text
vbr (eac3-encode only): off | q:0..1[,min:kbps][,max:kbps] - E-AC-3 only
        quality is encoder-relative, not a fixed target — bit cost rises
        steeply above roughly half the range, so a high quality with no
        max bound will often refuse real programme material outright;
        bitrate_kbps still matters in vbr mode — it feeds the same
        coupling/spx frequency defaults it always has, not a target rate
```

Example: `ac3cli eac3-encode in.wav out.ec3 192 none stereo q:0.4,max:320` encodes at quality 0.4,
capped at 320 kbps whenever the content would otherwise ask for more; `bitrate_kbps` (192 here)
still drives the coupling/spx band-edge defaults the way it always has, since VBR has no fixed
target rate to hand them.

Omit `vbr` (or pass `off`) for ordinary CBR — the default. AC-3 (`encode`) is CBR-only because
`frmsizecod` indexes a fixed frame-size table; `eac3-silence` and `eac3-sine` are E-AC-3
(`frmsiz`) and are CBR-only simply because their argument lists have no `vbr` slot.

## The `layout` grammar

```text
layout: mono | stereo | 1+1 | 51 | 71 | 512 | 514 | 714
        AC-3 carries only mono | stereo | 1+1 | 51 — everything wider needs the dependent
        substreams that only E-AC-3 has.
        71 renders 8 speakers from 10 coded channels
        714 renders 12 speakers from 14 coded channels
        For 'sine' and 'eac3-sine' each speaker gets its own tone; append
        'c' to a 'sine' layout (stereoc, 51c) to enable channel coupling.
        For 'encode' and 'eac3-encode' it names the OUTPUT layout: a
        source narrower than it leaves the channels it lacks silent, and
        a wider one folds down per §7.8 using cmixlev/surmixlev.

        [layout] also takes a comma-separated Table E2.5 location list
        instead of one of the names above, for anything Annex E allows
        that has no preset: e.g. L,C,R,LFE,Vhl,Vhr or L,C,R,LFE,LFE2,Vhc.
        AC-3 accepts one too, as long as it needs no dependent substream
        (e.g. L,R,Cs or L,C,R,Cs - Table 5.8 modes no preset names).
        Locations: L C R Ls Rs Lc Rc Lrs Rrs Cs Ts Lsd Rsd Lw Rw Vhl Vhr
        Vhc Lts Rts LFE2 LFE - a paired location (Lc/Rc, Lrs/Rrs, Lsd/Rsd,
        Lw/Rw, Vhl/Vhr, Lts/Rts) must be given both halves.
```

`71` and `714` render fewer speakers than they code because, per §E3.8.2, a dependent
substream's channels replace some of the bed's rather than adding to it — see
[Wide layouts](../library/encoding-eac3.md) for the encoder-side mechanics behind that.

`1+1` is not a speaker layout at all — two independent, single-channel programmes sharing one
syncframe (§5.4.2's "1+1 dual mono") — so it's never inferred from a source's channel count the
way `mono`/`stereo`/`51`/etc. are; it has to be named explicitly. `encode`/`eac3-encode` take its
two channels either as one two-channel WAV (channel 0 = Ch1, channel 1 = Ch2) or as two mono WAV
files (`in.wav` = Ch1, the trailing `in2.wav` positional = Ch2) — see [Commands](commands.md) for
both forms. `dialnorm2=`/`drc2=`/`heavy2` above set Ch2's own dialnorm/DRC/heavy compression;
none of the three is inherited from Ch1's — a stream that wants both programmes treated alike sets
both explicitly. `decode` writes Ch1/Ch2 back out in that same order, and `levels` names them
`Ch1`/`Ch2` rather than a speaker position that would not apply.

## Source options (`encode`/`eac3-encode`): `src=`, `map=` and `offset=`

```text
source options (encode/eac3-encode; any order, after the positional arguments):
  src=<path>        an additional input source; repeat for more than one
  map=<spec>        <source>.<channel>[-<channel2>]:<dest>[@<trim>][,...] - dest is a channel
                     name, obj, objm, p1, p2 or none; a channel range is only legal with obj,
                     objm or none, and folds to one mono object with objm; trim is an optional
                     signed dB gain in [-24,24] on dest, e.g. L@-3.5
                     once given, every loaded channel must appear - explicit 'none' silences
                     the goes-nowhere warning without giving it anywhere to go
  offset=<sourceIndex>:<seconds>   leading silence ahead of that source's own channels
                     (seconds >= 0), same 0-based numbering as src=
                     the programme is still as long as the longest one once every offset is
                     applied
```

`src=` loads another WAV alongside `in.wav` (source index 0), in the order given — `src=a.wav
src=b.wav` makes `a.wav` source 1 and `b.wav` source 2. Every source must share `in.wav`'s sample
rate.

With exactly one source (no `src=` at all), `map=` is not needed: the existing automatic
single-source panning applies, byte-identical to a plain `ac3cli encode`/`eac3-encode` invocation
that predates this option — omitting `map=` is defined to behave exactly as if `src=`/`map=` did
not exist. With more than one source, `map=` becomes mandatory: automatic panning has no defined
meaning across several files, so every loaded channel needs an explicit entry (or an explicit
`none`) before the encode will run.

Each `map=` entry is `<source>.<channel>:<dest>`, comma-separated, `<channel>` 0-indexed. A
channel *range* (`<channel>-<channel2>`) is only legal when `<dest>` is `obj`, `objm` or `none` —
a location or a programme names exactly one channel, so a range there would be ambiguous about
which one it means. `objm` folds the whole range into ONE mono object (equal-weight sum, scaled by
`1/n` so several full-range channels summed together don't clip past what one alone would) rather
than one object per channel the way a plain `obj` range does. Two entries naming the same location,
or more than one entry per dual-mono programme, is refused.

The `obj`/`objm` destinations parse but currently do nothing in `ac3cli`: the routing behind
`encode`/`eac3-encode` skips every non-location row (also true of a stray `p1`/`p2` row on a
target that isn't dual mono), the CLI has no object assembly behind `map=`, and `atmos-encode`
ignores `src=`/`map=` entirely — audio mapped onto an object destination is still discarded, but
no longer silently: `encode`/`eac3-encode` print a warning naming the source/channel and
destination for each row that resolves to nothing. Object destinations need the object-capable
front end, the GUI (see [GUI → Multi-source & assignment](../gui/source-assignment.md)).

Any `<dest>` may carry an optional trailing `@<trim>` — a signed decibel gain in `[-24, 24]`,
snapped to a tenth of a dB (`L@-3.5`, `obj@2`) — applied as linear gain wherever that channel's
content reaches the stream: folded into the routing matrix for a bed position or a dual-mono
programme, or, for `obj`/`objm`, into the object's plane at assembly — which only the GUI
performs, per the note above. Omitted (no `@`) means no trim, the same as an explicit `@0`.

```bash
ac3cli eac3-encode roundtrip-stereo.wav out.ec3 384 none 51 \
    src=roundtrip-51.wav \
    map=0.0:C,0.1:none,1.0:L,1.1:R@-3,1.2:none,1.3:LFE,1.4:Ls,1.5:Rs \
    offset=1:2.5
```

`roundtrip-stereo.wav`'s left channel (source 0, channel 0) carries the centre; its right channel
is explicitly silenced. `roundtrip-51.wav` (source 1) fills the rest, its right channel (`1.1`)
trimmed 3 dB down, with its own centre channel (`1.2`) also sent nowhere so it doesn't collide
with the first source's. `[vbr]` and `[in2.wav]` are both skippable here even though they come
earlier in `eac3-encode`'s own positional order — the parser lifts option tokens (anything
containing `=`, plus the known bare flags) out of the argument list wherever they appear, so the
remaining positionals keep their places whether options are present or not.

`offset=1:2.5` delays `roundtrip-51.wav` (source 1) by 2.5 seconds of leading silence ahead of its
own channels — every channel that source contributes shifts together, as when it starts, not what
it contains. `offset=` applies as silence, not truncation: the programme's overall length grows to
cover whichever source ends latest once every offset is applied, not just the longest source's own
raw length, so a delayed source is never cut short to fit. `<sourceIndex>` uses the same numbering
`src=` establishes (`0` is the primary positional file, `1..N` are `src=` in the order given), and
works with a single source too — `ac3cli encode in.wav out.ac3 384 51 offset=0:2.5` needs no
`src=`/`map=` at all. Omitting `offset=` for a source (or giving it `0` seconds) behaves exactly as
it always has.

`dialnorm=auto`/`dialnorm2=auto` work alongside `src=`/`map=`: the whole programme is routed once
as a measurement pre-pass — the same BS.1770-4 gated pass the single-file path runs, over what
`map=` actually assembles (post-routing, post-trim), not each source's own raw channels — before
the real encode loop routes it again to encode it. A `1+1` target measures Ch1/Ch2 independently,
same as the single-file case; every other target gets one whole-programme measurement over the
routed bed. A trim on `map=` (e.g. `L@-6`) is measured on the trimmed signal, since that is what
actually reaches the stream.

In the GUI, a full-bandwidth channel explicitly assigned onto `LFE`/`LFE2` is sent through a
120 Hz low-pass rather than passed through untouched — an explicit assignment states raw content
for that position, and a real subwoofer (and the LFE channel's own +10 dB mixing headroom)
assumes it only ever carries deep bass. `ac3cli` does not filter: its `map=` routing is a pure
gain matrix, so content mapped onto `LFE`/`LFE2` (e.g. `1.3:LFE` above) passes through
unfiltered. Neither front end touches a source's own dedicated LFE channel reaching `LFE`
through automatic single-source routing (no `src=`/`map=` at all) — that stays bit-exact, since
nothing there claims full-bandwidth content belongs on that position.

The GUI's own multi-source Format-tab table (**Add source…** plus a per-channel assignment field)
is a direct front end over this same grammar — see
[GUI → Multi-source & assignment](../gui/source-assignment.md).

## Record/live options (`record`, `live`): `container=mkv`

```text
record/live options (record, live; any order, after the positional arguments):
  container=mkv     write straight to Matroska instead of the bare elementary
                     stream this writes by default - same shape of choice as
                     the GUI's own Container setting (container=matroska is
                     an accepted alias)
  container=raw     the default, spelled out
```

`container=mkv` (alias `container=matroska`) writes the take straight to Matroska (`.mkv`)
instead of a bare `.ac3`/`.ec3` elementary stream, in the one `record`/`live` command — unlike
`mkv`, which wraps an
*already-encoded* file after the fact (see [Command-specific notes](#command-specific-notes)
below), there is no second command needed here. `container=raw` is the default spelled out
explicitly; any other value is refused rather than silently ignored, the same rule every option on
this page follows. This is the same choice the GUI's own Container combo offers on the Format tab
(see [GUI → Format & channels](../gui/format-and-channels.md)) — see [GUI → Live capture &
session](../gui/live-session.md#take-durability) for how a live session's own take durability
differs slightly between the two front ends.

```bash
ac3cli record out.mkv 30 192 0 container=mkv
ac3cli live out.mkv 0 30 448 -2 -2 atmos container=mkv
```

## Live options (`live`): `capture2=`

```text
live options (live; any order, after the positional arguments):
  capture2=<index>  a second capture device, clock-conformed to the first (see 'devices')
```

`capture2=<index>` names a second capture device — same 0-based numbering `devices` prints and the
`capture_device` positional already uses — that joins the session alongside the master. The master
(`capture_device`) still paces the session exactly as it always has: frame timing, target frame
count and every other positional argument mean what they meant before this option existed.
`capture2`'s own sample rate does not need to match the master's, only be a legal AC-3 rate itself
(32, 44.1 or 48 kHz) — a drift-tracking resampler continuously conforms the slave's stream to the
master's pacing, correcting both the nominal rate conversion and whatever free-running clock drift
the two devices accumulate against each other. The slave's channels are appended after the
master's own, at new, higher channel indices, in the same interleaved per-frame block that feeds
the encoder — so a two-device `atmos` session simply gets more objects to place. The measured
drift is printed once, in signed parts-per-million, when the session ends.

```bash
ac3cli live out.ec3 0 30 448 -2 -2 atmos capture2=1
```

Captures 30 seconds of Atmos-mode E-AC-3 from device 0 (the clock master) plus device 1
(clock-conformed to device 0), no monitor or passthrough, writing `out.ec3`.

## Qc options (`qc`): `preset=`

```text
qc options (qc; any order, after the positional arguments):
  preset=<name>     gate the measurement against a named delivery spec
                    ebu-r128-s2 | atsc-a85 | netflix
  preset=all        gate against every preset above
                    omitted: measure and report only, no gate
```

`preset=<name>` checks `qc`'s BS.1770-4 measurement against one named delivery-loudness gate instead of just
reporting it; `preset=all` checks every one below in a single run. Each preset states a target integrated
loudness, a symmetric tolerance around it (in LU) and a true-peak ceiling (a one-sided limit, never exceeded —
not a tolerance band). The numbers are defined in `ac3::meta::qc_preset()`
([`ac3/meta/qc.hpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/src/lib/include/ac3/meta/qc.hpp)), each
read directly from its own primary source rather than recalled from memory:

| Preset | Target | Tolerance | Max true peak | Source |
|---|---|---|---|---|
| `ebu-r128-s2` | −23.0 LUFS | ±1.0 LU | −1.0 dBTP | EBU R 128 s2 "Loudness in Streaming" (Geneva, November 2023, v3) recommendation (e) — programmes "should be streamed unchanged, that is at −23.0 LUFS" — which itself defers tolerance/true-peak to EBU R 128 (Geneva, November 2023, v5) recommendations (h) and (m) |
| `atsc-a85` | −24.0 LKFS | ±2.0 dB | −2.0 dBTP | ATSC A/85:2013 (with Corrigendum No. 1, 11 February 2021) §6 "Target Loudness and True Peak Levels for Content Delivery or Exchange" |
| `netflix` | −27.0 LKFS | ±2.0 LU | −2.0 dBTP | Netflix "Sound Mix Specifications & Best Practices" v1.6, Near-field Audio Prerequisites for Mix Facilities |

Omitting `preset=` entirely leaves `qc` in measure-and-report mode — every number is still printed, there is
just no PASS/FAIL verdict and nothing to gate on. See [Commands → `qc`](commands.md#decoding-inspection) for the
full report format and the exit-code convention this drives.

## Defaults

Optional positional arguments, when omitted:

- `silence`, `sine`, `eac3-silence`, `eac3-sine` — 5 s at 192 kbps; the tone commands default to
  1000 Hz at 50% amplitude, and the three that take `[layout]` default to `stereo`.
- `orbit` — 8 s at 448 kbps, 4 s per orbit.
- `atmos` — 8 s at 448 kbps, 4 objects, 6 s per orbit.
- `record` — 5 s at 192 kbps from device 0.
- `live` — 10 s at 192 kbps.
- `play`, `monitor` — device `-1`, the default output.

## What the encoder accepts

- WAV sample rates: AC-3 takes 32, 44.1 or 48 kHz (Table 5.6); E-AC-3 additionally takes the
  Annex E `fscod2` half rates 16, 22.05 and 24 kHz.
- The bit rate must be one of the 19 nominal AC-3 rates (Table 5.18), 32 through 640 kbps.
- `record` captures the first two channels of the endpoint (stereo); a mono device is duplicated
  across both.
- The Atmos commands take 1 to 15 objects — the bed's LFE is the 16th, and TS 103 420 §8.3.2.2
  caps the total at 16.

## Command-specific notes

- **`mkv`** reads format, packet boundaries, sample rate and channel count from the bitstream
  itself, so it cannot be told the wrong ones. E-AC-3 dependent substreams are grouped into their
  access unit and counted as the channels they render.
- **`spdif`** wraps an already-encoded AC-3 or E-AC-3 file's IEC 61937 bursts as a 2-channel
  16-bit PCM WAV, playable bit-exactly (100% volume, no mixing) into an S/PDIF or HDMI output to
  light up a receiver's Dolby Digital indicator. Detects AC-3 vs. E-AC-3 from the stream itself
  (`bsid`); E-AC-3's carrier runs at four times the content sample rate (WASAPI's own
  `make_eac3_format` convention), which is legal though unusual for a plain PCM16 file. The GUI's
  S/PDIF container option is this same command, run automatically as the second half of a
  two-command encode — see [GUI → Format & channels](../gui/format-and-channels.md).
- **`atmos`** encodes objects orbiting the room at different heights and rates as a 5.1 E-AC-3 bed
  plus JOC + OAMD side data (TS 103 420); FFmpeg reports the result as "Dolby Digital Plus + Dolby
  Atmos". **`atmos-encode`** does the same but makes each channel of a real source file an object
  instead of synthesizing motion. Its optional `[paths.txt]` (same format `atmos-path` reads)
  authors that motion instead of the default static, fanned-out placement — keyed by WAV channel
  index, so an object index the file doesn't mention keeps its default placement unchanged.
- **`atmos` mode**: `objects` (default) writes the JOC+OAMD container; `bed51` omits it so the
  5.1 bed still plays on a decoder that would otherwise refuse an object container it can't
  validate, instead of falling back to the bed on its own. See
  [Atmos & JOC](../concepts/atmos-joc.md) for why a decoder can tell the difference at all.
- **`sign-objects`** (with **`signing-key=<path>`**): signs the object container's EMDF protection
  tag so a validating decoder reconstructs the objects instead of playing the bed. Honored by all
  three Atmos commands (`atmos`, `atmos-path`, `atmos-encode`). Off unless you pass both —
  `sign-objects` alone with no key is an error. The key may also come from
  `AC3FORGE_SIGNING_KEY_FILE` / `AC3FORGE_SIGNING_KEY` instead of `signing-key=`. The key is never
  stored by the tool; the algorithm is in-tree but the key is yours to provision. Full details in
  [Object signing](../concepts/object-signing.md).
- **`fast-mdct=off`**: every encode runs the §7.9.4 fast forward MDCT by default (the quality
  evidence that made it the default: ~3e-12 max relative coefficient error against the direct
  form, 331 dB direct-vs-fast end-to-end SNR, 0.000 dB SNR delta against an independent oracle
  at 192–448 kbps — see `tools/quality_race.py fast-mdct`). `fast-mdct=off` forces the direct
  §8.2.3.2 reference form — the validation oracle — wherever the command encodes, including the
  `atmos*`, `record`, `live`, `eac3-sine` and `eac3-encode` (both its single-source and its
  `src=`/`map=` multi-source path). `eac3-encode` also has a second spelling: the bare
  `nofastmdct` token inside its `[tools]` positional argument reaches the same field, and wins
  over `fast-mdct=off` if both are given on the same command line. Typing `tools=nofastmdct` as
  if it were a trailing option does not parse, since any `=` token goes to the options parser,
  which has no `tools` key. The fast MDCT is not a coding tool, so `none`/`all` leave it alone.
  `eac3-silence` has no use for either spelling: it builds a silent access unit directly, with no
  forward transform in the loop to choose a path for. The bare `fast-mdct` word and the
  `fastmdct` tool token — the opt-in spellings from when this defaulted off — still parse and now
  name what already happens.
- **`keep-partial`**: `encode`, `eac3-encode` and `atmos-encode` refuse a frame that cannot fit the
  configuration mid-run just as they always have, but with `keep-partial` given, whatever frames
  were already encoded before that point are written to `<name>.partial.<ext>` (`out.ec3` →
  `out.partial.ec3`) rather than discarded — the run still exits non-zero and prints the same
  error either way, only what happens to the frames already produced changes. Off by default, the
  same as every other bare token here; a plain invocation with no `keep-partial` behaves exactly
  as it always has. Mirrors the GUI's own keep-partial-output preference (see
  [GUI → window layout](../gui/index.md#preferences)) rather than a separate idea — the same
  `.partial.` naming either way, so a file produced by either front end is named alike.

## Next

[Concepts → Atmos & JOC](../concepts/atmos-joc.md) if any of `JOC`, `OAMD`, or the object/bed
relationship above are unfamiliar; [Spatial & Atmos objects](../library/spatial-and-atmos.md) for
the library-level API these commands are thin wrappers over.
