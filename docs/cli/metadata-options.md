# Metadata options

Every encoding command in [Commands](commands.md) accepts these after its positional arguments,
in any order:

```text
metadata options (any order, after the positional arguments):
  drc=<profile>     §7.7.1 dynamic range control per block
                    film-standard | film-light | music-standard | music-light | speech
  heavy             §7.7.2 heavy compression: a peak ceiling in the
                    mono downmix, at syncframe resolution
  ceiling=<dBFS>    that ceiling (default -0.5)
  dialogue=<dBFS>   where heavy compression puts dialogue (default -20)
  dialnorm=auto     measure BS.1770 loudness and derive dialnorm (§5.4.2.8)
  dialnorm=<1..31>  set it directly (default 31)
  dialnorm2=auto | <1..31>   Ch2's own dialnorm, layout 1+1 only (§5.4.2.16, default 31)
  cmixlev=-3|-4.5|-6      centre downmix level (Table 5.9)
  surmixlev=-3|-6|off     surround downmix level (Table 5.10)
  mixmeta           E-AC-3 only: emit the mixmdate group (Table E1.2)
  lfemix=<0..31>|off      E-AC-3 LFE mix level, 10-code dB (§E2.3.1.11)
  dmixmod=ltrt|loro|none  preferred stereo downmix (Table D2.2)
```

For `decode`, `drc=<scale>` instead applies §7.7.1 partial compression (`0` = ignore, `1` = as
encoded), and bare `heavy` prefers `compr` where the stream carries it — the decode-time meaning
of these two tokens is deliberately the mirror of their encode-time meaning.

See [Metadata](../library/metadata.md) for what each of these fields actually is at the library
level (`dynrng`, `compr`, `dialnorm`, downmix levels) — the CLI tokens above map directly onto
that page's config fields.

## The `tools:` token (`eac3-encode`)

Annex E coding tools, `+`-joined:

```text
tools:  Annex E coding tools, '+'-joined — none | cpl | spx | aht | tpn | all
        (cpl:N / spx:N pin a band edge, aht:N the gain mode, ecpl selects
        enhanced coupling instead of standard, tpn selects transient
        pre-noise processing)
        cpl:N / spx:N pin that tool's band edge (e.g. cpl:4+spx:5);
        aht:N pins the GAQ mode — aht:0 is AHT with GAQ switched off;
        atten:N pins the SPX notch depth, noatten removes it;
        ecpl only takes effect alongside cpl (e.g. cpl+ecpl)
```

Example: `tools=cpl+spx:5+aht:0` turns on coupling (auto band edge), spectral extension pinned
to band 5, and AHT with GAQ off. `tools=cpl+ecpl+tpn` turns on enhanced coupling (auto band edge)
and transient pre-noise processing together — `ecpl` and `tpn` are independent tools, not
alternatives to each other or to `spx`/`aht`, so any combination `parse_tools` accepts is legal
here. `all` does not currently imply `ecpl` or `tpn`; name them explicitly to get either.

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

Omit `vbr` (or pass `off`) for ordinary CBR — the default, and the only mode AC-3 (`encode`,
`eac3-silence`, `eac3-sine`) supports at all, since `frmsizecod` has no free word count to vary.

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
both forms. `dialnorm2=` above sets Ch2's own dialnorm; `heavy`/`drc` apply to both channels
independently, each getting its own compressor. `decode` writes Ch1/Ch2 back out in that same
order, and `levels` names them `Ch1`/`Ch2` rather than a speaker position that would not apply.

## Source options (`encode`/`eac3-encode`): `src=` and `map=`

```text
source options (encode/eac3-encode; any order, after the positional arguments):
  src=<path>        an additional input source; repeat for more than one
  map=<spec>        <source>.<channel>[-<channel2>]:<dest>[,...] - dest is a channel name, obj,
                     p1, p2 or none; a channel range is only legal with obj or none
                     once given, every loaded channel must appear - explicit 'none' silences
                     the goes-nowhere warning without giving it anywhere to go
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
channel *range* (`<channel>-<channel2>`) is only legal when `<dest>` is `obj` or `none` — a
location or a programme names exactly one channel, so a range there would be ambiguous about
which one it means. Two entries naming the same location, or more than one entry per dual-mono
programme, is refused.

```bash
ac3cli eac3-encode roundtrip-stereo.wav out.ec3 384 none 51 \
    src=roundtrip-51.wav \
    map=0.0:C,0.1:none,1.0:L,1.1:R,1.2:none,1.3:LFE,1.4:Ls,1.5:Rs
```

`roundtrip-stereo.wav`'s left channel (source 0, channel 0) carries the centre; its right channel
is explicitly silenced. `roundtrip-51.wav` (source 1) fills the rest, with its own centre channel
(`1.2`) also sent nowhere so it doesn't collide with the first source's. `[vbr]` and `[in2.wav]`
are both skippable here even though they come earlier in `eac3-encode`'s own positional order —
the parser treats the first token containing `=` as the start of the trailing options, whichever
positional slot would otherwise have been next.

`dialnorm=auto`/`dialnorm2=auto` are not yet supported alongside `src=`/`map=` — pass an explicit
`dialnorm=<1..31>` (and `dialnorm2=` for `1+1`) instead; measuring loudness per source is a later
extension, not a hole in the routing itself.

The GUI's own multi-source Format-tab table (**Add source…** plus a per-channel assignment field)
is a direct front end over this same grammar — see
[GUI → Multi-source & assignment](../gui/source-assignment.md).

## Command-specific notes

- **`mkv`** reads format, packet boundaries, sample rate and channel count from the bitstream
  itself, so it cannot be told the wrong ones. E-AC-3 dependent substreams are grouped into their
  access unit and counted as the channels they render.
- **`atmos`** encodes objects orbiting the room at different heights and rates as a 5.1 E-AC-3 bed
  plus JOC + OAMD side data (TS 103 420); FFmpeg reports the result as "Dolby Digital Plus + Dolby
  Atmos". **`atmos-encode`** does the same but makes each channel of a real source file an object
  instead of synthesizing motion.
- **`atmos` mode**: `objects` (default) writes the JOC+OAMD container; `bed51` omits it so the
  5.1 bed still plays on a decoder that would otherwise refuse an object container it can't
  validate, instead of falling back to the bed on its own. See
  [Atmos & JOC](../concepts/atmos-joc.md) for why a decoder can tell the difference at all.

## Next

[Concepts → Atmos & JOC](../concepts/atmos-joc.md) if any of `JOC`, `OAMD`, or the object/bed
relationship above are unfamiliar; [Spatial & Atmos objects](../library/spatial-and-atmos.md) for
the library-level API these commands are thin wrappers over.
