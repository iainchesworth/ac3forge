# ac3forge

A **clean-room AC-3 encoder** written from first principles in C++23, working directly from
the published standards — no FFmpeg, no codec libraries. The goal: take one or more channels
of PCM audio (eventually: sound objects positioned and moved in 3D space) and produce a
compliant AC-3 elementary stream that any decoder or AV receiver accepts.

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

See [docs/RESEARCH.md](docs/RESEARCH.md) for the research summary, architecture, and
roadmap (next: 5.1 + LFE and the in-repo decoder, then the quality layer and the spatial
scene → VBAP → bed renderer).

## Ground rules

- **Clean-room:** every table and algorithm is transcribed from ATSC A/52:2018 with its
  section number cited in a comment. Open-source encoders are consulted for architecture
  lessons only; no code is ever copied.
- **FFmpeg is an oracle, not a dependency:** the installed `ffmpeg`/`ffprobe` CLI validates
  our output (`ffmpeg -v error -err_detect crccheck+bitstream+buffer+explode -i out.ac3 …`);
  nothing links against it.
- **vcpkg supplies test/tooling packages only** (currently Catch2).

## Building

Requirements: Visual Studio 2026 (MSVC), CMake ≥ 3.28, Ninja, and a
[vcpkg](https://github.com/microsoft/vcpkg) checkout with `VCPKG_ROOT` set (or use a
`CMakeUserPresets.json` that sets it — see `CMakePresets.json` for the inherited presets).

From a *Developer PowerShell for VS 2026*:

```powershell
cmake --preset debug        # or your user preset inheriting it
cmake --build --preset debug
ctest --preset debug
```

## Layout

```
src/ac3/core/   bit-exact normative code (bitwriter, crc16, tables; soon: mdct,
                window, bitalloc, exponents, mantissas — shared by encoder & decoder)
apps/ac3cli/    command-line tool
tests/          Catch2 unit tests (golden vectors from the spec itself)
docs/           RESEARCH.md — research summary, architecture, roadmap
docs/research/  full research briefs + adversarial verification records
```
