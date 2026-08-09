# ac3forge

A clean-room AC-3 and E-AC-3 encoder and decoder in C++23, implemented from the published
standards. It turns PCM — or mono sources placed and moved in 3D space — into AC-3, E-AC-3,
or E-AC-3 with Joint Object Coding elementary streams, and reads those streams back.

Nothing here links FFmpeg or any other codec library. The FFmpeg command-line tools are used
during development as an independent decoder to check output against; the build does not
depend on them.

**Standards and trademarks.** "Dolby", "Dolby Digital" and "Dolby Atmos" are trademarks of
Dolby Laboratories. This project implements the openly published standards — ATSC A/52:2018
(of which E-AC-3 is normative Annex E), ETSI TS 102 366 and ETSI TS 103 420 — and is not
affiliated with, endorsed by, or certified by Dolby Laboratories. Code and documentation use
the technical names AC-3 and E-AC-3. Whether the patents reading on these formats matter for
your use is your problem to assess, not something this project resolves.

**Status.** Version 0.2.0. The API is not stable. Green and required in CI on Windows (MSVC,
clang-cl), Linux (GCC 15, Clang 21, and an ASan+UBSan leg) and static analysis (clang-tidy);
macOS is the one experimental leg, never run anywhere. See [Portability](#portability).

## Contents

- [What it does](#what-it-does)
- [What it does not do](#what-it-does-not-do)
- [Building](#building)
- [Using the library](#using-the-library)
- [Using the CLI](#using-the-cli)
- [How it is validated](#how-it-is-validated)
- [Repository layout](#repository-layout)
- [Documentation](#documentation)
- [Licence](#licence)

## What it does

### Encoding

| | AC-3 (bsid 8) | E-AC-3 (bsid 16) |
|---|---|---|
| Coding modes | 1/0, 2/0, 3/0, 2/1, 3/1, 2/2, 3/2, each with or without LFE | the same, plus 7.1, 5.1.2, 5.1.4 and 7.1.4 through dependent substreams |
| Sample rates | 48, 44.1, 32 kHz | 48, 44.1, 32 kHz |
| Bit rates | the 19 nominal rates of Table 5.18, 32–640 kbps | the same 19, per substream |
| Transform | long blocks only (512-point MDCT, KBD window) | long blocks only |
| Exponents | D15 / D25 / D45, strategy chosen per block from the reuse span (§8.2.8) | frame-level, Table E2.10 code 0: D15 in block 0, reused for the other five |
| Coupling | yes (§7.4), begin and end frequencies auto or pinned | yes (§E3.3) |
| Rematrixing | yes, 2/0 (§7.5.3 minimum-power rule) | no — the syntax is written, the flags are always zero |
| Annex E tools | — | spectral extension (§E3.6), adaptive hybrid transform with GAQ (§E3.4) |
| Objects | panned to a 5.1 bed (no metadata survives) | OAMD + JOC in an EMDF container (TS 103 420) |

At 44.1 kHz, CBR needs non-integral frame sizes; the AC-3 encoder alternates between the two
Table 5.18 lengths on a Bresenham accumulator so the long-run rate is exact. E-AC-3 signals
`frmsiz` directly and needs no such alternation.

### Metadata

| Field | Section | What it does here |
|---|---|---|
| `dynrng` | §7.7.1 | Per-block dynamic range control from an RMS-detected compressor on a piecewise-linear curve. Five profiles: `film-standard`, `film-light`, `music-standard`, `music-light`, `speech`. A/52 fixes the wire format and the intent but not the curve, so the profiles are this project's, not the standard's. |
| `compr` | §7.7.2 | Heavy compression as a limiter guaranteeing a peak ceiling in the §7.8 mono downmix. Rounds down, because nearest-code rounding can overshoot a ceiling by half a step. Its peak detector includes the previous frame's MDCT overlap. |
| `dialnorm` | §5.4.2.8 | Measured with ITU-R BS.1770-4 gated loudness and negated, or set directly. A/52 predates BS.1770 and leaves the measurement open. |
| Downmix levels | Tables 5.9/5.10, E1.2 | `cmixlev`/`surmixlev` in AC-3; the whole `mixmdate` group in E-AC-3. |

### Decoding

The in-repo decoder shares its tables, bit-allocation engine, exponent decoding and IMDCT
with the encoder. It reads AC-3 (bsid ≤ 8) and E-AC-3 (bsid 11–16), including dependent
substreams, `chanmap`, and the §E3.8.2 render that lays a dependent's channels over the bed.

### Other

| Component | What it is |
|---|---|
| `ac3::io::scan` | Finds access-unit boundaries in a raw elementary stream and reports what it renders, without being told. |
| `matroska::matroska` | A standalone MKV muxer. Links nothing from `ac3::forge` and knows nothing about AC-3. |
| `ac3::sinks::iec61937` | S/PDIF burst packing: AC-3 byte-exact against FFmpeg's `spdif` muxer; E-AC-3 (`Eac3BurstPacker`) verified against FFmpeg's `spdif_header_eac3` and Microsoft's own IEC 61937 documentation (both independently fetched, not recalled — see the limitations below). |
| `ac3::capture` | Live input/loopback capture — WASAPI on Windows, ALSA on Linux — through a lock-free SPSC ring. |
| `ac3::sinks::PassthroughSink` | Exclusive-mode/direct bitstream output, AC-3 or E-AC-3 — WASAPI on Windows, ALSA on Linux. See the limitations below (Windows hardware-confirmed; the ALSA backend is not). |
| `ac3::sinks::MonitorSink` | Shared-mode PCM playback — WASAPI or ALSA: a non-bitstreamed preview/monitor path that decodes what is being encoded and plays it back on an ordinary output. Confirmed against real Windows hardware — see below. |
| `ac3::analysis` | Peak/RMS metering with console ballistics, and the Gerzon energy vector over the BS.775 ring. |

## What it does not do

### Not implemented

| Missing | Where it matters |
|---|---|
| Block switching (short blocks) | Transients smear. FFmpeg's AC-3 encoder has never used short blocks either, so this is conventional rather than unusual, but it is still a gap. |
| Dual mono (1+1, acmod 0) | Refused by the encoder and the decoder. It is two programmes sharing a syncframe, with a second copy of every metadata item, and it has no channel layout to render. |
| Delta bit allocation | Encoder never emits it; decoder refuses a stream carrying it. |
| E-AC-3 half sample rates (`fscod2`: 24, 22.05, 16 kHz) | Refused. Every table the core indexes is three columns wide. |
| Enhanced coupling, transient pre-noise processing | Recognised by the decoder and refused, rather than mis-decoded. |
| Variable bit rate | CBR only. |

### Verification gaps

**7.1.4 has no external oracle.** It needs two dependent substreams, and FFmpeg refuses any
frame with `substreamid != 0` in `ff_ac3_parse_header` — in every container tried
(hand-rolled MKV, FFmpeg Matroska, MPEG-TS, MP4). Only the in-repo decoder reads it, so for
that layout the encoder and decoder are checked against each other and nothing else:

```
$ ac3cli eac3-sine out.ec3 1 384 1000 50 714
$ ffmpeg -v error -i out.ec3 -f null -
[dec:eac3] Error submitting packet to decoder: Error number -84085770 occurred
$ ac3cli decode out.ec3 out.wav
decoded 32 E-AC-3 access units (3 substreams each) -> out.wav
  12 channels, 48000 Hz: L R C LFE Lrs Rrs Ls Rs Vhl Vhr Lts Rts
```

Fourteen channels are coded and twelve are rendered: per §E3.8.2 the dependent's Ls and Rs
replace the bed's rather than adding to them.

**The oracles are complementary, and neither covers everything.** The in-repo decoder refuses
Annex E coupling, spectral extension and AHT; FFmpeg reads all three, but refuses a *second*
dependent substream. A single dependent numbers from 0 in its own space, which is why FFmpeg
reads 7.1, 5.1.2 and 5.1.4 without complaint and only 7.1.4 defeats it. So a stream using an
Annex E tool *and* two dependents has no decoder available here at all, and that combination
is unverified.

| Stream | FFmpeg | In-repo decoder |
|---|---|---|
| AC-3, any supported mode | yes | yes |
| E-AC-3 up to 5.1.4 (one dependent), no Annex E tools | yes | yes |
| E-AC-3 7.1.4 (two dependents) | no | yes |
| E-AC-3 with cpl / spx / aht | yes | no |
| E-AC-3 7.1.4 with Annex E tools | no | no |

**Exclusive-mode passthrough — AC-3 and E-AC-3 alike — has never been confirmed against
bitstreaming hardware.** The development machine has no S/PDIF or HDMI endpoint behind a real
AV receiver; enumeration during this work also briefly surfaced a monitor's own HDMI audio
endpoint, which is not a Dolby-capable receiver either and was not used to test bitstreaming.
`IsFormatSupported` correctly answers no everywhere it has been tried, for both
`KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL` and `..._DOLBY_DIGITAL_PLUS`, and neither
descriptor has been accepted by a real device. What *is* verified: the exclusive-mode path
itself works (the Realtek endpoint accepts an exclusive PCM format), the AC-3 bursts are
byte-exact against FFmpeg's `spdif` muxer, and the E-AC-3 burst framing (data type 0x15, the
24576-byte/4x-carrier-rate burst, multi-syncframe accumulation, `Pd` in bytes not bits) is
independently verified against both FFmpeg's `spdif_header_eac3` and Microsoft's own
"Representing Formats for IEC 61937 Transmissions" documentation — fetched live and
cross-checked against each other, not recalled — plus round-trip and real-audio unit tests
(`tests/test_iec61937.cpp`). A receiver locking onto AC-3 has been confirmed only by playing
those bursts as a PCM16 WAV through a passthrough output, a different code path from
`PassthroughSink`; the same trick now exists for E-AC-3 (`ac3cli spdif`/`monitor`/`live`
branch on bsid) but has not itself been tried against a receiver either.

**The shared-mode monitor path (`MonitorSink`) *is* confirmed against real hardware.** Unlike
passthrough it needs no bitstream-capable endpoint — any output works — so
`ac3cli monitor`/`ac3cli live --monitor` have actually played decoded AC-3 and E-AC-3 (including
an Atmos stream's 5.1 bed) through this machine's Realtek output in real time, and a live
microphone capture→encode→monitor session has run end to end. Building this path against real
hardware surfaced two genuine bugs neither unit tests nor silent/synthetic input would have
caught: a fixed submit-readiness threshold smaller than an actual chunk let the ring buffer
silently perform a partial write while reporting failure (corrupting the stream and desyncing
the submitted/rendered counters), and the live pipeline's Atmos metering step wrote past the
end of a buffer sized for the object count rather than the bed's fixed six channels. Both are
fixed; see `src/lib/src/platform/windows/monitor.cpp` and `run_live` in `src/cli/main.cpp`.

**Objects will not decode as objects in Dolby's decoder.** DD+ JOC gates object decoding on a
keyed, sequence-bound HMAC-SHA-256 tag in the EMDF `protection` field — which the standard
itself leaves "implementation dependent and not defined" — keyed on a secret embedded in
decoder binaries. Streams from here are spec-correct (FFmpeg validates them, the bed decodes
bit-exactly, Dolby's own parser reports `atmos=true`) but they are not signed, so Dolby's
decoder falls back to the 5.1 bed. The gate is authenticity, not conformance. Forging the tag
is deliberately not attempted. What is verified about reconstruction is the mathematics:
§6.6.6 applied per band recovers each object to better than −20 dB.

**Objects sharing a direction cannot be separated.** JOC reconstructs objects as a linear
combination of the five bed channels. Two objects at the same azimuth and different heights
get identical bed gains, so no matrix can pull them apart; the solve splits their energy by
power instead. This is what a parametric object coder is, not a defect in this encoder.

**`compr` in E-AC-3 has no external oracle.** FFmpeg's Annex E header parser reads `compre`
and then skips the word, so `-heavy_compr` changes nothing on an E-AC-3 stream however good
the metadata is. It is covered bit-by-bit instead ([tests/test_drc.cpp](tests/test_drc.cpp),
[tools/eac3_parse.py](tools/eac3_parse.py)).

### Portability

Built and tested with MSVC 14.51 and clang-cl 21.1 on Windows 11, and with gcc 15.2 and clang
21.1 on Ubuntu 26.04 (WSL2). The codec itself has no platform dependency; the three features
that touch sound hardware — capture, monitor playback and IEC 61937 passthrough — live in one
directory per audio subsystem that `src/lib/CMakeLists.txt` picks between: WASAPI on Windows,
ALSA on Linux, with a no-backend fallback (macOS, or Linux without libasound headers) that
reports itself unavailable rather than failing to link. See [Linux audio](docs/BUILDING.md#linux-audio)
for the ALSA backend specifically.

CI (`.github/workflows/ci.yml`) runs all five platform/compiler legs plus static analysis on
every push, and requires six of them: windows-msvc, windows-llvm, linux-gcc, linux-llvm,
linux-llvm-asan-ubsan (AddressSanitizer + UndefinedBehaviorSanitizer, `cmake/Sanitizers.cmake`)
and static-analysis (clang-tidy, `.clang-tidy`). Only macos-llvm remains experimental
(`continue-on-error`) — it has never run anywhere, on CI or otherwise, because the project has
no Mac; `src/lib/CMakeLists.txt` falls back to the no-backend platform directory there, so the
codec half is expected to work and the three audio-hardware commands to report themselves
unavailable, but neither has been observed. See the status table at the top of `ci.yml` for
exact test counts per leg.

**No Linux audio has been tried against real hardware.** The ALSA backend was verified headless
(including against ALSA's software `null` device, under ASan+UBSan) because the available Linux
environment is WSL2, which has no sound devices at all. Nothing has been bitstreamed to an
actual S/PDIF or HDMI output, and no AV receiver has been asked to lock onto it.

The GUI builds and runs on Linux too — `ac3gui` compiles and passes its headless `--smoke` run
against Ubuntu 26.04's Qt 6.10 — but `AC3FORGE_BUILD_GUI` still defaults `OFF` on every Linux
and macOS preset (pass `-DAC3FORGE_BUILD_GUI=ON` on a machine with Qt 6.5+; CI does not, since
its containers carry no Qt kit). macOS gets the no-backend fallback and has never been built at
all, GUI or otherwise.

## Building

Requirements: Visual Studio 2026 (MSVC), CMake ≥ 3.28, Ninja, a
[vcpkg](https://github.com/microsoft/vcpkg) checkout with `VCPKG_ROOT` set (it supplies
Catch2, and nothing else), and — for the GUI only — a prebuilt Qt 6.5+ kit. Qt is never taken
from vcpkg.

```bash
cmake --preset config-windows-msvc-debug
cmake --build --preset build-windows-msvc-debug
ctest --preset test-windows-msvc-debug
```

The Windows presets pin their own compiler — chainloading a toolchain file that finds MSVC via
`vswhere` and imports its build environment if a Developer PowerShell hasn't already set one up
— so this works from an ordinary shell as well as a Developer one.
[docs/BUILDING.md](docs/BUILDING.md) covers the full preset list, building without Qt, and the
machine-local preset pattern.

## Using the library

Two headers and about a dozen lines to encode a frame:

```cpp
#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"

ac3::FrameEncoder encoder{{
    .bitrate_kbps = 448,
    .acmod = ac3::Acmod::k3_2,  // L, C, R, SL, SR
    .lfe = true,
}};

// Table 5.8 order, LFE last, exactly kSamplesPerFrame (1536) samples each.
std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
// encode_frame takes a span of spans, so the views must outlive the call.
const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

for (int frame = 0; frame < 31; ++frame) {
    fill_with_audio(pcm, frame, 48000.0);
    if (const auto encoded = encoder.encode_frame(views)) {
        write(stream, *encoded);  // one complete syncframe
    }
}
```

That is [examples/encode_ac3.cpp](examples/encode_ac3.cpp) with the error handling elided.

[docs/LIBRARY.md](docs/LIBRARY.md) covers the rest: `ac3::eac3::FrameEncoder` and
`AccessUnitEncoder`, both decoders, `ac3::io::scan`, the spatial object layer, the Atmos
encoder, and `matroska::mux`. Every example in it is a program under [examples/](examples/)
that the build compiles and `ctest` runs, so none of them can quietly rot.

## Using the CLI

`ac3cli` has twenty-one commands. Run it with no arguments for the full listing.

```bash
ac3cli encode in.wav out.ac3 448 couple
```

```bash
ac3cli eac3-sine out.ec3 5 384 1000 50 714
```

```bash
ac3cli decode out.ec3 out.wav
```

`encode` takes 1–6 channel WAVs and picks the `acmod` to match. `eac3-sine` and
`eac3-silence` take a layout: `stereo | 51 | 71 | 512 | 514 | 714`. Metadata options
(`drc=`, `heavy`, `dialnorm=auto`, `cmixlev=`, …) follow the positional arguments in any
order. `silence`, `sine`, `atmos`, `atmos-path` (objects driven by an authored keyframe file
instead of the built-in orbit), `atmos-encode`, `orbit`, `eac3-encode`, `levels`, `loudness`,
`spdif`, `mkv`, `record`, `devices`, `outputs` and `play` (both `spdif` and `play` handle AC-3
and E-AC-3, bsid decides) are documented in the usage text, as are two live-audio commands:
`monitor` (decode and play a file on an ordinary, non-bitstreamed output — the shared-mode
preview path) and `live` (capture → encode → optional live monitor and/or IEC 61937
passthrough, running continuously and still writing the file `record` always has; `live`'s
`atmos` mode moves each object's placement every frame from elapsed time, the same shape
`atmos`'s synthetic orbit demo uses).

`ac3gui` is a Qt Quick front end over the same library: file and live-capture encoding, a plan
view for dragging objects around the room, a height slider, and channel-level metering.

## How it is validated

Four independent checks, in rough order of strength.

1. **The in-repo decoder.** Fully normative and sharing the encoder's core, so a round trip
   exercises the bit-allocation model in both directions. It reaches float32-precision PCM
   parity with FFmpeg's decoder on identical streams: max sample difference 7.9e-6 (≈ −102
   dBFS) for AC-3, 1.4e-5 for E-AC-3. It also reads FFmpeg's own encoder output.
2. **FFmpeg as an external oracle.** Every stream this project produces is strict-decoded with
   `-err_detect crccheck+bitstream+buffer+explode`, which fails on a CRC error, a bitstream
   violation or a buffer problem rather than concealing it.
3. **Independent Python transcriptions.** [tools/](tools/) holds second implementations of the
   spec pseudocode, written from the standard separately from the C++: the §7.2.2 bit
   allocation, the Tables 7.29/7.30 DRC lookups, MDCT goldens. Agreement between two
   transcriptions of the same text is weaker evidence than a decoder, but it catches
   transcription slips that a self-consistent round trip cannot.
4. **Dolby's own tooling as a syntax oracle.** The Reference Player and the Dolby Media
   Encoder were diffed field-for-field against this encoder's output during the object work.
   That found several real bugs — the EMDF container belonging in a skip field with `auxdatae`
   clear rather than in the aux field, `codecdatae=0`, a dynamic-object-only programme with the
   LFE as an object but not a JOC output, and metadata flag arrays transmitted index-0-first.

Quality is measured, not asserted. `tools/quality_race.py` synthesizes stereo programme
material, encodes it with both encoders at matched bit rates, decodes both with FFmpeg as a
neutral referee, aligns by cross-correlation, and reports SNR against the original:

| Bit rate | ac3forge | FFmpeg | Difference |
|---|---|---|---|
| 192 kbps | 41.23 dB | 40.98 dB | +0.25 |
| 256 kbps | 44.00 dB | 42.85 dB | +1.15 |
| 320 kbps | 45.09 dB | 44.15 dB | +0.94 |
| 448 kbps | 51.05 dB | 47.60 dB | +3.46 |

Measured with FFmpeg 8.0.1 on 2026-08-09; reproduce with `python tools/quality_race.py ac3`.
SNR on synthetic material is a narrow metric — it says the waveform is closer, not that it
sounds better, and no listening test has been run.

The test suite is 249 Catch2 unit tests plus the seven example programs: 256 ctest entries on
Windows, 270 on Linux where the ALSA backend adds 14 tests of its own
(`tests/platform/alsa/`).

```bash
ctest --preset test-windows-msvc-debug
```

## Repository layout

```
cmake/          FindQt6.cmake (prebuilt-Qt discovery), CompilerWarnings.cmake, Sanitizers.cmake,
                toolchains/ (one per platform/compiler preset), vcpkg/triplets/ (overlays)
src/lib/        ac3::forge — the whole codec, GUI-free
  include/ac3/  the public API: core/ encoder/ decoder/ meta/ spatial/ oba/
                emdf/ analysis/ sinks/ io/ capture/ platform/
  src/          implementation; src/platform/{windows,alsa,posix}/ selected by CMake
src/matroska/   matroska::matroska — a standalone MKV muxer, no ac3::forge dependency
src/cli/        ac3cli — command-line front end
src/gui/        ac3gui — Qt Quick front end (QML module "Ac3Forge")
tests/          Catch2 unit tests; golden/ vectors generated by tools/; platform/alsa/ when built
examples/       the programs docs/LIBRARY.md is written from
fuzz/           libFuzzer harnesses over untrusted-input entry points (Clang only, off by
                default); see fuzz/README.md
tools/          Python: spec-table generators, independent reference
                implementations, the FFmpeg quality race
docs/           see below
```

The standards documents are not redistributed. `docs/spec/` is gitignored; the table
generators in `tools/` read from it. To run them, fetch ATSC A/52:2018, ETSI TS 102 366 and
ETSI TS 103 420 (all free) plus the TS 103 420 companion archive `ts_103420v010201p0.zip`,
which is where the JOC Huffman tables actually live, and extract each PDF to page-marked text
beside it. [docs/BUILDING.md](docs/BUILDING.md) has the details.

## Documentation

| Document | Contents |
|---|---|
| [docs/BUILDING.md](docs/BUILDING.md) | Building from a clean clone, including the failures you will hit |
| [docs/LIBRARY.md](docs/LIBRARY.md) | The public API, with compiled examples |
| [docs/HISTORY.md](docs/HISTORY.md) | How the implementation was built, milestone by milestone |
| [docs/RESEARCH.md](docs/RESEARCH.md) | The original feasibility research and the decisions that came out of it |
| [fuzz/README.md](fuzz/README.md) | The libFuzzer harnesses: what they cover, how to run them locally |
| [docs/DESIGN-BRIEF.md](docs/DESIGN-BRIEF.md) | Input document for a GUI design pass: current-state inventory, user journeys, open questions |
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
