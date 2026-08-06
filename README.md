# ac3forge

A **clean-room AC-3 encoder** written from first principles in C++23, working directly from
the published standards — no FFmpeg, no codec libraries. The goal: take one or more channels
of PCM audio (eventually: sound objects positioned and moved in 3D space) and produce a
compliant AC-3 elementary stream that any decoder or AV receiver accepts.

> **Naming note:** "Dolby Digital" is a live trademark of Dolby Laboratories. This project
> implements the openly published **AC-3** standard (ATSC A/52 / ETSI TS 102 366) and is not
> affiliated with or endorsed by Dolby. `ac3forge` is a provisional working name.

## Status

**Milestone 0 — scaffold.** Core bit I/O (`BitWriter`), the AC-3 CRC-16, and base syntax
tables, all with tests. See [docs/RESEARCH.md](docs/RESEARCH.md) for the full research
summary, architecture, and roadmap (next up: frame skeleton → a valid silent stereo frame
that `ffprobe` recognizes).

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
