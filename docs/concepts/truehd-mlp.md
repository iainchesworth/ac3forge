# TrueHD & MLP

[Concepts](index.md) introduced AC-3, E-AC-3 and Atmos-over-E-AC-3 as one lineage, each
building on the last. Dolby TrueHD is not a fourth link in that chain — it's a second,
unrelated lineage that happens to also carry Dolby Atmos. This page explains what makes it
different, how Atmos rides inside it (which is *not* how Atmos rides inside E-AC-3), and what
this project currently knows versus still has to work out before it can be built.

!!! note "Status: framing landed; block_data()'s DSP primitives started, not yet wired to a bitstream"
    `ac3::mlp` (`src/lib/include/ac3/mlp/`, `src/lib/src/mlp/`) implements `mlp_sync`'s
    `check_nibble` and `major_sync_info()` (`sync.hpp`) and `restart_header()`
    (`restart_header.hpp`), both built and round-trip tested against the confirmed syntax below.
    `substream_directory`, `substream_segment()`, `block()`'s own header and `EXTRA_DATA()` are
    not implemented yet.
    
    For `block_data()` itself - the one piece neither Dolby document specifies - two of its
    three DSP stages now exist as standalone, round-trip-tested primitives, independent of
    Dolby's exact wire format (which remains unknown): `matrix.hpp` (the lossless
    Primitive-Matrix-Quantiser cascade) and `rice.hpp` (Golomb-Rice entropy coding - a
    well-tested stand-in, now known NOT to be MLP's actual mechanism; see [What "Lossless Coding
    for Audio Discs" adds](#what-lossless-coding-for-audio-discs-adds) - real MLP uses
    block-adaptive Huffman tables). The lossless IIR/FIR predictor loop (Figs. 10/11 in the AES
    papers) is the remaining DSP primitive; actually packing any of this into `block_data()`'s
    real bitstream still needs a source more precise than the paraphrased patent/paper
    descriptions available so far. See [What's confirmed versus what's still
    open](#whats-confirmed-versus-whats-still-open).

## A different lineage: lossless, not perceptual

AC-3 and E-AC-3 are **perceptual** codecs: they throw away detail a listener is unlikely to
notice, using a transform (MDCT) and a psychoacoustic bit-allocation model, the same broad
family of technique as MP3 or AAC. Dolby TrueHD is built on **MLP** (Meridian Lossless
Packing), a completely different approach: **lossless** compression, using adaptive prediction
filters and lossless matrixing so that a decoder reconstructs the *exact* original PCM samples,
bit for bit. Nothing about it derives from or extends AC-3's transform-coding design.

```mermaid
graph LR
    subgraph "Perceptual (AC-3 / E-AC-3 / Atmos)"
        P1["PCM audio"] --> P2["MDCT transform +<br/>psychoacoustic model"] --> P3["Smaller, lossy<br/>bitstream"]
    end
    subgraph "Lossless (TrueHD / MLP)"
        L1["PCM audio"] --> L2["Adaptive prediction +<br/>lossless matrixing"] --> L3["Smaller, but exactly<br/>reconstructible bitstream"]
    end
```

One practical consequence: because reconstruction must be *exact*, TrueHD has no tolerance for
the small floating-point differences this project's AC-3/E-AC-3 path already accepts (see
[Validation](../verification.md)). Every step of the core algorithm has to be integer/fixed-point
arithmetic, reproducible identically across compilers and architectures.

## Bitstream organization

TrueHD has two levels of structure. Externally, the stream is a sequence of **access units**,
each beginning with an **MLP Sync** — either a lightweight *minor sync* (just enough to time and
size the access unit) or, roughly once every 128 access units, an expanded *major sync*
carrying everything a decoder needs to start from scratch. Internally, each access unit carries
one segment per **substream**, and each substream is a sequence of **blocks**, some of which
begin with a **restart header** — the actual point decoding can (re)start from.

```mermaid
graph LR
    AU["Access unit<br/>(MLP Sync + one segment per substream)"] --> S0["Substream 0 segment"]
    AU --> S1["Substream 1 segment"]
    AU --> S2["..."]
    S0 --> B0["Blocks<br/>(one may carry a restart header)"]
```

An audio frame is 40 multichannel samples at 48 kHz (scaling with rate: 80 at 96 kHz, 160 at
192 kHz) — far shorter than E-AC-3's 1536-sample frame. Delivery is smoothed through a shared
encoder/decoder FIFO (minimum 120,000 bytes) so a lossless stream's inherently variable data
rate stays within an 18.0 Mbps peak, using `input_timing`/`output_timing` fields the encoder is
responsible for computing correctly — this is a real encoder obligation, not just bitstream
framing.

## Presentations: 2, 6, 8, and up to 32 channels

A single TrueHD bitstream can carry several **presentations** at once — alternative renderings
of the same programme for decoders of different capability. `channel_meaning()` describes a
2-channel, 6-channel and 8-channel presentation inline (dialogue normalization, mix level,
channel assignment for each); `substream_info` says which substream(s) each one needs. A
low-capability decoder plays the narrowest presentation it understands and ignores the rest —
the same "ignore what you don't recognise" backward-compatibility principle E-AC-3 uses for
Atmos, just applied to whole alternative channel counts instead of a side-channel.

Beyond 8 channels, an optional `16ch_channel_meaning()` structure (despite its name) describes
up to 32 channels — its channel-count field is 5 bits, one less than the count, so `11111` means
32. This is where TrueHD's channel space actually reaches the "up to 32 channels" and 14.2.8
layouts Dolby has published, well beyond anything `ac3::plan::LayoutId` currently models.

## Atmos in TrueHD: discrete channels, not JOC

This is the important structural difference from [Atmos & JOC](atmos-joc.md). Atmos-over-E-AC-3
exists because E-AC-3 only has room for a 5.1 bed plus a side channel of matrix coefficients —
JOC parametrically reconstructs objects from that bed, and two objects that panned identically
into the bed can't be perfectly separated back out. TrueHD has no such constraint: it's
lossless, with headroom for up to 32 channels, so **objects just ride as their own discrete
channels** — no downmix, no parametric reconstruction, no shared-direction ambiguity.

`16ch_channel_meaning()` says exactly what's in those channels: ordinary loudspeaker feeds,
channels of a fixed "Intermediate Spatial Format" production/exchange bed (three defined
layouts, 10/14/15 channels), and/or a count of trailing dynamic-object channels — in that order,
mixed and matched via a content-description bitmask.

```mermaid
graph LR
    subgraph "Atmos over E-AC-3 (JOC)"
        O1["Object audio + position"] --> P1["Panned into 5.1 bed"]
        O1 --> J["JOC coefficients<br/>(how to reconstruct)"]
        P1 --> E1["E-AC-3 bitstream"]
        J --> E1
    end
    subgraph "Atmos over TrueHD"
        O2["Object audio + position"] --> C["Encoded as its own<br/>discrete lossless channel"]
        C --> E2["TrueHD bitstream<br/>(bed channels + object channels)"]
    end
```

## What's confirmed versus what's still open

Two Dolby documents (see [References](#references)) together fully specify the bitstream's
**framing and metadata**: every field of `access_unit()`, `major_sync_info()`,
`channel_meaning()`/`16ch_channel_meaning()`, `substream_directory`, `substream_segment()`,
`block()`, `restart_header()` and `EXTRA_DATA()`; all three CRC/parity schemes; the FIFO timing
model; and — critically — the static/structural side of how Atmos objects are described
(channel counts, assignment, ISF bed choice). None of that is guesswork.

What neither document specifies:

- **The core compression algorithm.** `block_data()`'s actual matrixing coefficients,
  prediction-filter computation and entropy/Huffman coding of residuals are explicitly out of
  scope of both Dolby documents ("does not address details of the core audio encoding and
  decoding algorithm").
- **Per-frame dynamic object metadata.** The channel-layout side of Atmos-in-TrueHD is fully
  documented (above), but *where an object actually is, moment to moment* isn't — that almost
  certainly rides in the one described extension point, `EXTRA_DATA()`, but its internal payload
  format for object positions is proprietary and undocumented in either source.

## How the missing pieces will be sourced

[CONTRIBUTING.md's clean-room rule](https://github.com/iainchesworth/ac3forge/blob/main/CONTRIBUTING.md#the-clean-room-rule)
— "the constraint the whole project rests on" — requires every table and algorithm to be
transcribed from a published standard, with open-source encoders like FFmpeg usable only for
architecture lessons, never as something to transcribe from, "because the spec contains every
table." That premise holds for AC-3/E-AC-3, where A/52 is a complete public spec. It does not
hold for MLP's core algorithm: unlike JOC (which has one narrow exception — TS 103 420 ships its
Huffman tables as a literal C file that counts as the standard itself), **no public standard for
MLP's core algorithm exists at all**.

Given that gap, the two missing pieces above will be independently derived from academic papers
and expired/public patent literature on Meridian Lossless Packing — cited in code comments the
same way A/52 section numbers are cited elsewhere in this codebase — rather than by reading
FFmpeg's `mlpenc.c`/`mlpdec.c` source. FFmpeg remains usable exactly as it already is for
AC-3/E-AC-3: as a black-box decode oracle to check output against, never as a source to read.

### Candidate sources for the core algorithm

Two independent primary families exist: academic papers by MLP's original inventors, and the
foundational (now-expired) patent family. Three of the papers and both key patents have now
been read in full (the user supplied PDFs for the papers directly); one further paper is a
newly-identified candidate, found only via the ones already read.

**Academic papers (primary):**

- **Read.** Gerzon, M. A.; Craven, P. G.; Stuart, J. R.; Law, M. J.; Wilson, R. J. — *"The MLP
  Lossless Compression System for PCM Audio"*, AES 17th International Conference on
  High-Quality Audio Coding, Florence, 1999 September; revised 2003 December, published in
  *J. Audio Eng. Soc.*, Vol. 52, No. 3, 2004 March, pp. 243–260. The fullest of the three papers
  read this session — see [What the AES papers
  add](#what-the-aes-papers-add-to-the-patent-account) below.
- **Read.** Stuart, J. R.; Craven, P. G.; Gerzon, M. A.; Law, M. J.; Wilson, R. J. — *"MLP
  Lossless Compression"*, AES 9th Regional Convention, Tokyo. Explicitly "an abbreviated version
  of [the JAES paper above]" — same content, same figures, no material technical detail beyond
  it. Worth keeping as a source only because it independently confirms decoder complexity
  figures (≈27 MIPS for 2-channel @192 kHz, ≈40 MIPS for 6-channel @96 kHz) the JAES paper
  doesn't state.
- Craven, P. G.; Law, M. J.; Stuart, J. R. — *"Lossless Compression Using IIR Prediction
  Filters"*, AES 102nd Convention, Munich, March 1997 (Preprint 4415). **Dropped from the active
  reading list** — its own citation trail shows it isn't the source of the actual predictor
  architecture: the JAES paper's §4.4 attributes Figs. 10/11 (the real algorithm) to the patent
  (their ref [8]), and cites this preprint (their ref [9]) only for motivating *why* IIR matters
  at high sample rates, content the JAES paper already restates. No accessible copy was found
  this session either, so it isn't worth chasing further unless a specific gap shows up later
  that the patent + JAES paper's Figs. 8–11 don't cover.
- **Read.** Craven, P. G.; Gerzon, M. A. — *"Lossless Coding for Audio Discs"*, *J. Audio Eng.
  Soc.*, Vol. 44, No. 9, pp. 706–720, 1996 September. The deepest and earliest of the three
  papers read this session — see [What this paper adds](#what-lossless-coding-for-audio-discs-adds)
  below. It significantly sharpens (and partly corrects) the entropy-coding picture the later
  papers left ambiguous.

**Patents (primary, algorithmic detail, foundational IP) — read:**

- **US 6,891,482 B2** — *"Lossless coding method for waveform data"* (Craven & Gerzon;
  originally Meridian Lossless Packing Limited, later Dolby Laboratories Licensing Corp.;
  priority 1995; expired). The core MLP patent. Describes FIR prediction filters with rational
  coefficients, a rounding quantizer with optional noise shaping inside the prediction loop,
  n×n matrix quantizers for multichannel decorrelation, and a final Huffman entropy-coding
  stage — a real algorithmic skeleton, not just claims boilerplate.
- **US 7,193,538 B2** — *"Matrix improvements to lossless encoding and decoding"* (Gerzon &
  Craven, same assignee lineage). Follow-on patent specifically about the matrixing stage.
- Same-invention family, for reference if more filing-history detail is needed:
  **WO1996037048A2** (the international application both papers' reference lists cite directly
  as PCT/GB96/01164, filed 1996 May), **US20040125003A1**, **CA2585240C**.

**Tertiary (orientation only — not citable as an algorithm source per the clean-room rule):**
Wikipedia's Meridian Lossless Packing page, the Hydrogenaudio wiki entry, and Robert C. Maher's
"Lossless Compression of Audio Data" textbook chapter. None describe the algorithm in
implementable detail; useful only for vocabulary and pointing at further primary citations.

### What the two patents actually describe

Read via Google Patents' rendering of each (not yet cross-checked against the primary USPTO
PDF — see the caveat below).

!!! warning "Caveat before this drives any code"
    The quotes below are an AI-summarized read of Google Patents' web rendering, not a direct
    read of the primary USPTO document. Good enough to plan against; not good enough to cite
    verbatim in a code comment without first checking the fragment against the actual patent
    PDF (`image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/6891482` and `/7193538`) —
    the same discipline `core/crc16.hpp` and friends already apply to A/52 section numbers.

**Prediction (US 6,891,482 B2).** The encoding filter is
`1 / [1 + B(z⁻¹)] × [1 + A(z⁻¹)]`, where A and B are FIR filters with rational coefficients
(shared denominator, e.g. all coefficients of the form m/16 or m/64). A worked third-order
example bounds each coefficient to a range like `-192 ≤ 64a₁ ≤ 192`. The loop structure: a
summing node feeds an integer rounding quantizer with unity step size (so its output is always
integer-valued and exactly representable), and that output feeds back through filter B to the
same summing node. Per-block state variables are the first *n* input and output samples (for an
*n*th-order filter), transmitted so a restart point can initialize the loop exactly.

**Entropy coding (US 6,891,482 B2).** Not a single fixed Huffman table: 17 tables, selected
per block by the block's peak signal level (signal-adaptive, Laplacian-PDF-shaped). Only the 4
most-significant *varying* bits of the residual are Huffman-coded; the remaining
less-significant bits are sent unaltered/uncoded after the Huffman word. Quoted overhead versus
an optimal entropy coder: "typically less than optimal by only about 0.1 to 0.3 bit/sample."

**Matrixing (US 7,193,538 B2).** Multichannel decorrelation is a cascade of **Primitive Matrix
Quantisers (PMQs)**, each modifying one channel by adding proportions of the others while
staying exactly invertible — restricted to matrices with determinant 1, which is why encoded
coefficients get scaled (e.g. by 4/3) and the decoder applies the compensating inverse scale.
This directly explains several `restart_header()` fields the bitstream-description PDF names
but doesn't define the *purpose* of:

  - `dither_seed` (u(23)) — PMQ quantization needs synchronized dither between encoder and
    decoder to stay lossless; the patent calls this "diamond dither" (sum of two independent
    rectangular-PDF sequences → triangular PDF), seeded and regenerated identically on both
    sides from this field.
  - `max_shift` (s(4)) — the patent's `output_shift`: downmix coefficients can push the result
    past 24-bit range, so the encoder pre-scales by a power of two and the decoder applies the
    compensating shift before clipping, rather than constraining coefficients to unacceptably
    low levels.
  - `max_lsbs` (u(5)) — the patent's LSB-bypass path: a PMQ with a sub-unity gain coefficient
    (e.g. ±½) produces one extra bit of precision the main path can't carry; that bit rides
    separately and gets shifted back in in the decoder.
  - `lossless_check` (u(8)) — an 8-bit parity word the encoder computes over the *actual*
    (multichannel) or *simulated* (downmix) output before transmission, so a decoder can detect
    an algorithmic mismatch rather than silently losing losslessness; each channel's word is
    bit-rotated by its channel number so an identical error across channels is still caught.
  - `ch_assign[]` (u(6) per channel) — recovers channel order after the encoder permutes
    channels via partial pivoting (favouring the first two channels for the 2-channel downmix
    substream).

None of this is final — it needs a direct read of the patent PDFs themselves (not just an AI
summary of them) before it's solid enough to write `block_data()` against. But it's enough to
know the shape of the remaining work, and it already explains several `restart_header()` fields
(see [restart_header.hpp](https://github.com/iainchesworth/ac3forge/blob/main/src/lib/include/ac3/mlp/restart_header.hpp),
which packs them at their documented positions with this same provisional-semantics caveat).

### What the AES papers add to the patent account

The JAES 2004 paper (Gerzon, Craven, Stuart, Law, Wilson) is the fullest of the three read this
session — the Tokyo paper is an earlier, strictly shorter version of the same material. Both
independently corroborate the patents' account rather than contradicting it, and add systems-level
framing the patents (written for claims, not exposition) don't bother with:

- **The matrix stage is confirmed from a second angle.** Fig. 4 shows the same structure as the
  patent's PMQ cascade, just in the paper's own notation: each "affine transformation" modifies
  one channel by adding a quantized linear combination of the others (`m_coeff[1,2]`,
  `m_coeff[1,3]`, `m_coeff[1,4]` feeding a summing node and quantizer `Q`), with the encoder and
  decoder sides being exact mirrors of each other. "A trivial (though important) example" is a
  stereo mix rotating from L/R to sum/difference.
- **Prediction is IIR-capable up to 8th order**, confirmed explicitly ("The encoder is free to
  select IIR or FIR filters up to eighth order from a wide palette") — the patent's worked
  example was only 3rd-order, so this sets the real upper bound. Figs. 8–11 give the same
  encoder/decoder block structure as the patent's `1/[1+B(z⁻¹)] × [1+A(z⁻¹)]`, framed as: a
  conventional FIR/IIR predictor doesn't survive lossless round-tripping because IIR filters
  with fractional coefficients "cannot be exactly implemented since representation of the
  recirculating signal requires an ever-increasing word length" — the fix is quantizing *inside*
  the feedback loop (their Fig. 10/11) so only finite-precision values ever recirculate.
- **Entropy coding is framed around Rice coding, not "17 Huffman tables" as such** — though the
  two aren't necessarily in tension: "audio signals often have a Laplacian distribution... The
  Rice code provides a simple and near-optimal way of encoding such a signal... and has the
  advantage that encoding and decoding need not use tables," but "the Rice code is not used
  unconditionally, the MLP encoder may choose from a number of entropy coding methods,"
  including plain PCM as a fallback for pathological "peak-level RPDF (all values equally
  probable) white noise" the encoder shouldn't try to compress. A parameterized Rice code and a
  small family of matched Huffman tables are two ways of describing the same near-Laplacian-optimal
  coding — the patent's account may just be describing a concrete table-driven implementation of
  what these papers describe more abstractly. Worth resolving explicitly once `block_data()`
  implementation starts, not assumed equivalent without checking.
- **`lsb_bypass` is a real, named signal path in both papers' own block diagrams** (Fig. 3/21 of
  the JAES paper, Fig. 3/16 of the Tokyo paper) — independent confirmation of the patent's
  LSB-bypass account for `max_lsbs`, beyond the patent alone.
- **"Decoder lossless self-check" is listed as one of MLP's defining novel techniques** in both
  papers' §3, without bit-level detail — corroborates `lossless_check`'s purpose (the patent's
  account of *how* is still the only source for the actual mechanism).
- **Channel ceiling reconciled.** Both AES papers state MLP supports "up to 63 audio channels" —
  not 32. This isn't a conflict with the TrueHD-specific bitstream-description document's
  32-channel figure (from `16ch_channel_count`'s 5-bit, "one less than count" field): 63 is the
  general MLP architecture's ceiling (`ch_assign[]` is `u(6)`, i.e. 0–63), while 32 is the
  ceiling the *TrueHD/Blu-ray FBA profile specifically* exposes through its own narrower
  presentation-count field. The general format goes wider than what this project's target
  profile can address.
- **Concrete validation targets for Phase 4**, once there's a real encoder to check: Table 1's
  peak/average data-rate reduction figures (4 bit/sample peak @48 kHz, 8 @96 kHz, 9 @192 kHz)
  and the Tokyo paper's decoder complexity figures (≈27 MIPS for 2-channel @192 kHz, ≈40 MIPS
  for 6-channel @96 kHz) are real published numbers a working implementation should land near.

### What "Lossless Coding for Audio Discs" adds

This 1996 paper is earlier and more foundational than the other two — it develops the ideas from
first principles rather than describing a finished system, and in doing so gives real mechanism
detail the later, more polished papers only gesture at.

- **Entropy coding is genuine table-driven Huffman coding, not Rice/Golomb coding** — this is a
  real correction to the working hypothesis in [What the AES papers
  add](#what-the-aes-papers-add-to-the-patent-account) above. The paper walks through a full
  worked example: a synthetic distribution split into a "near" range and a "far" range, one bit
  spent to say which range a value falls in, then a fixed-width field for the value within that
  range — and a second worked example with an explicit input-value-to-codeword table for a
  seven-level distribution, netting real bits-per-sample figures against plain PCM. Critically,
  it also confirms the adaptive-table mechanism the patent's "17 tables" almost certainly refers
  to: the encoder keeps "a selection of decoding tables available" and picks whichever suits the
  current block's sample-value distribution, block by block. `ac3::mlp::rice` (Golomb-Rice) is
  therefore best understood as a well-defined, testable *stand-in* for this stage — a reasonable
  near-optimal choice for a Laplacian source, and not a wasted primitive — but not a confirmed
  match for MLP's actual mechanism, which is table-selected Huffman. Implementing a real
  MLP-shaped entropy coder needs an actual Huffman table (or table family), not just a Rice
  parameter.
- **Predictor state transmission and block-length tradeoffs are explained, not just asserted.**
  At the start of each block a restart needs the filter's own delay-memory state — "these
  internal filter data... are termed 'initialization data'" — and a higher filter order directly
  means more of this per-block overhead, which is the actual reason real designs stay around
  3rd–4th order rather than going higher (the earlier sources stated the practical order limit;
  this paper explains why it exists as a real cost/benefit tradeoff, not a rule of thumb).
- **Error containment motivates the restart-point architecture directly.** IIR predictor errors
  recirculate indefinitely once introduced, and Huffman coding's variable-length codewords let a
  single bit error desynchronize the decoder from the encoder — both problems are bounded by
  packing "full initialisation and restart information" into every block, i.e. exactly the
  `restart_header()` mechanism already implemented in `ac3::mlp`, now with a first-principles
  rationale rather than just a bit-layout table to transcribe.
- **Determinism was a first-order design constraint from the start**, not an incidental
  refinement: cross-platform bit-exact output "must give bit-exact identical outputs" between
  independently implemented encoder/decoder predictor pairs is called out explicitly, predating
  the patent's more mechanical fixed-point/rational-coefficient solution to the same problem.
- One divergence from the eventual TrueHD/Blu-ray profile, not a contradiction: this paper's own
  provisional 1996 proposal used block lengths as low as 384 samples, wider than the
  confirmed 40–160-sample blocks in the actual FBA syntax (`mlp_tables.hpp`'s
  `samples_per_access_unit`) — later tuning during MLP's evolution into TrueHD, not a conflict
  to resolve.

## v1 scope

Given the size of the gap above, initial implementation targets the fully-specified 2/6/8-channel
presentations (stereo through 7.1) and the core lossless codec — not the `16ch_channel_meaning()`
tier or Atmos, which stay deferred until the per-frame object metadata question is resolved.

!!! example "See it in code"
    - [`src/lib/include/ac3/mlp/`](https://github.com/iainchesworth/ac3forge/tree/main/src/lib/include/ac3/mlp) /
      [`src/lib/src/mlp/`](https://github.com/iainchesworth/ac3forge/tree/main/src/lib/src/mlp) —
      `mlp_sync`/`major_sync_info()` (`sync.hpp`) and `restart_header()` (`restart_header.hpp`),
      the two framing increments built so far
    - [References](#references) below — every source document this page is built from

## References

All saved under [`docs/reference/`](https://github.com/iainchesworth/ac3forge/tree/main/docs/reference)
for citation:

**Bitstream framing and metadata:**

- *Dolby TrueHD (MLP) bitstreams within the ISO base media file format* (Dolby Laboratories,
  2019) — ISOBMFF/MP4 muxing rules, `MLPSampleEntry`/`MLPSpecificBox`, access-unit-to-sample
  mapping. Does not cover the core algorithm.
- *Dolby TrueHD (MLP) high-level bitstream description* (Dolby Laboratories, 7 February 2018) —
  the full external/internal bitstream syntax `sync.hpp`/`restart_header.hpp` are built from.
  Explicitly does not cover the core audio encoding/decoding algorithm.

**Core compression algorithm** — see [Candidate sources for the core
algorithm](#candidate-sources-for-the-core-algorithm) and [What the AES papers
add](#what-the-aes-papers-add-to-the-patent-account) above for what each says; two read in full
this session and saved:

- Gerzon, Craven, Stuart, Law, Wilson, *"The MLP Lossless Compression System for PCM Audio"*,
  *J. Audio Eng. Soc.*, Vol. 52, No. 3, 2004 March.
- Stuart, Craven, Gerzon, Law, Wilson, *"MLP Lossless Compression"*, AES 9th Regional
  Convention, Tokyo.

The two patents (US 6,891,482 B2, US 7,193,538 B2) are fully public via Google Patents/USPTO and
not duplicated into `docs/reference/` — no download needed, just the patent number.

!!! note "Trademarks"
    "Dolby", "Dolby TrueHD" and "MLP Lossless" are trademarks of Dolby Laboratories. ac3forge is
    not affiliated with, endorsed by, or certified by Dolby Laboratories.

---

Back to [Atmos & JOC](atmos-joc.md), or up to the [Concepts overview](index.md).
