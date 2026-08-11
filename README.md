# ac3forge

<!-- Build & code health -->
[![CI](https://github.com/iainchesworth/ac3forge/actions/workflows/ci.yml/badge.svg)](https://github.com/iainchesworth/ac3forge/actions/workflows/ci.yml)
[![CodeQL](https://github.com/iainchesworth/ac3forge/actions/workflows/codeql.yml/badge.svg)](https://github.com/iainchesworth/ac3forge/actions/workflows/codeql.yml)
[![MSVC Code Analysis](https://github.com/iainchesworth/ac3forge/actions/workflows/msvc-analysis.yml/badge.svg)](https://github.com/iainchesworth/ac3forge/actions/workflows/msvc-analysis.yml)
[![OSV-Scanner](https://github.com/iainchesworth/ac3forge/actions/workflows/osv-scanner.yml/badge.svg)](https://github.com/iainchesworth/ac3forge/actions/workflows/osv-scanner.yml)
[![Zizmor](https://github.com/iainchesworth/ac3forge/actions/workflows/zizmor.yml/badge.svg)](https://github.com/iainchesworth/ac3forge/actions/workflows/zizmor.yml)
<!-- Supply chain & project meta -->
[![OpenSSF Scorecard](https://api.scorecard.dev/projects/github.com/iainchesworth/ac3forge/badge)](https://scorecard.dev/viewer/?uri=github.com/iainchesworth/ac3forge)
[![Latest release](https://img.shields.io/github/v/release/iainchesworth/ac3forge?include_prereleases&sort=semver)](https://github.com/iainchesworth/ac3forge/releases/latest)
[![Docs](https://img.shields.io/badge/docs-published-2f7d7b)](https://iainchesworth.github.io/ac3forge/)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)](docs/building.md)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

A clean-room AC-3 and E-AC-3 encoder and decoder in C++23, implemented from the published
standards. It turns PCM — or mono sources placed and moved in 3D space — into AC-3, E-AC-3,
or E-AC-3 with Joint Object Coding elementary streams, and reads those streams back.

Nothing here links FFmpeg or any other codec library. The FFmpeg command-line tools are used
during development as an independent decoder to check output against; the build does not
depend on them.

**Documentation:** [iainchesworth.github.io/ac3forge](https://iainchesworth.github.io/ac3forge/)
— a beginner's guide to the formats, a developer quick start, the full library and CLI
reference, and a step-by-step GUI guide with screenshots. This file is a short pointer into it,
not a copy of it; where the two disagree, the docs are current and this file is what's stale.

**Standards and trademarks.** "Dolby", "Dolby Digital" and "Dolby Atmos" are trademarks of
Dolby Laboratories. This project implements the openly published standards — ATSC A/52:2018
(of which E-AC-3 is normative Annex E), ETSI TS 102 366 and ETSI TS 103 420 — and is not
affiliated with, endorsed by, or certified by Dolby Laboratories. Code and documentation use
the technical names AC-3 and E-AC-3. Whether the patents reading on these formats matter for
your use is your problem to assess, not something this project resolves.

**Status.** The API is not stable — no release has been tagged yet, so the Latest release badge
above reads empty; once one lands, that badge is the current version, not this paragraph. CI
requires Windows (MSVC, clang-cl), Linux (GCC, Clang) and macOS (Homebrew LLVM) — CLI and GUI
alike on the first four, CLI only on macOS — plus an ASan+UBSan leg, clang-tidy static analysis,
a coverage gate over the library, a per-platform gold-reference quality gate, a dedicated
Linux FFmpeg-validation leg, and a required Android build leg for the Shield TV demo app under
`platform/android/`. See [docs/building.md](docs/building.md#verified-configuration) for
exact toolchain versions and what each CI leg covers.

## What it does

Encodes AC-3 (bsid 8) across every coding mode the standard defines, and E-AC-3 (bsid 16) across
those plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams, spectral extension, the
adaptive hybrid transform, and Dolby Atmos objects via JOC. The in-repo decoder shares the
encoder's core and reads both formats back, including every Annex E coding tool at every layout.
Also included: a standalone MKV muxer, S/PDIF (IEC 61937) burst packing, WASAPI/ALSA live
capture and playback, peak/RMS/loudness metering, and **Shield Atmos Demo** — a small
Android TV app (`platform/android/`) that streams live, controller-driven Atmos object motion
out an NVIDIA Shield's HDMI passthrough to a real AV receiver, sideload-only. See
[docs/platforms/android.md](docs/platforms/android.md).

Full capability tables — coding modes, sample rates, bit rates, metadata fields, spec section
citations — are in [What it does](docs/index.md#what-it-does).

## What it does not do

Enhanced coupling (`cpl+ecpl`) and transient pre-noise processing (`tpn`) are both implemented,
each with a known MVP limitation worth knowing before enabling it: enhanced coupling's encoder
always sends angle/chaos as zero (an amplitude-only fit), which degrades if two channels' content
shares one narrow coupling band; transient pre-noise processing's decoder buffers one frame at a
time once a stream turns it on, so a caller must call `Eac3Decoder::flush()` at end-of-stream or
lose the last held-back frame. That buffering is per substream, not per stream — `decode_access_unit`
assembles a whole access unit correctly even when only some of its substreams turn the tool on,
queuing whichever ones release early rather than dropping or misaligning them, and `flush()` drains
both its own assembly cache and each substream's held-back frame. AC-3 has no VBR — its frame size indexes a fixed table rather than
stating a word count, so it stays CBR; E-AC-3 supports both. Object streams from here are
spec-correct but won't decode as *objects* in Dolby's own decoder: it gates that on an
authenticity tag this project doesn't produce, which is a licensing gate, not a conformance
failure.

What that means for object reconstruction, which streams FFmpeg can check independently versus
which only the in-repo decoder can, and what has and hasn't been confirmed against real audio
hardware, is in [Validation](docs/verification.md).

## Building

Requires CMake ≥ 3.28, Ninja, a [vcpkg](https://github.com/microsoft/vcpkg) checkout with
`VCPKG_ROOT` set (it supplies Catch2, and nothing else), and — for the GUI — a prebuilt Qt 6.5+
kit, never from vcpkg. Windows needs Visual Studio 2026 (MSVC) or clang-cl; Linux needs GCC ≥ 15
or Clang ≥ 21.

```bash
cmake --preset config-windows-msvc-debug
cmake --build --preset build-windows-msvc-debug
ctest --preset test-windows-msvc-debug
```

Linux follows the same shape (`config-linux-gcc-debug`, etc.), with the GUI opt-in via
`-DAC3FORGE_BUILD_GUI=ON` rather than on by default. See
[Quick start](docs/quickstart.md) for the shortest path and
[Building from source](docs/building.md) for the rest — the full preset list, building without
Qt, packaging, and per-platform notes. Cutting a release is covered in
[docs/releasing.md](docs/releasing.md).

## Using it

```bash
ac3cli encode in.wav out.ac3 448 couple
ac3cli decode out.ec3 out.wav
```

`ac3cli` has twenty-one commands; run it with no arguments for the full listing.
`ac3gui` is a Qt Quick front end over the same library: file and live-capture encoding, a plan
view for placing objects, and channel-level metering. For the C++ API — two headers and about a
dozen lines to encode a frame — see [Quick start](docs/quickstart.md) or
[Library conventions](docs/library/index.md).

## Validation

Quality is measured, not asserted. `tools/quality_race.py` synthesizes stereo programme
material, encodes it with both ac3forge and FFmpeg at matched bit rates, decodes both with
FFmpeg as a neutral referee, aligns by cross-correlation, and reports SNR against the original:

| Bit rate | ac3forge | FFmpeg | Difference |
|---|---|---|---|
| 192 kbps | 41.23 dB | 40.98 dB | +0.25 |
| 256 kbps | 44.00 dB | 42.85 dB | +1.15 |
| 320 kbps | 45.09 dB | 44.15 dB | +0.94 |
| 448 kbps | 51.05 dB | 47.60 dB | +3.46 |

Measured with FFmpeg 8.0.1 on 2026-08-09; reproduce with `python tools/quality_race.py ac3`.
SNR on synthetic material is a narrow metric — it says the waveform is closer, not that it
sounds better, and no listening test has been run.

That's one number from a larger picture — which streams FFmpeg can check independently and
which only the in-repo decoder can, what Dolby's own tooling did and didn't confirm, and the
full test-suite counts — all in [Validation](docs/verification.md).

## Repository layout

```
cmake/          toolchains, Qt/CPack/sanitizer/coverage modules, vcpkg triplet overlays
src/lib/        ac3::forge — the whole codec, GUI-free
src/matroska/   matroska::matroska — a standalone MKV muxer, no ac3::forge dependency
src/cli/        ac3cli — command-line front end
src/gui/        ac3gui — Qt Quick front end (QML module "Ac3Forge")
platform/android/  Shield Atmos Demo — Android TV app, live Atmos object motion over HDMI
tests/          Catch2 unit tests; golden/ vectors generated by tools/
examples/       the programs docs/library/ is written from
fuzz/           libFuzzer harnesses over untrusted-input entry points (Clang only, off by
                default) — see fuzz/README.md
tools/          Python: spec-table generators, independent reference
                implementations, the FFmpeg quality race
docs/           the site source — see Documentation below
```

The standards documents are not redistributed; `docs/spec/` is gitignored. See
[docs/building.md](docs/building.md#the-standards-documents) if you need to re-run the table
generators in `tools/`.

## Documentation

[iainchesworth.github.io/ac3forge](https://iainchesworth.github.io/ac3forge/) is built from
`docs/` with mkdocs. Locally, the same pages:

| Document | Contents |
|---|---|
| [docs/quickstart.md](docs/quickstart.md) | Developer quick start: clone to first encode |
| [docs/building.md](docs/building.md) | Building from a clean clone, including the failures you will hit |
| [docs/platforms/](docs/platforms/windows.md) | Windows / Linux / macOS specifics: toolchains, audio backends, packaging |
| [docs/platforms/android.md](docs/platforms/android.md) | Shield Atmos Demo: the Android TV app, HDMI passthrough, controller input, screenshots |
| [docs/concepts/](docs/concepts/index.md) | Beginner's guide to AC-3, E-AC-3, Atmos and JOC, with diagrams |
| [docs/library/](docs/library/index.md) | The public API, with compiled examples |
| [docs/cli/](docs/cli/index.md) | The `ac3cli` reference: all 21 commands, metadata options |
| [docs/gui/](docs/gui/index.md) | Step-by-step `ac3gui` guide, with screenshots |
| [docs/verification.md](docs/verification.md) | How output is checked, and where checking runs out |
| [docs/project/history.md](docs/project/history.md) | How the implementation was built, milestone by milestone |
| [docs/quality-trend.md](docs/quality-trend.md) | Gold-reference SNR history by commit, develop vs. main - the FFmpeg-oracle gate's numbers, trended over time |
| [docs/releasing.md](docs/releasing.md) | Cutting a release: versioning, the tag-triggered workflow, GPG signing |
| [fuzz/README.md](fuzz/README.md) | The libFuzzer harnesses: what they cover, how to run them locally |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Conventions, and the validation discipline |

## Licence

Copyright (C) 2026 Iain Chesworth.

ac3forge is free software: you can redistribute it and/or modify it under the terms of the GNU
General Public License version 3 as published by the Free Software Foundation. It is
distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the
implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the full text in
[LICENSE](LICENSE).

GPL-3.0 is copyleft: anything that links this library must be distributed under the GPL too.
That is a deliberate choice, not an oversight. The licence covers this source only — it grants
no rights in the standards it implements, and says nothing about any patents reading on AC-3
or E-AC-3.
