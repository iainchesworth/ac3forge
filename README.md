# ac3forge

A **clean-room AC-3 encoder** written from first principles in C++23, working directly from
the published standards — no FFmpeg, no codec libraries. The goal: take one or more channels
of PCM audio — or sound objects positioned and moved in 3D space — and produce a compliant
AC-3, Dolby Digital Plus, or DD+ with Dolby Atmos elementary stream that any decoder or AV
receiver accepts.

> **Naming note:** "Dolby Digital" is a live trademark of Dolby Laboratories. This project
> implements the openly published **AC-3** standard (ATSC A/52 / ETSI TS 102 366) and is not
> affiliated with or endorsed by Dolby. `ac3forge` is a provisional working name.

## Status

**Milestones 0–2 complete.** The encoder emits fully valid AC-3 syncframes (2/0 digital
silence, any legal bitrate × sample rate): `ac3cli silence out.ac3` produces a stream that
FFmpeg strict-decodes (`-err_detect crccheck+bitstream+buffer+explode`) with zero errors to
bit-perfect silence, and that an independent from-spec bitstream parser rates CONFORMANT —
including the A/52 §5.5 layout constraints (padding via in-block skip fields) and both CRC
words (crc1 solved via GF(2) polynomial inverse, since it precedes its coverage region).

**Milestone 3 (MDCT + KBD window) complete.** The analysis filterbank is in: the 512-point
KBD window is generated at compile time from the Kaiser-Bessel formula and reproduces every
value of the spec's published Table 7.33 exactly (5-decimal rounding), the forward MDCT
matches independent numpy goldens ≤1e-10, and a 50%-overlap TDAC round-trip through the
*normative* §7.9.4.1 decoder inverse reconstructs input ≤1e-10 — locking window, both
transforms, and the −2/N ↔ ×2 level convention together.

**Milestone 5 (the hard middle) complete — the encoder encodes real audio.**
`ac3cli sine out.ac3` produces AC-3 that FFmpeg strict-decodes with zero errors to a
999.93 Hz sine at exactly the target amplitude (+0.000 dB) with **88.3 dB SNR**. The full
pipeline: windowed MDCT → 25-bit fixed coefficients → D15 exponents (decoder-mirrored) →
the bit-exact §7.2.2 integer bit-allocation engine (validated against an independent
Python transcription of the spec pseudocode, zero tolerance) → binary SNR-offset search →
§7.3 mantissa quantization with cross-channel grouping → packing + CRCs. The
bit-allocation tables (7.6–7.16) are script-extracted from the spec text with
self-verification, like every table before them.

**Milestone 6 (5.1 + LFE + the in-repo decoder) complete.** The encoder handles every
audio coding mode (mono through 3/2) plus LFE at all three sample rates, with exact
44.1 kHz CBR via Bresenham frame-size alternation. The new in-repo decoder — built on the
same normative core — reaches **float32-precision PCM parity with FFmpeg's decoder**
(max diff 7.9e-6 ≈ −102 dBFS) on identical streams, and a 5.1 encode with per-channel
tones decodes through FFmpeg with every channel carrying exactly its own tone
(channel-order verified end-to-end). `ac3cli decode` exercises the decoder from the CLI.

**Milestone 7 (quality layer) complete — and the encoder now beats FFmpeg's.** Per-block
exponent-strategy selection (§8.2.8: D45/D25/D15 by reuse span, variation-triggered),
stereo rematrixing (§7.5.3 minimum-power rule, with the decoder-side undo), and
bitrate-aware bandwidth defaults. The quality race (`tools/quality_race.py`, synthesized
program material, FFmpeg as neutral referee): **ours 41.2/44.0/45.1/51.1 dB vs FFmpeg's
41.0/42.9/44.2/47.6 dB at 192/256/320/448 kbps** — ahead at every rate (SNR metric;
`ac3cli encode` now takes arbitrary stereo WAV input). Decoder parity vs FFmpeg holds on
rematrix-active program material (max diff 1.1e-5).

**Milestones 8–9 complete: sounds move in space, and the stream is receiver-ready.**
The spatial layer (`src/lib/src/spatial/`) places mono objects on the ITU 5.1 ring via
energy-normalized 2D VBAP with per-block gain ramps and explicit LFE sends; `ac3cli orbit`
renders a tone circling the listener straight into 5.1 AC-3 (an end-to-end test parks the
object at each speaker and proves the decoded energy follows it: C → L → SL → SR → R).
The IEC 61937 packer (`src/lib/src/sinks/`) wraps frames into S/PDIF bursts **byte-exact against
FFmpeg's spdif muxer**, and `ac3cli spdif` emits them as a PCM16 WAV — played bit-exactly
through a passthrough output, a receiver locks on and lights its Dolby Digital indicator.

**Live WASAPI capture works.** `ac3::capture` enumerates every active input endpoint plus
every render endpoint as a loopback source, and streams interleaved float samples through a
lock-free SPSC ring into the encoder. Verified end to end on real hardware: a 1 kHz tone
played through the speakers was captured via loopback, encoded, and decoded back at exactly
1000.0 Hz with zero ring overruns. Loopback gaps (a render endpoint delivers nothing while
the machine is silent) are filled against a QPC timeline so the stream stays continuous.
`ac3cli devices` lists endpoints, `ac3cli record` captures straight to AC-3, and the GUI's
capture card is live with a peak-level meter.

**Exclusive-mode passthrough is implemented.** `ac3::sinks::PassthroughSink` opens a render
endpoint in WASAPI exclusive mode with a `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL`
format (including the documented buffer-alignment retry) and streams bursts from a
lock-free queue on an MMCSS "Pro Audio" thread — exclusive mode being mandatory, since the
shared-mode engine would mix, resample or volume-scale the bursts and destroy the bit
pattern. `ac3cli outputs` probes every endpoint twice, for AC-3 *and* for plain exclusive
PCM, so an unavailable device tells you **why**: "cannot bitstream" (analog output) versus
"no exclusive access" (disabled or in use). `ac3cli play` streams a file to a receiver.

> Not yet confirmed against real bitstreaming hardware: this machine has no S/PDIF or HDMI
> audio endpoint, so `IsFormatSupported` correctly answers "no" everywhere. The exclusive-mode
> path itself is proven — the Realtek endpoint accepts our exclusive PCM format — but the
> IEC 61937 descriptor awaits a receiver to confirm positively.

**Channel coupling encodes and decodes.** Above the coupling frequency the full-bandwidth channels stop
carrying their own coefficients and share one coupling channel plus per-band coordinates —
the tool that makes 5.1 viable well below 448 kbit/s. `ac3cli sine … 51c` or
`ac3cli encode … couple` turns it on. FFmpeg strict-decodes coupled 5.1, and a targeted
probe confirms the envelope really is preserved: a channel carrying a 12 kHz tone stays
113 dB above a silent one in that band, while the region below the coupling frequency is
bit-for-bit untouched. The in-repo decoder reads coupling too — strategy, banded
coordinates, phase flags and leak parameters — so coupled streams round-trip in process
without FFmpeg in the loop.

**E-AC-3 encodes, up to 7.1.4.** `ac3cli eac3-sine` emits bsid-16 (Dolby Digital Plus)
carrying real audio in stereo, 5.1, 7.1, 5.1.2, 5.1.4 and 7.1.4. E-AC-3 is a different
container, not an AC-3 variant: no crc1, an arbitrary 11-bit `frmsiz` instead of a size
table (so the 44.1 kHz padding alternation disappears), and exponent strategies for all
six blocks hoisted into a frame-level `audfrm`. Layouts wider than 5.1 ride in *dependent
substreams* beside a self-sufficient 5.1 bed, each with a Table E2.5 `chanmap` saying
which speakers its channels belong to; per §E3.8.2 the ones that collide with the bed
replace it and the rest extend the layout.

**And it decodes.** The in-repo decoder now reads bsid 16 — the whole of Tables
E1.2/E1.3/E1.4, dependent substreams, `chanmap` and the §E3.8.2 render — reaching
float32-precision PCM parity with FFmpeg (max diff 1.4e-5) on every layout FFmpeg will
read, and reading FFmpeg's *own* encoder output too. That last part is the point: **7.1.4
has no external oracle at all.** It needs two dependent substreams, and FFmpeg refuses any
frame with `substreamid != 0` in `ff_ac3_parse_header` — proven exhaustively across
hand-rolled MKV, FFmpeg Matroska, MPEG-TS and MP4. A decoder we control is what closes
that gap, and every later Annex E feature inherits the same self-check.

**Dolby Atmos objects encode (ETSI TS 103 420).** `ac3cli atmos out.ec3` sends objects
orbiting the room at different heights and rates, and ffprobe reports
`eac3 (Dolby Digital Plus + Dolby Atmos), 48000 Hz, 5.1(side)` — the same shape real DD+
Atmos files probe as. There are no extra coded channels: the objects are panned into a 5.1
bed that any decoder plays unchanged, and beside it ride two payloads in an EMDF container
(ETSI TS 102 366 Annex H) tucked into a block skip field — **OAMD** saying where each
object is, and **JOC** saying how to pull them back out as a per-band matrix over the five
downmix channels. (Dolby's own DD+ JOC streams carry the container in a skip field with
`auxdatae` clear, not in the aux field; ours match, checked against their reference content.) This is also why discrete 7.1.4 was a dead end: real 7.1.4 is JOC over a
5.1 bed, not twelve channels, and no shipping profile allows the two dependent substreams
the discrete layout would need.

The JOC Huffman tables are not printed in the standard — Annex A.1 gives only their names,
modes and types, and ships the trees in the companion archive as `ts_103420_tables.c`, so
`tools/gen_joc_tables.py` inverts that file (decoder trees in, encoder codewords out) and
refuses to write unless every tree is a complete prefix code. The reconstruction matrix is
the minimum mean-square estimate `M = P Dᵀ (P D Dᵀ + εI)⁻¹`; because the encoder *built*
the downmix it knows `D` exactly instead of estimating it, which makes the solve near-exact
for well-separated objects. Objects that share a direction — two at one azimuth and
different heights, say — cannot be separated by any linear combination of the bed, and the
solve splits their energy by power instead. `ac3cli atmos-encode in.wav out.ec3` makes every
channel of a real file an object; `ac3gui` exposes the same thing as a switch, a plan view of
the room to drag the objects around, and sliders for height, spread and LFE send. Spread is
not decoration: objects that reach the bed by the same route are exactly the ones JOC cannot
pull apart again. The LFE send is the only route to that channel, because no direction points
at it.

The syntax was checked field-for-field against Dolby's own tooling — the Reference Player
and the Dolby Media Encoder — used as oracles. That diffing fixed several real bugs (the
skip-field carriage above; `codecdatae=0`; a dynamic-object-only program with the LFE as an
object but not a JOC output; metadata flag arrays transmitted index-0-first) and left our
frame headers and container matching Dolby's byte-for-byte on the fields that matter.

> **The one thing our objects will *not* do:** decode as objects in Dolby's decoder.
> Reverse-engineering established why, and it is not a bug in this encoder. DD+ JOC gates
> object decoding on a keyed, sequence-bound MAC (HMAC-SHA-256) in the EMDF `protection`
> field — which the standard leaves "implementation dependent and not defined" — whose key
> is a secret embedded in every decoder binary. Our stream is spec-correct (FFmpeg validates
> it, the bed decodes bit-exactly, Dolby's own parser flags `atmos=true`); it simply isn't
> *signed* with Dolby's key, so the decoder falls back to the 5.1 bed. The gate is
> authenticity, not conformance. What *is* verified about reconstruction is the maths —
> §6.6.6 applied per band recovers each object to better than −20 dB, and the same for
> leakage between them.

**You can see what every channel is carrying.** `ac3/analysis/` meters audio the way a
console does — peak with an instant attack and a 20 dB/s fallback, a 1.2 s hold marker, RMS
over a 300 ms integration — plus exact whole-signal statistics and the Gerzon energy vector
over the BS.775 ring. Both front ends draw from it, including where a level sits on the bar,
so a printed figure and a moving needle can never disagree. `ac3cli levels` reports any WAV,
AC-3 or E-AC-3 file channel by channel; `encode`, `decode`, `sine` and `orbit` print the same
table when they finish, and `record` meters live in the terminal. The GUI grows a Channel
levels card that relabels itself for the active layout beside a soundfield view showing the
speaker ring and where the energy sits. Feeding those meters meant widening both front ends
to 1–6 channel WAV input, with the WAV↔A/52 channel permutation now in the library rather
than copied into each caller. `ac3gui --smoke` and `--smoke-record` drive the file and
live-capture paths headlessly and report what the meters did, so "the display is wired to the
audio" is a checkable claim rather than a screenshot.

**Both front ends reach the whole codec.** Everything above — AC-3 or E-AC-3, any of the
seven layouts, the Annex E tools, the metadata group, objects, `.ac3`/`.ec3`/`.mkv` — is
selectable from `ac3cli` *and* from `ac3gui`, from a file or from a microphone. The two agree
because they are not two implementations: `ac3::plan` holds the layout table, the `'+'`-joined
tool token and the metadata mapping, and each front end only collects and displays. A GUI that
re-derived any of it would be free to mean something different by "5.1.4" or by "all", and
nothing would catch it. `ac3cli`'s usage text and `ac3gui`'s combo boxes are both generated
from the same table, so neither can offer a layout the parser rejects or hide one it takes.

A source need not already *be* the layout it is encoded into. `plan::route` places each source
channel onto the target's speakers by direction — pairwise VBAP over the target ring, with a
constant-power crossfade to the height layer — and where the target is narrower it uses §7.8's
own coefficients instead, because fold-down is specified and up-mix is not. Nothing is
invented: a stereo source encoded as 7.1.4 leaves ten speakers silent, and both front ends say
so by name rather than leaving it to be discovered on the meters. Two rules earn their keep
there: a bed channel a dependent *replaces* is fed a full 5.1 fold (a 7.1 source's sides and
rears both land in it), while one a dependent merely *extends* is not, or a 5.1.4 decoder
would hear the ceiling twice. And per-substream §7.8.1 normalisation keeps a fold from
clipping — a 5.1.4 source rendered into 7.1.4 measured +1.5 dBFS in the bed without it.

**The metadata layer is real.** Everything above decodes; this is what makes it *play*
right. An AV receiver reads exactly these bits to set level, compress dynamics and fold
down to fewer speakers than the stream carries, and until now they were all zero.

- **`dynrng` (§7.7.1)** — per-block dynamic range control, generated by an RMS-detected
  compressor riding a piecewise-linear static curve. The five conventional Dolby profiles
  (`drc=film-standard|film-light|music-standard|music-light|speech`) are exposed; A/52 fixes
  the wire format and the intent but never the curve, so the profiles are documented as what
  they are. Levels are referenced to dialogue via `dialnorm`, so a profile behaves the same
  whatever level the source was mastered at.
- **`compr` (§7.7.2)** — heavy compression, implemented as what the spec asks for: a
  *limiter* guaranteeing a peak ceiling in the §7.8 mono downmix, not a rescaled `dynrng`.
  Instantaneous attack, rate-limited release, and it rounds **down** because nearest-code
  rounding can overshoot a ceiling by half a step. Its peak detector includes the previous
  frame's MDCT overlap — those samples are coded in this frame, and omitting them is exactly
  how the ceiling leaks at a loud-to-quiet transition.
- **`dialnorm` (§5.4.2.8)** — measured, not defaulted. `ac3cli loudness` and
  `dialnorm=auto` run ITU-R BS.1770-4 gated loudness (K-weighting designed analytically, so
  44.1 and 32 kHz work too) and negate it. A/52 predates BS.1770 by eleven years and leaves
  the measurement open; every modern delivery spec that fills the gap names BS.1770, so that
  is what this measures. The 1 kHz/−20 dBFS calibration reads −19.99 LKFS, matching FFmpeg's
  `ebur128` to 0.01 LU.
- **Downmix levels** — `cmixlev`/`surmixlev` in AC-3, and in E-AC-3 the whole `mixmdate`
  group (`dmixmod`, the Lt/Rt and Lo/Ro centre and surround levels, `lfemixlevcod`), with the
  `strmtyp == 0x0` gate so a dependent substream carries only the level group.

Verified against the oracle rather than against the bits alone: `tools/check_drc.py` runs 22
checks in which a decode that *applies* the metadata is compared against one that ignores it
(`ffmpeg -drc_scale`, `-heavy_compr`, `-ac 2`), so a stream carrying dead metadata fails.
Measured: 5.24 dB of cut on loud passages, 5.63 dB of boost on quiet ones, programme range
39.0 → 28.1 dB; the `compr` ceiling holds across hard transitions; every downmix level code
moves FFmpeg's own fold-down by the dB Tables 5.9/5.10 specify, to 0.01 dB. `tools/drc_ref.py`
is an independent transcription of Tables 7.29/7.30 as arithmetic-shift lookups, so the
bit-packing has a second opinion. One honest gap: FFmpeg's Annex E header parser *skips* the
compression word, so E-AC-3 `compr` has no external oracle and is covered bit-by-bit instead.

See [docs/RESEARCH.md](docs/RESEARCH.md) for the research summary, architecture, and
roadmap.

## Ground rules

- **Clean-room:** every table and algorithm is transcribed from ATSC A/52:2018 — or, for
  the object layer, from ETSI TS 103 420 and TS 102 366 — with its section number cited in
  a comment. Open-source encoders are consulted for architecture lessons only; no code is
  ever copied. (The one exception is normative by construction: TS 103 420 ships its JOC
  Huffman tables *as* a C file, so that file is the standard, not an implementation of it.)
- **FFmpeg is an oracle, not a dependency:** the installed `ffmpeg`/`ffprobe` CLI validates
  our output (`ffmpeg -v error -err_detect crccheck+bitstream+buffer+explode -i out.ac3 …`);
  nothing links against it.
- **vcpkg supplies test/tooling packages only** (currently Catch2). **Qt is a prebuilt
  dependency**, never a vcpkg port: `cmake/FindQt6.cmake` widens `CMAKE_PREFIX_PATH` to the
  standard install roots (`C:/Qt/6.x/msvc2022_64`, `~/Qt`, `/opt/Qt`, …) and then defers to
  Qt's own config package. `-DCMAKE_PREFIX_PATH=…` or `-DQt6_DIR=…` always wins.
- **Warnings are errors** (`ac3::warnings`, linked privately into every first-party target).
- **Standards documents are not redistributed.** `docs/spec/` is gitignored. The table
  generators in `tools/` read from it, so fetch the three free documents first — ATSC
  A/52:2018, ETSI TS 102 366 (EMDF is Annex H) and ETSI TS 103 420 plus its companion
  archive `ts_103420v010201p0.zip`, which is where the JOC Huffman tables actually live —
  and extract each PDF to page-marked text (`===== PDF PAGE n =====`) beside it.

## Building

Requirements: Visual Studio 2026 (MSVC), CMake ≥ 3.28, Ninja, a
[vcpkg](https://github.com/microsoft/vcpkg) checkout with `VCPKG_ROOT` set, and — for the
GUI — a prebuilt Qt 6.5+ kit (auto-detected; verified against 6.8.3 msvc2022_64).

From a *Developer PowerShell for VS 2026*:

```powershell
cmake --preset debug        # or your user preset inheriting it
cmake --build --preset debug
ctest --preset debug
```

Options: `AC3FORGE_BUILD_CLI`, `AC3FORGE_BUILD_GUI`, `AC3FORGE_BUILD_TESTS` (all `ON`).
Configure with `-DAC3FORGE_BUILD_GUI=OFF` to build without Qt.

## Layout

```
cmake/          FindQt6.cmake (prebuilt-Qt discovery), CompilerWarnings.cmake
src/lib/        ac3::forge — the whole codec, GUI-free
  include/ac3/  public headers: core/ encoder/ decoder/ meta/ spatial/ analysis/
                oba/ emdf/ sinks/ io/ capture/  (oba/ = object-based audio:
                OAMD, JOC, the Atmos encoder; emdf/ = the TS 102 366 Annex H
                container; encoder/plan.hpp = what both front ends encode from)
  src/          implementation
src/cli/        ac3cli — command-line front end
src/gui/        ac3gui — Qt6 Quick front end (QML module "Ac3Forge")
tests/          Catch2 unit tests; golden/ vectors generated by tools/
tools/          Python generators (spec-table extraction, golden vectors,
                the FFmpeg quality race) and the sine analysis harness
docs/           RESEARCH.md plus the full research briefs and verification records
```
