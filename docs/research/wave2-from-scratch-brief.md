# Building a Clean-Room AC-3 Encoder in C++23: Design Brief

## The short version

You want to write, from first principles, a library that encodes multichannel audio into AC-3 (the codec marketed as "Dolby Digital"), with stretch goals of E-AC-3 (Dolby Digital Plus) and Atmos-style object audio, plus a spatial layer where applications place and move sound sources in 3D. This is genuinely feasible, and here is the honest framing:

- **The specs are free and sufficient.** The complete normative standard (ATSC A/52:2018, with E-AC-3 as Annex E) is a free PDF, and it fully defines the bitstream and the decoder. It also includes an informative "how to build a basic encoder" chapter. Working open-source encoders (FFmpeg, Aften) were written from exactly these documents.
- **The core insight of the codec:** AC-3 is *decoder-defined*. The decoder recomputes bit allocation from a handful of transmitted parameters using exact integer arithmetic, so your encoder must contain a **bit-exact** copy of that integer model. What you get to *design* is the strategy layer: which parameters to send, when to reuse data, how to spend bits.
- **Scope:** FFmpeg's entire AC-3/E-AC-3 encoder family is 4,643 lines of C (verified count). A realistic estimate for a clean C++23 equivalent is 4,000–6,000 library LOC plus 2,000–3,000 LOC of tests/tools: roughly **4–8 focused weeks to a first valid, FFmpeg-decodable stereo/5.1 encoder**, then additional weeks-to-months for quality tuning, coupling, and E-AC-3. This is a serious multi-month project, not a weekend hack — the bulk of the difficulty is in getting the bit allocation and bitstream packing exactly right, not in the math being exotic.
- **Legal:** AC-3 patents expired in 2017 (high confidence). E-AC-3's last known US patent expired 2026-01-30 (the date is verified; "it was the last one" is community analysis, not a legal opinion). Atmos/JOC remains heavily patented into the mid-2030s — your spatial layer should render objects into channel beds internally, and the open IAMF format is the legally clean endgame for true object output. Never use the word "Dolby" in naming.

---

## 1. The standards and what they give us

Three documents, all free to download, cover everything:

| Document | What it is | Free? | Notes |
|---|---|---|---|
| [ATSC A/52:2018](https://www.atsc.org/wp-content/uploads/2021/04/A52-2018.pdf) | The master AC-3 + E-AC-3 standard (271 pages, current revision, approved 2018-01-25) | Yes | E-AC-3 is **Annex E (normative)**. Section 8 is an informative encoder guide. Verified: no newer revision exists; ATSC 3.0 moved to a different codec (AC-4). |
| [ETSI TS 102 366 V1.4.1](https://www.etsi.org/deliver/etsi_ts/102300_102399/102366/01.04.01_60/ts_102366v010401p.pdf) | The European twin — same technical content, same annex structure | Yes | Uniquely carries the **full EMDF metadata format (Annex H)** — A/52's own Annex H is a stub that defers to it. Needed later for object-audio experiments. ETSI's server 403s non-browser user agents; use a browser UA. |
| [ETSI TS 103 420 V1.2.1](https://www.etsi.org/deliver/etsi_ts/103400_103499/103420/01.02.01_60/ts_103420v010201p.pdf) | "Backwards-compatible object audio carriage using Enhanced AC-3" — the open spec for Atmos-in-DD+ (Joint Object Coding) | Yes | Decoder-only: the word "encoder" appears zero times in it (verified). |

The critical structural fact, quoted from A/52 §8.1: *"the encoder is not precisely specified. The only normative requirement on the encoder is that the output elementary bit stream follow AC-3 syntax."* Everything a decoder does is bit-exact and normative; everything about encoder decision-making is yours to design.

**What the spec hands you vs. what you must design:**

| The spec gives you (normative — transcribe exactly) | You must design (spec silent or informative) |
|---|---|
| Complete bitstream syntax with field widths (§5, Annex E) | Psychoacoustic *steering*: SNR offsets, decay/gain codes, delta bit allocation |
| Frame structure: sync word 0x0B77, 6 blocks × 256 samples = 1536/frame; §5.5 packing constraints | The SNR-offset search that fills each frame |
| Both CRC-16s, polynomial x¹⁶+x¹⁵+x²+1, including the crc1 zero-at-5/8 trick | Transient detection / block switching |
| The full integer bit-allocation model (§7.2) — pseudocode + every table | Exponent strategy selection (D15/D25/D45/reuse) |
| Exponent coding rules, mantissa quantizers and grouping (§7.1, §7.3) | Coupling strategy (whether/when/which bands) |
| The 512-point transform window as 256 literal values (Table 7.33) | Rematrixing decision rule |
| Forward MDCT equation (§8.2.3.2) and fast-FFT structure (§7.9.4) | Dither policy, input filtering, bitrate/mode selection |
| Frame-size tables for all 19 bitrates × 3 sample rates (Table 5.18) | Everything encoder-side for E-AC-3's advanced tools |

Every lookup table you need appears verbatim in the spec, so a from-spec implementation carries no code lineage from FFmpeg or any GPL/LGPL project.

---

## 2. Anatomy of an AC-3 encoder

A quick vocabulary pass: **PCM** is plain uncompressed audio samples. AC-3 transforms each channel's PCM into frequency-domain coefficients, then represents each coefficient as a **mantissa** (the value) times 2^(−**exponent**) (the scale) — a crude floating-point format where exponents double as a spectral envelope that drives bit allocation.

The pipeline, in the order FFmpeg's encoder runs it (verified against source):

1. **Framing** — buffer 1536 samples per channel (6 blocks of 256). *Easy.*
2. **MDCT** — per block, a 512-sample windowed transform (Modified Discrete Cosine Transform) with 50% overlap produces 256 coefficients per channel. *Moderate; heavy math but fully specified.*
3. **Coupling** (optional) — share high-frequency content across channels. *Hard; defer.*
4. **Rematrixing** (stereo only) — code L+R/L−R instead of L/R per band when advantageous. *Easy-moderate.*
5. **Exponent extraction and strategy** — pick per-block exponent coding (D15/D25/D45 or reuse), encode differentially. *Moderate.*
6. **Bit allocation** — run the normative integer model to decide bits-per-mantissa, searching SNR offsets until the frame is exactly full. *Hard — the heart of the project.*
7. **Mantissa quantization** — quantize to the allocated precision, with the fiddly 3-values-per-word grouping. *Moderate but detail-dense.*
8. **Bitstream packing + CRCs.** *Easy-moderate.*

**What a minimal valid encoder can defer** — this is the most liberating finding. FFmpeg's deployed, universally compatible encoder: never uses short blocks (writes `blksw=0` unconditionally — 25 years in production), shipped its first decade with no coupling, and defaults dialnorm to −31 (meaning "no level change"). Aften never implemented coupling at all. So v1 scope = **long blocks only, no coupling, rematrixing on, fixed metadata**. What you *cannot* defer is the full bit-allocation model — the decoder re-runs it, so even a "minimal" encoder contains all of it.

One correction to circulating folklore: FFmpeg's channel coupling is *not* float-encoder-only — the fixed-point encoder gained coupling in August 2011 (verified against the commit); FFmpeg's own docs are stale on this.

---

## 3. The DSP core

### MDCT and the KBD window

The window is given as 256 literal values (Table 7.33), mirrored to 512 points. Verified numerically: it is exactly the **Kaiser-Bessel-Derived (KBD) window with α=5** (Kaiser kernel length 257, β=5π) — every table entry matches within 5-decimal rounding. So generate it at full precision from the formula and use the spec table as a unit-test oracle (`scipy.signal.windows.kaiser_bessel_derived(512, 5*pi)` is an independent reference).

The forward transform (§8.2.3.2): `XD[k] = (−2/N)·Σ x[n]·cos((2π/4N)(2n+1)(2k+1) + (π/4)(2k+1)(1+α))` with α=0 for long blocks. **Gotchas:** the −2/N scale matters for absolute level, because the decoder multiplies by 2 in overlap-add to undo encoder headroom — validate with a known-amplitude sine decoded through FFmpeg. Short blocks (if you ever add them) interleave coefficients: X[2k] = first half-transform, X[2k+1] = second, so downstream stages never notice. Implement a direct O(N²) version first, then the spec's fast structure (§7.9.4): a 128-point complex FFT with pre/post twiddles `xcos1[k] = −cos(2π(8k+1)/8N)` — about 100 lines, no external FFT dependency.

### Exponents

Exponents are leading-zero counts (0–24, bigger = quieter). Strategies: D15 (one exponent per coefficient), D25 (per 2), D45 (per 4), or reuse the previous block's set; block 0 always sends fresh ones. Differentials are constrained to ±2 and pack three-per-7-bit-word as `25·M1 + 5·M2 + M3`. Shared pairs/quads must take the group's **minimum** exponent, and slew-limiting works by only ever *decreasing* exponents (forward pass `exp[i] = min(exp[i], exp[i−1]+2)`, then backward). **The gotcha that bites everyone:** the encoder must run the decoder's exponent reconstruction and use *those* decoded exponents for mantissa normalization and bit allocation — not its raw ones — or the decoder's allocation silently diverges and the stream decodes as noise.

### Bit allocation (the non-negotiable core)

Fully specified integer pseudocode (§7.2.2): power spectral density `psd = 3072 − (exp << 7)`; integration into 50 roughly-⅙-octave bands using a log-addition table; an excitation curve from two decaying "leak" state variables modeling masking; comparison against a hearing-threshold table; then per-bin `bap = baptab[clamp((psd − mask) >> 5, 0, 63)]`. Named gotchas, all verified against the spec text:

- **The `mask &= 0x1fe0` truncation must happen in exactly the specified order** (subtract snroffset, subtract floor, clamp, truncate, re-add floor).
- **The spec has a known erratum**: the `calc_lowcomp` pseudocode contains a stray semicolon (`if ((b0 + 256) == b1) ; { a = 384; }`); the universally implemented intent is `if (b0+256==b1) a=384; else if (b0>b1) a=max(0,a−64);`.
- The SNR offset (`snroffset = (((csnroffst − 15) << 4) + fsnroffst) << 2`) is the knob your rate-control loop turns: bits-used is monotone in it, so a plain binary search over ~1024 values works; FFmpeg walks down by 64 then refines with steps 64/16/4/1.
- At 44.1 kHz, frame sizes alternate by one padding word between adjacent frame-size codes — implement the alternation or your average bitrate drifts.

### Mantissas

Symmetric quantizers for bap 1–5 (3/5/7/11/15 levels), asymmetric two's-complement for bap 6–15 (5–16 bits). Grouping: three 3-level values in 5 bits (`9a+3b+c`), three 5-level values in 7 bits (`25a+5b+c`), two 11-level values in 7 bits (`11a+b`), emitted at the first member's position, spanning channels within a block. **The bit counter used inside your SNR search must share this exact grouping logic with the packer**, or frames will over/under-fill. The encoder never generates dither; it just sets a per-channel flag telling the decoder to fill zero-bit bins with noise.

### Rematrixing

Four bands (bins 13–24, 25–36, 37–60, 61–252). One flagged subtlety: **the spec contradicts itself** — §7.5.3's pseudocode uses a *minimum*-power rule while informative §8.2.6 describes a *maximum*-power rule. FFmpeg implements the minimum rule; either yields valid bitstreams (it's encoder discretion), but follow §7.5.3/FFmpeg for parity.

### Numeric strategy (the single most important design decision)

Do the front half in floating point and the back half in integers: float64 window+MDCT in the reference build, then immediately convert coefficients to **signed 25-bit fixed point** (`round(c × 2²⁴)`). From that handoff on — exponent extraction via `std::countl_zero`, smoothing, PSD, the whole §7.2 pipeline, the SNR search, quantization, packing — everything is pure int32 table-driven math, exactly as the spec mandates. Result: the entire back half is **bit-exact and platform/compiler-independent by construction**; only the MDCT can wobble by ULPs, which almost never flips an exponent and never invalidates a stream. Pin the reference build's output bytes in CI; compare optimized builds by decoded-audio SNR instead. Never let `/fp:fast` touch the reference path.

---

## 4. Legal reality

*None of this is legal advice; confidence labels reflect the verification pass.*

- **AC-3 patents: expired.** *(High confidence — verified on Google Patents and multiple independent sources.)* The last patents (US6449368, US5890106) expired in March 2017. This is why FFmpeg ships an AC-3 encoder everywhere royalty-free.
- **E-AC-3: almost certainly clear in the US, with an asterisk.** *(Medium-high confidence.)* US 7,516,064 — Dolby's Adaptive Hybrid Transform patent — is verified "Expired - Lifetime" as of 2026-01-30. However, the claim that it was *the last* E-AC-3 patent is community inference (Phoronix, 2026-01-31, explicitly "I am not a lawyer"), not an authoritative determination; the verifier found no source proving no other blocking patent exists, and Fedora is still awaiting formal legal review. Dolby also continues to file *new* adjacent patents (e.g., US 12,183,354 on DRC-metadata handling, expires 2034) — so implement only what the published spec text describes, and get a freedom-to-operate opinion before commercial E-AC-3 release.
- **Atmos/JOC: actively patented.** *(High confidence.)* Dolby marks 80–100+ patents per Atmos product; active object-audio patents verified through at least 2034 (e.g., US 10,276,172, US 12,277,942). The "~2036" figure sometimes cited is an estimate; the verified floor is 2034. TS 103 420 is free to *read*, but emitting JOC bitstreams commercially is off the table for roughly a decade.
- **The hardware Atmos gate.** *(Verified spec fact; contested conclusion.)* TS 102 366 Annex H says verbatim that the EMDF `protection_bits` calculation and `key_id` values are "implementation dependent and not defined in the present document." The one public experiment (the raress96 Rust PoC) produced JOC streams that software decoders recognize as object audio with correct 3D positions, but consumer hardware fell back to non-Atmos. **Flag:** the popular "secret Dolby MAC blocks hardware Atmos" explanation is *not established* — the PoC's own diagnostics are equally consistent with core-stream conformance problems, and no independent corroboration exists. Treat hardware Atmos light-up as unproven territory either way.
- **Trademarks never expire.** "Dolby," "Dolby Digital," "Dolby Digital Plus," "Dolby Atmos" are live registered marks. Name everything "AC-3 / E-AC-3 (ATSC A/52, ETSI TS 102 366 compliant)."
- **Clean-room hygiene.** *(Flagged: the "reading LGPL code then writing original code is copyright-safe" claim was verified as an overgeneralization.)* Clean-room process defends against copyright, not patents; NEC v. Intel validated independent reimplementation but is a single non-precedential district-court ruling, and with proven access, infringement turns on substantial similarity. The risk-managed posture: implement strictly from the spec, cite the A/52 section number in a comment for every table and algorithm, read FFmpeg/Aften only for *architecture* decisions (record what you consulted), and never transcribe code, comments, or non-spec table layouts. Since the specs contain every table, you never need to copy anything.

---

## 5. The feasibility ladder

| Rung | Target | Legal risk (2026) | Incremental effort | Verdict |
|---|---|---|---|---|
| 0 | AC-3 stereo/5.1, long blocks, no coupling | Minimal | The base project (~4–8 weeks) | **Do it** |
| 1 | AC-3 quality layer: exponent search, rematrixing, tuning | Minimal | +2–4 weeks | Do it |
| 2 | Channel coupling | Minimal | +2–4 weeks | Optional; matters below ~192 kbps 5.1 |
| 3 | Minimal E-AC-3: bsid-16 framing, arbitrary bitrates, ≤5.1 | Low (see §4 asterisk) | +2–4 weeks | Good stretch — FFmpeg proves it's a ~300-line shim over the AC-3 core |
| 4 | E-AC-3 7.1 via dependent substreams; then SPX/AHT | Low | +weeks; full tools +3–6 months | Encoder-side logic is original DSP design |
| 5 | DD+ JOC / OAMD (object metadata in the bitstream) | **High** — dense active patents; hardware engagement unproven | 1–3 months to PoC | Private research only; never distribute |
| 6 | TrueHD Atmos | No public spec exists at all | — | Not viable |
| Alt | **IAMF / Eclipsa Audio** object output | Minimal — AOMedia royalty-free | Renderer reuse + muxer | **The recommended spatial endgame** |

IAMF (finalized 2023-11-09, free spec and reference tools on GitHub; YouTube ingest since January 2025, Samsung 2025+ TVs as "Eclipsa Audio") gives your spatial ambitions a fully legal object-audio deliverable using the *same* scene model and renderer that feeds the AC-3 bed. One caution from verification: don't assume "AVRs will play my E-AC-3 over HDMI ARC" — DD+ over base ARC is device-dependent (eARC is the reliable path), and no public test report confirms self-encoded E-AC-3 on named consumer hardware. Plan a real-device test early in the E-AC-3 phase.

---

## 6. The spatial/object layer

Vocabulary: an **object** is a mono audio stream plus time-varying metadata (3D position, gain, size); a **bed** is a conventional channel-based stem (5.1, 7.1.4). The layer is a four-stage pipeline, and its one load-bearing architectural decision is that **rendering is a view of the scene, not a consuming transformation**:

```
Scene (ADM-semantics object + bed model)
  └─ Renderer (concept; VBAP panner default)
      └─ Bed mixer (5.1 bus; per-block gain ramps)
          └─ FrameSink (AC-3 encoder | WAV debug | later: E-AC-3, IAMF)
```

- **Scene model:** adopt the semantics (not the XML) of ITU-R BS.2076 "Audio Definition Model" — Objects/DirectSpeakers/HOA types, time-stamped position blocks with azimuth/elevation/distance or X/Y/Z plus size. This keeps you aligned with Atmos concepts, the ITU renderer spec, and IAMF.
- **Renderer:** write your own small **VBAP** panner (Vector Base Amplitude Panning, Pulkki 1997): choose the speaker triplet containing the target direction, solve `g = pᵀL⁻¹`, clamp, normalize so **Σg² = 1** (energy preservation — correct for loudspeakers, where signals sum incoherently). Map object "size" to MDAP (pan a cluster of virtual sources). Validate your gains numerically against **libear** (EBU's Apache-2.0 C++ implementation of ITU-R BS.2127) as a test-only oracle. Avoid libspatialaudio as a dependency (LGPL, no vcpkg port).
- **Hard rules:** objects never feed the LFE channel (the Atmos convention — LFE comes only from beds or explicit sends); distance attenuation is a pluggable source-stage policy (none/inverse/log), defaulting to none.
- **Timing:** clock automation at the AC-3 block — 256 samples (5.33 ms at 48 kHz). One gain vector per object per block, applied with per-sample **linear ramps** between blocks to kill zipper noise (audible clicks from stepwise gain changes). Six blocks feed the encoder as one frame. Expose block size as a sink-negotiated parameter, not a constant.
- **Real-time discipline:** the render thread never allocates, locks, or does I/O; control changes arrive via a pre-allocated lock-free SPSC queue; object handles are generational indices, not pointers.
- **Format agnosticism:** the render stage outputs `{bed frames, per-object gain matrices, untouched object PCM + metadata}`. The AC-3 sink reads only the bed; a future IAMF sink consumes the objects directly; a (research-only) JOC sink would need both. Nothing about AC-3 leaks upward except the block clock.

---

## 7. Validation strategy

A layered pyramid, fastest tests at the bottom:

**L0 — Golden DSP vectors (every commit, ms).** The spec itself is your golden data: Table 7.33's 256 window values (typed in, asserted against your KBD generator); MDCT vectors from a brute-force numpy implementation of the §8.2.3.2 equation (impulse/DC/sine/random, checked in with generator scripts); **bit-exact** bap arrays from a Python port of the §7.2.2 integer pseudocode (no tolerance — it's integer math); the CRC property asserted directly (shifting the first 5/8 of any frame, minus syncword, through the x¹⁶+x¹⁵+x²+1 register yields zero). Catch2 v3 + RapidCheck via vcpkg.

**L1 — Bitstream round-trips.** Property tests: random valid field vectors survive write→parse; exponent coding round-trips; mantissa grouping round-trips; every packet size matches the frame-size table.

**L2 — In-repo minimal decoder (recommended: yes, build it).** The decode process is completely normative, it shares tables/bit-allocation/window with the encoder, and it costs ~2–4 weeks. It is your strongest correctness anchor: in-process encode→decode round-trips, cross-checks against FFmpeg's FATE sample streams, and a fuzz target (MSVC `/fsanitize=fuzzer`). You'll want it anyway for E-AC-3 debugging.

**L3 — FFmpeg oracle (every commit, seconds).** Critical verified finding: **FFmpeg does not check AC-3 CRCs by default** (`err_detect` defaults to 0). The meaningful oracle command is:

```
ffmpeg -v error -err_detect crccheck+bitstream+buffer+explode -i out.ac3 -f wav out.wav
```

Pass = exit 0 and empty stderr, across a matrix of sample rates × bitrates × channel modes. Add `ffprobe -of json -show_streams -show_packets -show_frames` assertions (codec, layout, 1536 samples/frame, packet sizes), a second independent decoder (a52dec via MSYS2) with PCM cross-diff, and MediaInfo for header fields. Pin `dialnorm=−31`, omit DRC gain words, and decode with `-drc_scale 0` so metadata never pollutes PCM comparisons. Keep AC3ESBrowser handy for field-level debugging.

**L4 — Perceptual metrics (nightly).** Delay-compensated stddev/PSNR of decoded-vs-input with per-bitrate regression thresholds — exactly how FFmpeg validates its own encoder in FATE (`CMP = stddev`, not checksums). Race against `ffmpeg -c:a ac3` at matched bitrates; track GstPEAQ ODG or ViSQOL MOS as CI trend lines, not absolutes; occasional human ABX in foobar2000.

**L5 — Real AVR (manual, per release).** Wrap frames in IEC 61937 (`ffmpeg -c:a copy -f spdif`), play bit-exactly via WASAPI exclusive over TOSLINK/HDMI. Hardware decoders are the strictest you'll meet: no lock = framing bug; intermittent mutes = CRC/size bugs; wrong speaker lights = BSI bugs. E-AC-3 needs the HDMI path.

CI: GitHub Actions `windows-latest` ships VS 2026 (GA May 7, 2026 — note: not May 4 as sometimes reported), matching a local MSVC 2026 toolchain; pin the FFmpeg CLI version; cache a small FATE AC-3 subset.

---

## 8. Library architecture and module layout

Mirror the spec's own normative/informative split:

```
/core        — bit-exact normative code, shared by encoder and decoder:
               tables.hpp (constexpr std::array + static_asserts, A/52 section cited per table)
               bitwriter/bitreader (MSB-first, 64-bit accumulator), crc16
               mdct (direct + FFT paths), window (consteval KBD generator)
               bitalloc (the §7.2 integer engine — ONE implementation, both sides)
               exponents, mantissas (quantizers + the three group state machines)
/encoder     — strategy layer (the clean-room value-add, swappable policies):
               exponent_strategy, snr_search, rematrix, framer, (later: transient, coupling)
/decoder     — minimal in-repo decoder for validation
/spatial     — scene, vbap renderer, bed mixer, distance policies, SPSC queue
/sinks       — ac3_sink, wav_sink; later eac3_sink, iamf_sink
/tools, /tests
```

C++23 specifics (all verified against 2026 toolchain reality): `std::expected` for control-path errors (render/encode hot paths are noexcept); `std::span`/`std::mdspan` over planar float buffers; `std::countl_zero` for exponent extraction; consteval KBD generation needs ~40 lines of custom constexpr math (Bessel-I0 series, Newton sqrt) because constexpr `sqrt/cos` land only in C++26 and MSVC hasn't shipped them. **No `std::simd`** — it's C++26; MSVC hasn't started, and libc++ hasn't implemented it either — so go scalar-first with SoA layout, `alignas(64)`, fixed 256-length branch-free loops, and an FFmpeg-style dsp-struct seam for later intrinsics. Ship conventional headers, not C++ modules: CMake's `import std` remains experimental and Ninja-only, and VS 2026 is still fixing module ICEs. Design E-AC-3 readiness in now: blocks-per-frame and frame size as parameters, a {program → substreams → channels} model, opaque skip-field payload injection in the frame writer, swappable transform stage.

---

## 9. Roadmap

| # | Milestone | Exit criterion | Effort |
|---|---|---|---|
| 1 | Bitwriter + CRC16 + frame skeleton | Hand-packed sync frame passes CRC property tests | ~3 days |
| 2 | **Valid silent 2.0 frame** (headers + all-zero baps) | `ffprobe` reports ac3, correct rate/layout/frame count | ~1 week cumulative |
| 3 | MDCT + KBD window | Matches numpy goldens ≤1e-10; TDAC round-trip reconstructs input | +1 week |
| 4 | Fixed exponent strategy (D15 block 0, reuse after) + decoder-mirror reconstruction | Property tests green | +1 week |
| 5 | Bit-allocation engine + SNR search + mantissas + packer | **A 2.0 sine decodes cleanly under strict `-err_detect`, at the right amplitude and frequency** | +2–3 weeks (the hard middle) |
| 6 | 5.1 + LFE, all sample rates/bitrates, in-repo decoder | Full L3 matrix green; a52dec cross-decode agrees | +2 weeks |
| 7 | Quality layer: exponent-strategy search, rematrixing, bandwidth tuning | PSNR/PEAQ trend approaches FFmpeg at 256–448 kbps | +2–4 weeks |
| 8 | Spatial layer: scene + VBAP + bed mixer → WAV sink, then AC-3 sink | Moving source renders smoothly; gains match libear | +1–2 weeks (parallelizable from week 1) |
| 9 | Coupling (optional) | Quality parity at ≤192 kbps 5.1 | +2–4 weeks |
| 10 | Minimal E-AC-3 (bsid-16, arbitrary bitrates, ≤5.1) | Strict FFmpeg decode; real-device HDMI test | +2–4 weeks |
| 11 | E-AC-3 7.1 / SPX / AHT; IAMF sink prototype | — | months, as appetite allows |

First clean FFmpeg decode of real audio typically lands week 3–5. Budget the most debugging time for milestone 5.

---

## 10. Open questions for you

1. **License and distribution:** open-source (which license?) or proprietary? This determines how formal the clean-room documentation and the E-AC-3 legal review need to be.
2. **Commercial intent for E-AC-3?** If yes, budget for a freedom-to-operate opinion — the "last patent expired" claim is community analysis only.
3. **Real-time encoding requirement?** File-to-file only would relax the spatial layer's lock-free constraints (though the discipline is cheap if designed in early).
4. **Is IAMF/Eclipsa acceptable as the object-audio deliverable**, with DD+ JOC strictly a private research branch? (Recommended.)
5. **Do you own an AVR/soundbar for L5 testing**, and does it have eARC (for the E-AC-3 phase)?
6. **CI budget:** are nightly perceptual-metric runs and a cached FATE subset acceptable in your GitHub Actions plan?

---

## Glossary

- **AC-3 / E-AC-3** — the codecs behind "Dolby Digital" / "Dolby Digital Plus" (trademarked names; use the technical names).
- **MDCT** — Modified Discrete Cosine Transform; overlapped time→frequency transform whose aliasing cancels on reconstruction.
- **KBD window** — Kaiser-Bessel-Derived window; the tapering function applied before the MDCT.
- **Exponent / mantissa** — AC-3's per-coefficient scale (power of 2) and value; exponents double as the spectral envelope.
- **Bit allocation / bap** — the integer model deciding how many bits each mantissa gets; "bap" is the bit-allocation pointer selecting a quantizer.
- **PSD** — power spectral density, the integer loudness measure derived from exponents.
- **SNR offset** — the tunable parameter shifting the masking curve to spend more or fewer bits; the CBR rate-control knob.
- **Coupling / rematrixing / SPX / AHT** — optional tools: cross-channel high-frequency sharing; stereo sum/difference coding; spectral extension; adaptive hybrid transform.
- **BSI / acmod / dialnorm / DRC** — bit stream information header; audio coding mode (channel layout); dialogue normalization level; dynamic range compression.
- **Syncframe** — one AC-3 frame: 6 blocks × 256 samples = 1536 samples per channel.
- **Bed / object** — fixed channel-based stem vs. mono source with time-varying 3D position metadata.
- **VBAP / MDAP** — vector base amplitude panning (triplet gains for a 3D direction); its multi-direction variant for source spread.
- **ADM** — Audio Definition Model (ITU-R BS.2076), the vendor-neutral object/bed metadata model.
- **JOC / OAMD / EMDF** — joint object coding (Atmos-in-DD+); object audio metadata; the extensible metadata container carrying them.
- **IAMF** — Immersive Audio Model and Formats; AOMedia's royalty-free immersive container ("Eclipsa Audio").
- **IEC 61937 / S/PDIF** — wrapping compressed frames for transport to AV receivers.
- **LFE** — low-frequency effects channel (the ".1").

---

## Sources

- [ATSC A/52:2018 — Digital Audio Compression (AC-3, E-AC-3)](https://www.atsc.org/wp-content/uploads/2021/04/A52-2018.pdf)
- [ETSI TS 102 366 V1.4.1 — AC-3/Enhanced AC-3 Standard](https://www.etsi.org/deliver/etsi_ts/102300_102399/102366/01.04.01_60/ts_102366v010401p.pdf)
- [ETSI TS 103 420 V1.2.1 — Object audio carriage using Enhanced AC-3 (JOC)](https://www.etsi.org/deliver/etsi_ts/103400_103499/103420/01.02.01_60/ts_103420v010201p.pdf)
- [FFmpeg libavcodec/ac3enc.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/ac3enc.c) · [eac3enc.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/eac3enc.c) · [ac3.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/ac3.c) · [ac3dec.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavcodec/ac3dec.c) · [spdifenc.c](https://github.com/FFmpeg/FFmpeg/blob/master/libavformat/spdifenc.c)
- [FFmpeg codecs documentation](https://ffmpeg.org/ffmpeg-codecs.html) · [FATE testing docs](https://ffmpeg.org/fate.html) · [FATE AC-3 samples](http://fate-suite.ffmpeg.org/ac3/) · [E-AC-3 encoder commit (2011)](https://ffmpeg.org/pipermail/ffmpeg-cvslog/2011-May/037773.html)
- [Aften A/52 encoder](https://aften.sourceforge.net/) · [GitHub mirror](https://github.com/justinruggles/aften) · [liba52/a52dec](http://liba52.sourceforge.net/) · [MSYS2 a52dec package](https://packages.msys2.org/base/mingw-w64-a52dec)
- [US7516064 (E-AC-3 AHT, expired 2026-01-30)](https://patents.google.com/patent/US7516064B2/en) · [US6449368](https://patents.google.com/patent/US6449368B1/en) · [US5890106](https://patents.google.com/patent/US5890106A/en) · [US10276172 (Atmos, active)](https://patents.google.com/patent/US10276172B2/en) · [US12183354 (DRC metadata, active)](https://patents.google.com/patent/US12183354B2/en)
- [Phoronix — DD+ patents might now be expired (2026)](https://www.phoronix.com/news/Dolby-Digital-Plus-E-AC3-2026) · [Fedora legal discussion](https://discussion.fedoraproject.org/t/dolby-digital-plus-e-ac3-patents-have-now-expired-time-to-add-it-to-fedora/180329) · [Dolby virtual patent marking](https://professional.dolby.com/about/virtual-patent-marking/) · [Dolby patent licensing](https://www.dolby.com/about/patent-licensing/)
- [Clean-room design (Wikipedia)](https://en.wikipedia.org/wiki/Clean-room_design) · [Harvard JOLT on NEC v. Intel](https://jolt.law.harvard.edu/articles/pdf/v03/03HarvJLTech209.pdf)
- [raress96/dolby-atmos-encoder (JOC PoC)](https://github.com/raress96/dolby-atmos-encoder) · [truehdd](https://github.com/truehdd/truehdd)
- [IAMF spec (AOMedia)](https://aomediacodec.github.io/iamf/) · [AOMedia IAMF announcement](https://aomedia.org/press%20releases/AOMedia-Advances-the-Audio-Innovation-Era/) · [Eclipsa Audio (Google Open Source Blog)](https://opensource.googleblog.com/2025/01/introducing-eclipsa-audio-immersive-audio-for-everyone.html)
- [ITU-R BS.2076-3 (ADM)](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.2076-3-202502-I!!PDF-E.pdf) · [ITU-R BS.775 (5.1 layout)](https://www.itu.int/dms_pubrec/itu-r/rec/bs/R-REC-BS.775-1-199407-S!!PDF-E.pdf) · [ITU-R BS.2127 (ADM renderer)](https://www.itu.int/rec/R-REC-BS.2127/en) · [ebu/libear](https://github.com/ebu/libear) · [ebu_adm_renderer](https://github.com/ebu/ebu_adm_renderer)
- [Pulkki VBAP (Aalto)](https://research.aalto.fi/en/publications/virtual-sound-source-positioning-using-vector-base-amplitude-pann/) · [polarch/Vector-Base-Amplitude-Panning](https://github.com/polarch/Vector-Base-Amplitude-Panning)
- [Ross Bencina — Real-time audio programming 101](http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing) · [cameron314/readerwriterqueue](https://github.com/cameron314/readerwriterqueue)
- [AC3ESBrowser](https://github.com/virinext/ac3esbrowser) · [MediaInfoLib File_Ac3.cpp](https://github.com/MediaArea/MediaInfoLib/blob/master/Source/MediaInfo/Audio/File_Ac3.cpp) · [GstPEAQ](https://github.com/HSU-ANT/gstpeaq) · [Google ViSQOL](https://github.com/google/visqol)
- [IEC 61937-3](https://standards.iteh.ai/catalog/standards/iec/6317be3f-4217-4744-844c-690532179bd8/iec-61937-3-2017)
- [MSVC C++23 support (Build Tools 14.51)](https://devblogs.microsoft.com/cppblog/c23-support-in-msvc-build-tools-14-51/) · [Microsoft STL changelog](https://github.com/microsoft/STL/wiki/Changelog) · [P1383R2 constexpr math](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p1383r2.pdf) · [libc++ C++26 status](https://libcxx.llvm.org/Status/Cxx26.html) · [import std in CMake 3.30](https://www.kitware.com/import-std-in-cmake-3-30/)
- [GitHub Actions VS 2026 image](https://github.com/actions/runner-images/issues/14016) · [windows-latest migration](https://github.com/actions/runner-images/issues/14017)
- [scipy kaiser_bessel_derived](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.windows.kaiser_bessel_derived.html) · [Davidson — Digital Audio Coding: Dolby AC-3](https://dsp-book.narod.ru/DSPMW/41.PDF)
- [Sonos — Atmos over TV/ARC support notes](https://support.sonos.com/en-us/article/listen-to-dolby-atmos-audio-from-your-tv-on-sonos) · [Doom9 encoder quality comparison](https://forum.doom9.org/archive/index.php/t-172261.html)
