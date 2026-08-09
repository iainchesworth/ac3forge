# ac3forge

A clean-room AC-3 and E-AC-3 encoder and decoder in C++23, implemented from the published
standards. It turns PCM — or mono sources placed and moved in 3D space — into AC-3, E-AC-3,
or E-AC-3 with Joint Object Coding elementary streams, and reads those streams back.

**Documentation:** [iainchesworth.github.io/ac3forge](https://iainchesworth.github.io/ac3forge/) —
a beginner's guide to the formats, a developer quick start, the full library reference, the CLI
reference, and a step-by-step GUI guide with screenshots.

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
clang-cl), Linux (GCC 15, Clang 21) and macOS (Homebrew LLVM) — CLI and GUI alike on the first
four, CLI only on macOS — plus an ASan+UBSan leg, clang-tidy static analysis, a per-platform
gold-reference *quality* gate, and a dedicated Linux FFmpeg-validation leg checking output
*correctness* across the full option space. No leg remains experimental. See
[Portability](#portability).

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
| Bit rates | CBR only — the 19 nominal rates of Table 5.18, 32–640 kbps | CBR (the same 19, per substream) or VBR — a quality target with optional min/max kbps bounds, per substream |
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
| Variable bit rate on AC-3 | `frmsizecod` indexes Table 5.18 rather than stating a word count directly, so AC-3 has no free frame size to vary at all and stays CBR. E-AC-3 supports VBR — see [Encoding E-AC-3](docs/library/encoding-eac3.md#variable-bit-rate-frameconfigvbr). |

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
21.1 on Ubuntu 26.04 (WSL2) — CLI and GUI on every one of those four. The codec itself has no
platform dependency; the three features that touch sound hardware — capture, monitor playback
and IEC 61937 passthrough — live in one directory per audio subsystem that
`src/lib/CMakeLists.txt` picks between: WASAPI on Windows, ALSA on Linux, with a no-backend
fallback (macOS, or Linux without libasound headers) that reports itself unavailable rather than
failing to link. See [Linux audio](docs/BUILDING.md#linux-audio) for the ALSA backend
specifically.

CI (`.github/workflows/ci.yml`) runs all six platform/compiler legs plus static analysis and
FFmpeg validation on every push, and requires eight jobs: windows-msvc, windows-llvm, linux-gcc,
linux-llvm, linux-llvm-asan-ubsan (AddressSanitizer + UndefinedBehaviorSanitizer,
`cmake/Sanitizers.cmake`), macos-llvm, static-analysis (clang-tidy, `.clang-tidy`) and
ffmpeg-validate. No leg remains experimental — macos-llvm was the last promoted, once a real
GitHub Actions run (this project has no Mac) confirmed 256/256 tests and the gold-reference gate
both green.

ffmpeg-validate is a separate, CLI-only linux-llvm build that runs `scripts/run-codec-matrix.sh`'s
FFmpeg strict-decode checks, `tools/check_drc.py`, `tools/check_coupling.py`/
`check_coupling_level.py`, and `tools/quality_race.py ci` against a pinned FFmpeg, plus
`tools/check_matrix_coverage.py` to catch a new layout, Annex E tool token or command landing
without a matching matrix entry (see [Oracles](CONTRIBUTING.md#oracles)). It answers a different
question from the gold-reference gate every other leg also runs (see
[docs/BUILDING.md](docs/BUILDING.md#gold-reference-correctness-gate)): gold-reference checks that
one fixed sample decodes to the *right audio*, identically enough across every compiler/OS; this
leg checks that *every option in the encoder's surface* — every layout, every Annex E tool token,
every metadata flag — produces a *structurally valid* stream at all, something one fixed sample
can never exercise.

The two Linux legs install a Qt6 kit and build `ac3gui` too (`-DAC3FORGE_BUILD_GUI=ON`, on top of
the preset's own default `OFF`), then run it headless via `--smoke`; `linux-llvm-asan-ubsan`
stays CLI-only on purpose, to keep a Qt kit out of the sanitizer leg's install time.
`src/lib/CMakeLists.txt` falls back to the no-backend platform directory on macOS, so the codec
half is verified there but the three audio-hardware commands only report themselves unavailable,
never tested against real hardware. See the status table at the top of `ci.yml` for exact test
counts per leg — and its own caveat that the two Linux legs' `GREEN*` marking means "confirmed by
a local WSL2 run", pending that push's first real hosted CI run.

**No Linux audio has been tried against real hardware.** The ALSA backend was verified headless
(including against ALSA's software `null` device, under ASan+UBSan) because the available Linux
environment is WSL2, which has no sound devices at all. Nothing has been bitstreamed to an
actual S/PDIF or HDMI output, and no AV receiver has been asked to lock onto it.

No macOS host exists for this project, so `macos-llvm` remains unverified — it gets the
no-backend fallback and has never been built at all. See
[docs/BUILDING.md](docs/BUILDING.md)'s [Verified configuration](docs/BUILDING.md#verified-configuration)
for exact toolchain versions.

## Building

Requirements: CMake ≥ 3.28, Ninja, a [vcpkg](https://github.com/microsoft/vcpkg) checkout with
`VCPKG_ROOT` set (it supplies Catch2, and nothing else), and — for the GUI only — a prebuilt
Qt 6.5+ kit. Qt is never taken from vcpkg. On Windows that means Visual Studio 2026 (MSVC) or
clang-cl; on Linux, GCC ≥ 15 or Clang ≥ 21.

```bash
cmake --preset config-windows-msvc-debug
cmake --build --preset build-windows-msvc-debug
ctest --preset test-windows-msvc-debug
```

Every preset pins its own compiler rather than trusting `PATH` — the Windows ones chainload a
toolchain file that finds MSVC via `vswhere` and imports its build environment if a Developer
PowerShell hasn't already set one up, so this works from an ordinary shell too; the Linux ones
`find_program` an exact GCC/Clang version the same way. On Linux, substitute the
`config-linux-gcc-debug` / `build-linux-gcc-debug` / `test-linux-gcc-debug` presets (or `-llvm-`
for Clang); the GUI is opt-in there via `-DAC3FORGE_BUILD_GUI=ON` rather than on by default.
[docs/BUILDING.md](docs/BUILDING.md) covers the full preset list, building without Qt, the
Linux GUI opt-in, and the machine-local preset pattern.

**Packaging.** `cpack --preset pack-windows-msvc` (and the equivalent `pack-<platform>` preset
for every leg in the matrix, though only Windows has actually been run) produces a ZIP, plus
NSIS/DEB/RPM/DragNDrop on top where the platform packaging tool is available. A `vX.Y.Z` tag
pushed to `main` additionally triggers [`release.yml`](.github/workflows/release.yml), which
builds, signs and publishes packages as a GitHub Release. See
[docs/BUILDING.md#packaging](docs/BUILDING.md#packaging) and
[docs/releasing.md](docs/releasing.md).

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

[docs/library/](docs/library/index.md) covers the rest: `ac3::eac3::FrameEncoder` and
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

`ac3cli --version` (a flag, not one of the twenty-one commands) prints the semantic version plus
git provenance — commit, branch, dirty flag — stamped in at build time by
`cmake/GenerateVersion.cmake`.

## How it is validated

Four independent checks, in rough order of strength.

1. **The in-repo decoder.** Fully normative and sharing the encoder's core, so a round trip
   exercises the bit-allocation model in both directions. It reaches float32-precision PCM
   parity with FFmpeg's decoder on identical streams: max sample difference 7.9e-6 (≈ −102
   dBFS) for AC-3, 1.4e-5 for E-AC-3. It also reads FFmpeg's own encoder output.
2. **FFmpeg as an external oracle.** Every stream this project produces is strict-decoded with
   `-xerror -err_detect crccheck+bitstream+buffer+explode`, which fails on a CRC error, a
   bitstream violation or a buffer problem rather than concealing it (`-xerror` is what turns a
   detected error into a failing process — `-err_detect` alone does not change FFmpeg's exit
   code). Automated and required in CI; see [Oracles](CONTRIBUTING.md#oracles).
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
                Packaging.cmake (CPack), GenerateVersion.cmake (git-provenance version stamping),
                toolchains/ (one per platform/compiler preset), vcpkg/triplets/ (overlays)
src/lib/        ac3::forge — the whole codec, GUI-free
  include/ac3/  the public API: core/ encoder/ decoder/ meta/ spatial/ oba/
                emdf/ analysis/ sinks/ io/ capture/ platform/
  src/          implementation; src/platform/{windows,alsa,posix}/ selected by CMake
src/matroska/   matroska::matroska — a standalone MKV muxer, no ac3::forge dependency
src/cli/        ac3cli — command-line front end
src/gui/        ac3gui — Qt Quick front end (QML module "Ac3Forge")
tests/          Catch2 unit tests; golden/ vectors generated by tools/; platform/alsa/ when built
examples/       the programs docs/library/ is written from
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

The published site — [iainchesworth.github.io/ac3forge](https://iainchesworth.github.io/ac3forge/)
— is the best-in-class starting point: a beginner's guide to AC-3/E-AC-3/Atmos/JOC, a developer
quick start, the full library reference, the CLI reference, and a step-by-step GUI guide with
screenshots. The table below is the same material's in-repo source, plus a few things that don't
belong on the public site.

| Document | Contents |
|---|---|
| [docs/quickstart.md](docs/quickstart.md) | Developer quick start: clone to first encode |
| [docs/BUILDING.md](docs/BUILDING.md) | Building from a clean clone, including the failures you will hit |
| [docs/platforms/](docs/platforms/windows.md) | Windows / Linux / macOS specifics: toolchains, audio backends, packaging |
| [docs/releasing.md](docs/releasing.md) | Cutting a release: versioning, the tag-triggered workflow, GPG signing |
| [docs/concepts/](docs/concepts/index.md) | Beginner's guide to AC-3, E-AC-3, Atmos and JOC, with diagrams |
| [docs/library/](docs/library/index.md) | The public API, with compiled examples |
| [docs/cli/](docs/cli/index.md) | The `ac3cli` reference: all 21 commands, metadata options |
| [docs/gui/](docs/gui/index.md) | Step-by-step `ac3gui` guide, with screenshots |
| [docs/project/history.md](docs/project/history.md) | How the implementation was built, milestone by milestone |
| [docs/project/research.md](docs/project/research.md) | The original feasibility research and the decisions that came out of it |
| [docs/quality-trend.md](docs/quality-trend.md) | Gold-reference SNR history by commit, develop vs. main - the persisted half of `research.md`'s L3/L4 validation pyramid |
| [fuzz/README.md](fuzz/README.md) | The libFuzzer harnesses: what they cover, how to run them locally |
| [docs/project/gui-design-brief.md](docs/project/gui-design-brief.md) | Superseded input document for the GUI redesign: current-state inventory at the time, user journeys, open questions |
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
