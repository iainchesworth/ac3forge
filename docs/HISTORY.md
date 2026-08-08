# Implementation history

How the codec was built, in the order it was built. This is a record of what was implemented
and what evidence closed each step, kept out of the README because a landing page is not a
development log. Nothing here supersedes the current [capability and limitation
tables](../README.md#what-it-does) — where the two disagree, the README is right and this file
is stale.

Milestone numbering is as it was used during development. Milestone 4 was folded into 5.

## Milestones 0–2 — a valid syncframe

The encoder emits AC-3 syncframes carrying 2/0 digital silence at any legal bit rate and
sample rate.

`ac3cli silence out.ac3` produces a stream that FFmpeg strict-decodes
(`-err_detect crccheck+bitstream+buffer+explode`) with zero errors to bit-perfect silence, and
that an independent from-spec bitstream parser rates conformant — including the §5.5 layout
constraints (padding placed in in-block skip fields) and both CRC words. `crc1` precedes the
region it covers, so it is solved with a GF(2) polynomial inverse rather than computed
forward.

## Milestone 3 — MDCT and the KBD window

The 512-point Kaiser-Bessel-derived window is generated at compile time from the KBD formula
and reproduces every value of Table 7.33 exactly at the spec's 5-decimal rounding. The forward
MDCT matches independent numpy goldens to ≤ 1e-10. A 50%-overlap TDAC round trip through the
*normative* §7.9.4.1 decoder inverse reconstructs the input to ≤ 1e-10, which locks the
window, both transforms and the −2/N ↔ ×2 level convention together rather than one at a time.

## Milestone 5 — real audio

`ac3cli sine out.ac3` produces AC-3 that FFmpeg strict-decodes to a 999.93 Hz sine at exactly
the target amplitude (+0.000 dB) with 88.3 dB SNR.

The pipeline: windowed MDCT → 25-bit fixed coefficients → D15 exponents mirroring the decoder
→ the bit-exact §7.2.2 integer bit-allocation engine → binary SNR-offset search → §7.3
mantissa quantization with cross-channel grouping → packing and CRCs. The allocation engine
was validated against an independent Python transcription of the spec pseudocode at zero
tolerance. Tables 7.6–7.16 are script-extracted from the spec text with self-verification, as
every table before them was.

## Milestone 6 — 5.1, LFE, and the in-repo decoder

Every audio coding mode (mono through 3/2) plus LFE, at all three sample rates. Exact 44.1 kHz
CBR arrives via Bresenham alternation between the two Table 5.18 frame lengths.

The in-repo decoder, built on the same normative core, reaches float32-precision PCM parity
with FFmpeg's decoder on identical streams: max sample difference 7.9e-6, about −102 dBFS. A
5.1 encode with a different tone per channel decodes through FFmpeg with every channel
carrying its own tone, verifying channel order end to end.

## Milestone 7 — the quality layer

Per-block exponent strategy selection (§8.2.8: D45/D25/D15 chosen by reuse span, triggered by
variation), 2/0 rematrixing (§7.5.3 minimum-power rule, with the decoder-side undo), and
bit-rate-aware bandwidth defaults.

This is the point at which output quality passed FFmpeg's encoder on the SNR metric. Current
numbers and method are in the [README](../README.md#how-it-is-validated); `ac3cli encode`
gained arbitrary stereo WAV input here. Decoder parity held on rematrix-active material at max
difference 1.1e-5.

## Milestones 8–9 — space, and getting it to a receiver

The spatial layer (`src/lib/src/spatial/`) places mono objects on the ITU 5.1 ring by
energy-normalized 2D VBAP with per-block gain ramps and explicit LFE sends. `ac3cli orbit`
renders a tone circling the listener into 5.1 AC-3. An end-to-end test parks the object at
each speaker in turn and asserts the decoded energy follows it: C → L → SL → SR → R.

The IEC 61937 packer (`src/lib/src/sinks/`) wraps frames into S/PDIF bursts byte-exact against
FFmpeg's `spdif` muxer. `ac3cli spdif` emits them as a PCM16 WAV; played bit-exactly through a
passthrough output, a receiver locks on and lights its Dolby Digital indicator.

## Live capture

`ac3::capture` enumerates every active input endpoint plus every render endpoint as a loopback
source, and streams interleaved float samples through a lock-free SPSC ring into the encoder.
Verified on hardware: a 1 kHz tone played through the speakers, captured via loopback, encoded
and decoded back at exactly 1000.0 Hz with zero ring overruns. Loopback gaps — a render
endpoint delivers nothing while the machine is silent — are filled against a QPC timeline so
the stream stays continuous.

`ac3cli devices` lists endpoints and `ac3cli record` captures straight to AC-3.

## Exclusive-mode passthrough

`ac3::sinks::PassthroughSink` opens a render endpoint in WASAPI exclusive mode with a
`KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL` format, including the documented
buffer-alignment retry, and streams bursts from a lock-free queue on an MMCSS "Pro Audio"
thread. Exclusive mode is mandatory: the shared-mode engine would mix, resample or
volume-scale the bursts and destroy the bit pattern.

`ac3cli outputs` probes every endpoint twice, for AC-3 and for plain exclusive PCM, so an
unavailable device reports *why* — "cannot bitstream" (an analog output) as against "no
exclusive access" (disabled or in use).

This has never been confirmed against bitstreaming hardware; see the
[README](../README.md#verification-gaps).

## Channel coupling

Above the coupling frequency the full-bandwidth channels stop carrying their own coefficients
and share one coupling channel plus per-band coordinates. This is the tool that makes 5.1
viable well below 448 kbit/s. `ac3cli sine … 51c` and `ac3cli encode … couple` enable it.

FFmpeg strict-decodes coupled 5.1. A targeted probe confirms the envelope is genuinely
preserved: a channel carrying a 12 kHz tone stays 113 dB above a silent one in that band,
while the region below the coupling frequency is bit-for-bit untouched. The in-repo decoder
reads coupling too — strategy, banded coordinates, phase flags, leak parameters — so coupled
AC-3 round-trips in process.

## E-AC-3

`ac3cli eac3-sine` emits bsid-16 frames carrying real audio in stereo, 5.1, 7.1, 5.1.2, 5.1.4
and 7.1.4.

E-AC-3 is a different container rather than an AC-3 variant: no `crc1`, an arbitrary 11-bit
`frmsiz` instead of a size table (so the 44.1 kHz padding alternation disappears), and
exponent strategies for all six blocks hoisted into a frame-level `audfrm`. Layouts wider than
5.1 ride in dependent substreams beside a self-sufficient 5.1 bed, each with a Table E2.5
`chanmap`; per §E3.8.2 the channels that collide with the bed replace it and the rest extend
the layout.

The decoder followed: the whole of Tables E1.2/E1.3/E1.4, dependent substreams, `chanmap` and
the §E3.8.2 render, at float32-precision parity with FFmpeg (max difference 1.4e-5) on every
layout FFmpeg will read — and reading FFmpeg's own encoder output as well.

That last part was the point. 7.1.4 needs two dependent substreams and FFmpeg refuses any
frame with `substreamid != 0`, proven exhaustively across hand-rolled MKV, FFmpeg Matroska,
MPEG-TS and MP4. A decoder under our control is what closes that gap.

## Annex E coding tools

Spectral extension (§E3.6), the adaptive hybrid transform with gain-adaptive quantization
(§E3.4), and Annex E coupling (§E3.3), each opt-in per `FrameConfig`. The JOC Huffman tables
needed by the object layer were generated here too: TS 103 420 Annex A.1 gives only their
names, modes and types and ships the trees in the companion archive as `ts_103420_tables.c`,
so `tools/gen_joc_tables.py` inverts that file — decoder trees in, encoder codewords out — and
refuses to write unless every tree is a complete prefix code.

The in-repo decoder does not read any of these three. For streams using them, FFmpeg is the
only oracle.

## Dolby Atmos objects (ETSI TS 103 420)

`ac3cli atmos out.ec3` sends objects orbiting the room at different heights and rates, and
ffprobe reports `eac3 (Dolby Digital Plus + Dolby Atmos), 48000 Hz, 5.1(side)` — the same
shape real DD+ Atmos files probe as.

There are no extra coded channels. The objects are panned into a 5.1 bed that any decoder
plays unchanged, and beside it ride two payloads in an EMDF container (TS 102 366 Annex H)
tucked into a block skip field: OAMD saying where each object is, and JOC saying how to pull
them back out as a per-band matrix over the five downmix channels. Dolby's own DD+ JOC streams
carry the container in a skip field with `auxdatae` clear rather than in the aux field; ours
match, checked against their reference content.

This is also why discrete 7.1.4 was a dead end as a delivery format: real 7.1.4 is JOC over a
5.1 bed, not twelve channels, and no shipping profile allows the two dependent substreams the
discrete layout would need.

The reconstruction matrix is the minimum mean-square estimate `M = P Dᵀ (P D Dᵀ + εI)⁻¹`.
Because the encoder built the downmix it knows `D` exactly instead of estimating it, which
makes the solve near-exact for well-separated objects.

The syntax was checked field-for-field against Dolby's Reference Player and Dolby Media
Encoder as oracles. That diffing found several real bugs: the skip-field carriage above,
`codecdatae=0`, a dynamic-object-only programme with the LFE as an object but not a JOC
output, and metadata flag arrays transmitted index-0-first. It left the frame headers and
container matching Dolby's byte-for-byte on the fields that matter.

Two limits established here are structural and remain: objects sharing a direction cannot be
separated, and Dolby's decoder will not treat these as objects because the stream is not
signed with its key. Both are in the [README](../README.md#verification-gaps).

## Metering and analysis

`ac3/analysis/` meters audio the way a console does: peak with instant attack and a 20 dB/s
fallback, a 1.2 s hold marker, RMS over a 300 ms integration, plus exact whole-signal
statistics and the Gerzon energy vector over the BS.775 ring.

Both front ends draw from it, including for where a level sits on the bar, so a printed figure
and a moving needle cannot disagree. `ac3cli levels` reports any WAV or AC-3 file channel by
channel; `encode`, `decode`, `sine` and `orbit` print the same table when they finish; `record`
meters live. The GUI gained a channel-levels card that relabels itself for the active layout
and a soundfield view. Feeding the meters meant widening both front ends to 1–6 channel WAV
input, with the WAV↔A/52 permutation moved into the library rather than copied into each
caller.

`ac3gui --smoke` and `--smoke-record` drive the file and live-capture paths headlessly and
report what the meters did, so "the display is wired to the audio" is checkable rather than a
screenshot.

## The metadata layer

Everything above decodes; this is what makes it *play* right. An AV receiver reads exactly
these bits to set level, compress dynamics and fold down, and until this point they were all
zero. `dynrng`, `compr`, a measured `dialnorm`, and the downmix levels — see the
[README](../README.md#metadata) for what each one does here.

Verified against the oracle rather than against the bits alone: `tools/check_drc.py` runs 22
checks in which a decode that *applies* the metadata is compared against one that ignores it
(`ffmpeg -drc_scale`, `-heavy_compr`, `-ac 2`), so a stream carrying dead metadata fails.
Measured: 5.24 dB of cut on loud passages, 5.63 dB of boost on quiet ones, programme range
39.0 → 28.1 dB; the `compr` ceiling holds across hard transitions; every downmix level code
moves FFmpeg's own fold-down by the dB Tables 5.9/5.10 specify, to 0.01 dB.
`tools/drc_ref.py` is an independent transcription of Tables 7.29/7.30 as arithmetic-shift
lookups, so the bit-packing has a second opinion.

One gap found here and still open: FFmpeg's Annex E header parser skips the compression word,
so E-AC-3 `compr` has no external oracle and is covered bit-by-bit instead.

## Since

- The Matroska muxer (`src/matroska/`), deliberately independent of `ac3::forge`.
- `ac3::io::scan`, so a muxer derives format, packet boundaries, sample rate and channel count
  from the bitstream rather than being told.
- `ac3cli` dispatch moved to a single command table, so an argv index cannot be quietly wrong.
- This documentation, and the `examples/` targets behind it.
