# AC-3 From First Principles — Research Summary & Project Direction

**Date:** 2026-08-06
**Method:** two multi-agent research waves (26 agents, ~500 web sources consulted). Every load-bearing claim was independently, adversarially fact-checked: **122 claims → 113 confirmed, 3 refuted (corrected here), 6 uncertain (flagged inline)**.
**Full detail:** [wave 2 — from-scratch implementation brief](https://github.com/iainchesworth/ac3forge/blob/main/docs/research/wave2-from-scratch-brief.md) (the spine of this project) and [wave 1 — domain brief](https://github.com/iainchesworth/ac3forge/blob/main/docs/research/wave1-domain-brief.md) (format, I/O, toolchain background; its "link FFmpeg" recommendation predates the project pivot and is superseded). Verification records: [wave 1](https://github.com/iainchesworth/ac3forge/blob/main/docs/research/wave1-verdicts.json) · [wave 2](https://github.com/iainchesworth/ac3forge/blob/main/docs/research/wave2-verdicts.json).

---

## 1. Mission and ground rules

Build, **from first principles in idiomatic C++23**, a library that encodes one or more channels of PCM audio into an AC-3 ("Dolby Digital") elementary stream — then grow upward: a spatial layer where applications place and move sound sources in 3D space and hear that in the encoded output; later E-AC-3; possibly object-audio formats.

Ground rules established for this project:

- **No codec libraries.** We do not link FFmpeg or any encoder. The installed FFmpeg 8.0.1 CLI is a *validation oracle only* — it decodes our streams to prove they are real AC-3.
- **Clean-room from the spec.** Implement strictly from the published standards. Cite the A/52 section number in a comment for every table and algorithm. Open-source encoders (FFmpeg, Aften) may be consulted for *architecture* decisions only — never transcribe code. Since the spec contains every table, we never need to copy anything.
- **Patent landscape is cleared context** for this project (user-confirmed) — noted where relevant, never a gate.
- **Naming:** "Dolby", "Dolby Digital", "Dolby Atmos" are live trademarks. Code, docs, and any released artifact use the technical names **AC-3 / E-AC-3** (working code name: `ac3forge`, provisional).

## 2. The one insight that shapes everything

**AC-3 is decoder-defined.** The standard (ATSC A/52 §8.1, verbatim): *"the encoder is not precisely specified. The only normative requirement on the encoder is that the output elementary bit stream follow AC-3 syntax."*

The decoder recomputes bit allocation itself, from a handful of transmitted parameters, using **exact integer arithmetic** specified in the standard. Consequences:

1. Our encoder must contain a **bit-exact copy of the decoder's integer bit-allocation model** (A/52 §7.2). This is non-negotiable and cannot be simplified — it is the heart of the project.
2. Everything *else* encoder-side is our free design space: which exponent strategy to send, when to reuse data, how to search SNR offsets, whether to couple channels. This is where a clean-room implementation can be genuinely original.
3. A "minimal valid encoder" is much smaller than the full standard: FFmpeg's production encoder has **never used short blocks in 25 years** (`blksw=0` always), shipped its first decade without channel coupling, and Aften never implemented coupling at all. **v1 scope: long blocks only, no coupling, rematrixing on, fixed metadata.**

## 3. Verified foundations

### The standards (all free PDFs — the complete implementation source)

| Document | Role |
|---|---|
| [ATSC A/52:2018](https://www.atsc.org/wp-content/uploads/2021/04/A52-2018.pdf) | The master standard, 271 pp. Bitstream syntax (§5), decoder bit allocation (§7.2), informative encoder guide (§8). E-AC-3 is normative Annex E. Current revision — nothing newer exists. |
| [ETSI TS 102 366 V1.4.1](https://www.etsi.org/deliver/etsi_ts/102300_102399/102366/01.04.01_60/ts_102366v010401p.pdf) | European twin. Uniquely carries the full EMDF metadata format (Annex H) needed for later object-audio work. (ETSI's server rejects non-browser user agents.) |
| [ETSI TS 103 420 V1.2.1](https://www.etsi.org/deliver/etsi_ts/103400_103499/103420/01.02.01_60/ts_103420v010201p.pdf) | Atmos-in-DD+ (Joint Object Coding) — decoder-only spec; "encoder" appears zero times in it. Research-branch material. |

### Format facts (verified against the spec)

- Stream = self-contained **syncframes** back-to-back: sync word `0x0B77`, 6 blocks × 256 samples = **1536 samples/channel/frame** (32 ms @ 48 kHz), two CRC-16s (poly x¹⁶+x¹⁵+x²+1), no global header.
- Sample rates: **48 / 44.1 / 32 kHz** only. Bitrates: **19 fixed CBR values, 32–640 kbit/s**; frame byte size fixed per rate (44.1 kHz alternates one padding word between adjacent frames — implement the alternation or average bitrate drifts).
- Channels: mono → **5.1** max. AC-3 internal channel order (L, C, R, SL, SR, LFE) differs from WAV/Windows order — our conditioner owns the remap.
- The 512-point window (Table 7.33) is **exactly the Kaiser-Bessel-Derived window, α=5** — verified numerically against `scipy.signal.windows.kaiser_bessel_derived(512, 5π)`. Generate from the formula; use the spec table as a unit-test oracle.

### Known traps (each independently verified — these are the bugs that cost weeks)

- **Decoded-exponent mirroring:** the encoder must run the decoder's exponent reconstruction and use *those* exponents for mantissa normalization and bit allocation — not its raw ones — or streams decode as noise.
- **`mask &= 0x1fe0` truncation order** in bit allocation must match the spec exactly (subtract snroffset, subtract floor, clamp, truncate, re-add floor).
- **Spec erratum:** `calc_lowcomp` pseudocode contains a stray semicolon; the universally implemented intent is `if (b0+256==b1) a=384; else if (b0>b1) a=max(0,a−64);`.
- **Spec self-contradiction:** rematrixing — §7.5.3 (minimum-power rule) vs informative §8.2.6 (maximum-power). Follow §7.5.3; both are valid (encoder discretion).
- **Shared bit-counting:** the bit counter inside the SNR-offset search must share the exact mantissa-grouping state machine with the packer, or frames over/under-fill.
- **dialnorm:** −31 means "no attenuation" and plays ~4 dB hotter than commercial content (typically −27). Treat loudness metadata as a first-class input; fixed −31 is fine for testing (and keeps PCM comparisons clean), −27 or measured (ITU-R BS.1770) for real content.

### Numeric strategy (the most important design decision)

Float front half, integer back half. Reference build: float64 window + MDCT → convert coefficients to **signed 25-bit fixed point** (`round(c × 2²⁴)`) → from there on, everything (exponent extraction via `std::countl_zero`, PSD, the §7.2 model, SNR search, quantization, packing) is pure int32 table-driven math. The entire back half is **bit-exact and platform-independent by construction**; only the MDCT can wobble by ULPs. Pin reference-build output bytes in CI; never let `/fp:fast` touch the reference path.

## 4. Scope, honestly

FFmpeg's entire AC-3/E-AC-3 encoder family is **4,643 lines of C** (verified count). Realistic clean C++23 equivalent: **4,000–6,000 library LOC + 2,000–3,000 LOC tests/tools ≈ 4–8 focused weeks to a first valid, FFmpeg-decodable stereo/5.1 encoder**, then weeks-to-months for quality tuning, coupling, E-AC-3. The difficulty is concentrated in bit allocation + bitstream packing being *exactly* right, not in exotic math. Performance is a non-problem: FFmpeg encodes a 32 ms frame in ~0.1 ms; even a 50× slower first implementation is comfortably real-time.

## 5. Feasibility ladder

| Rung | Target | Patent picture (2026, informational) | Verdict |
|---|---|---|---|
| 0–1 | AC-3 stereo/5.1 + quality layer | Expired March 2017 (high confidence) | **The project** |
| 2 | Channel coupling | Expired | Optional; matters below ~192 kbps 5.1 |
| 3 | Minimal E-AC-3 (bsid-16, arbitrary bitrates, ≤5.1) | Last known US patent expired 2026-01-30 (date verified; "it was the last" is community analysis) | Strong stretch — a ~300-line shim over the AC-3 core (proven by FFmpeg's own eac3 encoder) |
| 4 | E-AC-3 7.1 / SPX / AHT | As above | Months; original encoder-side DSP design |
| 5 | DD+ JOC (Atmos-in-DD+) | Densely patented through ≥2034; hardware Atmos light-up additionally gated by implementation-defined EMDF protection fields — one public PoC decodes as Atmos in software but not on hardware (cause not established) | Research branch |
| Alt | **IAMF / Eclipsa Audio** object output | AOMedia royalty-free, spec + reference tools open | **Recommended object-audio endgame** — same scene model and renderer, legally clean, shipping in real products (YouTube, Samsung TVs) |

## 6. Architecture

```
/core        bit-exact normative code, shared encoder+decoder:
             tables (constexpr, A/52 § cited per table) · bitwriter/bitreader ·
             crc16 · mdct (direct + FFT paths) · window (KBD generator) ·
             bitalloc (§7.2 integer engine — ONE implementation, both sides) ·
             exponents · mantissas (quantizers + grouping state machines)
/encoder     strategy layer (the clean-room value-add, swappable policies):
             exponent_strategy · snr_search · rematrix · framer
             (later: transient detection, coupling)
/decoder     minimal in-repo decoder — strongest correctness anchor (~2–4 wks,
             fully normative, shares /core; also the E-AC-3 debugging tool)
/spatial     scene (ADM semantics: objects + beds) · VBAP renderer ·
             bed mixer (per-block gain ramps) · distance policies · SPSC queue
/sinks       ac3_sink · wav_sink · iec61937_sink (S/PDIF bursts — we write our
             own packer; trivially simple framing) · later: eac3_sink, iamf_sink
/io          sources: tone generator · WAV reader · later miniaudio capture
             (vcpkg `miniaudio` is the one wrapper with WASAPI loopback),
             raw-WASAPI process-loopback and exclusive-mode passthrough modules
/apps        ac3cli
/tests /tools
```

Spatial-layer load-bearing decisions (from research): rendering is a *view* of the scene, not a consuming transform — the render stage outputs `{bed frames, per-object gain matrices, untouched object PCM + metadata}` so an AC-3 sink reads only the bed while a future IAMF sink consumes objects directly. VBAP panner (~solve `g = pᵀL⁻¹`, normalize Σg²=1), validated against EBU **libear** as a test-only oracle. Automation clocked at the 256-sample block with per-sample linear gain ramps (kills zipper noise). Objects never feed the LFE. Audio thread never allocates/locks; control via pre-allocated lock-free SPSC queue.

C++23 toolchain reality (verified): `std::expected`, `std::print`, `std::countl_zero`, `std::span`/`mdspan` are solid on MSVC 2026. `CMAKE_CXX_STANDARD 23` emits `/std:c++latest` (no `/std:c++23` switch exists yet; 14.52 previews announce one). **No `std::simd`** (C++26; unshipped) — scalar-first with SoA layout and a dsp-struct seam for later intrinsics. **No C++ modules** — `import std` still experimental/Ninja-only and VS 2026 still fixing module ICEs. constexpr KBD generation needs ~40 lines of custom Bessel-I₀/Newton-sqrt math (constexpr `sqrt`/`cos` land in C++26).

## 7. Validation pyramid

- **L0 — Golden DSP vectors** (every commit, ms): spec Table 7.33 asserted against our KBD generator; numpy MDCT goldens (impulse/DC/sine/random) with generator scripts checked in; **bit-exact** bap arrays from a Python port of §7.2.2 (integer math — zero tolerance); CRC property (first 5/8 of frame shifts to zero). Catch2 v3 (+ RapidCheck for property tests) via vcpkg.
- **L1 — Bitstream round-trips:** write→parse property tests; exponent and mantissa-grouping round-trips; packet size == frame-size table, always.
- **L2 — In-repo minimal decoder:** encode→decode in-process; cross-check against FFmpeg FATE AC-3 sample streams; fuzz target (`/fsanitize=fuzzer`).
- **L3 — FFmpeg oracle** (every commit, seconds). Critical verified finding: **FFmpeg does not check AC-3 CRCs by default.** The meaningful command is:
  `ffmpeg -v error -err_detect crccheck+bitstream+buffer+explode -i out.ac3 -f wav out.wav` (pass = exit 0, empty stderr), plus `ffprobe -of json -show_streams -show_frames` assertions, across a rate×bitrate×acmod matrix. Second opinion: a52dec (MSYS2). Pin dialnorm=−31 and decode with `-drc_scale 0` so metadata never pollutes PCM comparisons.
- **L4 — Perceptual** (nightly): delay-compensated stddev/PSNR vs input with per-bitrate regression thresholds (exactly how FFmpeg's own FATE validates its encoder); race `ffmpeg -c:a ac3` at matched bitrates; GstPEAQ/ViSQOL as trend lines.
- **L5 — Real hardware** (manual): IEC 61937-wrapped frames over WASAPI exclusive → TOSLINK/HDMI. The "spdif-WAV" trick validates the packer before any WASAPI code exists: wrap the burst stream in a WAV header, play it bit-exact, watch the receiver light up. Hardware decoders are the strictest test we will ever meet.

## 8. Roadmap

| # | Milestone | Exit criterion |
|---|---|---|
| 0 | Scaffold: repo, CMake+vcpkg, CI stub, bitwriter+CRC16+base tables with tests | Green build + tests on this machine |
| 1 | Frame skeleton | Hand-packed sync frame passes CRC property tests |
| 2 | **Valid silent 2.0 frame** | `ffprobe` reports ac3, correct rate/layout/frame count |
| 3 | MDCT + KBD window | numpy goldens ≤1e-10; TDAC round-trip reconstructs input |
| 4 | Exponent pipeline (D15 block 0 + reuse; decoder-mirror reconstruction) | Property tests green |
| 5 | Bit allocation + SNR search + mantissas + packer — *the hard middle* | 2.0 sine decodes cleanly under strict `-err_detect`, right amplitude & frequency |
| 6 | 5.1 + LFE, all rates/bitrates; in-repo decoder | Full L3 matrix green; a52dec agrees |
| 7 | Quality layer: exponent-strategy search, rematrixing, bandwidth tuning | PSNR/PEAQ trend approaches FFmpeg at 256–448 kbps |
| 8 | Spatial layer: scene + VBAP + bed mixer → WAV sink, then AC-3 sink (parallelizable from week 1) | Moving source renders smoothly; gains match libear |
| 9 | IEC 61937 packer + spdif-WAV receiver test | A real receiver lights "Dolby Digital" from a bit-exact WAV |
| 10 | Live: capture (WASAPI loopback) + WASAPI-exclusive passthrough sink | The software "DD-Live" moment; ~70–100 ms end-to-end budget — **done for AC-3 file capture and live encode; E-AC-3/Atmos passthrough, a shared-mode monitor path (`MonitorSink`), and `ac3cli live` (capture→encode→live monitor and/or passthrough, continuously) landed later — see [history.md](history.md#live-monitor-e-ac-3atmos-passthrough-and-the-live-pipeline). Still unconfirmed against real bitstreaming hardware; see the [README](https://github.com/iainchesworth/ac3forge/blob/main/README.md#verification-gaps).** |
| 11 | Coupling (optional quality work at low bitrates) | Parity at ≤192 kbps 5.1 |
| 12 | Minimal E-AC-3 (bsid-16) | Strict FFmpeg decode; real-device HDMI/eARC test |
| 13 | E-AC-3 7.1/SPX/AHT · IAMF sink prototype | As appetite allows |

First clean FFmpeg decode of real audio typically lands week 3–5. Budget the most debugging time for milestone 5.

## 9. Open questions

1. **Name & license.** `ac3forge` is a provisional code name; is it a keeper? Open-source (which license?) or private? (Affects how formally we document the clean-room process.)
2. **Real-time from day one?** File-to-file first is assumed; the lock-free spatial discipline is designed in regardless (it's cheap early, expensive late).
3. **Hardware for L5/L9:** do you have an AVR or soundbar with TOSLINK or HDMI (ideally eARC, for the E-AC-3 phase)?
4. **Is IAMF/Eclipsa acceptable as the object-audio endgame**, with DD+ JOC strictly a private research branch? (Recommended.)
5. **CI appetite:** GitHub Actions with nightly perceptual runs, or local-only for now?

## Glossary

See the [wave 2 brief's glossary](https://github.com/iainchesworth/ac3forge/blob/main/docs/research/wave2-from-scratch-brief.md#glossary) — it covers every term used here (MDCT, KBD, exponent/mantissa, bap, PSD, SNR offset, acmod, dialnorm, DRC, VBAP, ADM, JOC, EMDF, IAMF, IEC 61937, LFE, bed/object).
