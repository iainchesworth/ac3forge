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
The spatial layer (`src/spatial/`) places mono objects on the ITU 5.1 ring via
energy-normalized 2D VBAP with per-block gain ramps and explicit LFE sends; `ac3cli orbit`
renders a tone circling the listener straight into 5.1 AC-3 (an end-to-end test parks the
object at each speaker and proves the decoded energy follows it: C → L → SL → SR → R).
The IEC 61937 packer (`src/sinks/`) wraps frames into S/PDIF bursts **byte-exact against
FFmpeg's spdif muxer**, and `ac3cli spdif` emits them as a PCM16 WAV — played bit-exactly
through a passthrough output, a receiver locks on and lights its Dolby Digital indicator.

See [docs/RESEARCH.md](docs/RESEARCH.md) for the research summary, architecture, and
roadmap (remaining rungs: channel coupling, live WASAPI capture/passthrough, E-AC-3).

## Ground rules

- **Clean-room:** every table and algorithm is transcribed from ATSC A/52:2018 with its
  section number cited in a comment. Open-source encoders are consulted for architecture
  lessons only; no code is ever copied.
- **FFmpeg is an oracle, not a dependency:** the installed `ffmpeg`/`ffprobe` CLI validates
  our output (`ffmpeg -v error -err_detect crccheck+bitstream+buffer+explode -i out.ac3 …`);
  nothing links against it.
- **vcpkg supplies test/tooling packages only** (currently Catch2). **Qt is a prebuilt
  dependency**, never a vcpkg port: `cmake/FindQt6.cmake` widens `CMAKE_PREFIX_PATH` to the
  standard install roots (`C:/Qt/6.x/msvc2022_64`, `~/Qt`, `/opt/Qt`, …) and then defers to
  Qt's own config package. `-DCMAKE_PREFIX_PATH=…` or `-DQt6_DIR=…` always wins.
- **Warnings are errors** (`ac3::warnings`, linked privately into every first-party target).

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
  include/ac3/  public headers: core/ encoder/ decoder/ spatial/ sinks/ io/
  src/          implementation
src/cli/        ac3cli — command-line front end
src/gui/        ac3gui — Qt6 Quick front end (QML module "Ac3Forge")
tests/          Catch2 unit tests; golden/ vectors generated by tools/
tools/          Python generators (spec-table extraction, golden vectors,
                the FFmpeg quality race) and the sine analysis harness
docs/           RESEARCH.md plus the full research briefs and verification records
```
