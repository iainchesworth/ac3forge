# Building ac3forge

Every command here has been run on the configuration described under
[Verified configuration](#verified-configuration). Anything not verified is marked as such.

## Requirements

| | Version | Notes |
|---|---|---|
| A compiler | MSVC (VS 2026), clang-cl 21, GCC 15, or Clang 21 | C++23. `std::expected`, `std::print` and deducing-`this` are all used. One [preset](#presets) per compiler; all four are required, green CI legs — see [Portability](../README.md#portability). |
| CMake | ≥ 3.28 | `cmake_minimum_required(VERSION 3.28...4.3)`. |
| Ninja | any recent | The presets hard-code the Ninja generator. |
| vcpkg | any recent | Supplies Catch2, and nothing else. Needed only when tests are on. |
| Qt | 6.5+ prebuilt | GUI only. **Never from vcpkg** — see [Qt](#qt). |
| Python 3 + numpy | 3.11+ | Only for `tools/`; not part of the build. |
| FFmpeg CLI | 8.x | Only for validation scripts; not part of the build. |

## The short version

With `VCPKG_ROOT` set to a vcpkg checkout, from any shell — a Developer PowerShell is not
required; see [The compiler is pinned, not PATH-found](#the-compiler-is-pinned-not-path-found):

```bash
cmake --preset config-windows-msvc-debug
```

```bash
cmake --build --preset build-windows-msvc-debug
```

```bash
ctest --preset test-windows-msvc-debug
```

Swap `-debug` off all three preset names for a Release build, or `msvc` for `llvm` to build
with clang-cl instead (experimental — see [Presets](#presets)).

## The compiler is pinned, not PATH-found

Every Windows preset chainloads `cmake/toolchains/windows.msvc.toolchain.cmake` (or
`windows.llvm.toolchain.cmake` for clang-cl), which finds `cl.exe`/`clang-cl.exe` and `link.exe`
by `find_program` against the MSVC tools directory, `NO_DEFAULT_PATH` — so whatever else is
first on `PATH` (LLVM installed for something unrelated, Git for Windows' own `link.exe`) cannot
be picked up by mistake the way a bare `find_package`-less configure would.

That toolchain directory has to come from somewhere. If `VCToolsInstallDir` and `INCLUDE` are
already set — a Developer PowerShell — it uses them. Otherwise
`cmake/toolchains/windows.msvc.environment.cmake` locates the newest Visual Studio install with
`vswhere`, runs its `vcvarsall.bat x64` in a subprocess, and imports the result into the CMake
process so every compiler check, `try_compile` and the actual `ninja` invocation inherit it —
which is what makes an ordinary shell work at all. The include/lib search paths are then baked
onto the compile and link lines themselves, not left in the environment, so the build tree stays
correct regardless of which shell later runs `cmake --build`.

The failure mode this leaves is not "wrong compiler picked up silently" but "no compiler found
at all": if no Visual Studio install carrying the `Microsoft.VisualStudio.Component.VC.Tools.x86.x64`
component exists, configure fails with a `FATAL_ERROR` naming what was missing (`vswhere.exe`,
or a matching VS install) rather than picking something else and failing later. Install the
Visual Studio Build Tools (or Community/Professional/Enterprise) with the "Desktop development
with C++" workload if you hit that.

## Presets

`CMakePresets.json` is checked in and holds only what is machine-independent. It is built from
hidden fragments composed together, not a flat list:

- `core` — the Ninja generator, the vcpkg toolchain file from `$env{VCPKG_ROOT}`,
  `CMAKE_EXPORT_COMPILE_COMMANDS`, and the vcpkg overlay triplets under `cmake/vcpkg/triplets/`.
- `debug` / `release` — just `CMAKE_BUILD_TYPE`.
- `windows-msvc`, `windows-llvm`, `linux-gcc`, `linux-llvm`, `macos-llvm` — one per
  platform/compiler pair. Each sets `VCPKG_TARGET_TRIPLET`, chainloads that platform's toolchain
  file (see [above](#the-compiler-is-pinned-not-path-found)) via `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`,
  and is gated by a `condition` on `hostSystemName` so only the presets for the machine you're on
  even appear. `AC3FORGE_BUILD_GUI` is `ON` for the two Windows ones and `OFF` for the rest — see
  [Portability](../README.md#portability).

Ten concrete `config-<platform>[-debug]` presets inherit `[ release|debug, <platform>, core ]`,
each with a matching `build-<platform>[-debug]` and `test-<platform>[-debug]` preset:

| Platform | Compiler | Configure preset | Build preset | Test preset |
|---|---|---|---|---|
| Windows | MSVC | `config-windows-msvc[-debug]` | `build-windows-msvc[-debug]` | `test-windows-msvc[-debug]` |
| Windows | clang-cl | `config-windows-llvm[-debug]` | `build-windows-llvm[-debug]` | `test-windows-llvm[-debug]` |
| Linux | GCC 15 | `config-linux-gcc[-debug]` | `build-linux-gcc[-debug]` | `test-linux-gcc[-debug]` |
| Linux | Clang 21 | `config-linux-llvm[-debug]` | `build-linux-llvm[-debug]` | `test-linux-llvm[-debug]` |
| macOS | AppleClang | `config-macos-llvm[-debug]` | `build-macos-llvm[-debug]` | `test-macos-llvm[-debug]` |

There is an eleventh configure/build/test trio, Debug-only and not part of the table above
because it isn't a platform/compiler pair but an instrumented variant of `linux-llvm`:
`config-linux-llvm-asan-ubsan` / `build-linux-llvm-asan-ubsan` / `test-linux-llvm-asan-ubsan`,
which inherits `linux-llvm` plus a `sanitize-asan-ubsan` fragment setting
`AC3FORGE_SANITIZERS=address,undefined` (see `cmake/Sanitizers.cmake`; MSVC is rejected outright,
so this only exists for GCC/Clang). See [Portability](../README.md#portability) for what CI says
about all eleven. There are also six `ci-<platform>` `workflowPresets` (Release except for the
asan-ubsan one, which is Debug-only) that chain configure→build→test in one
`cmake --workflow --preset ci-windows-msvc` call; that is exactly what CI itself runs.

Anything machine-specific belongs in `CMakeUserPresets.json`, which is gitignored. The pattern
is a hidden `local` preset carrying the paths, inherited alongside the checked-in fragments:

```json
{
  "version": 6,
  "configurePresets": [
    {
      "name": "local",
      "hidden": true,
      "environment": {
        "VCPKG_ROOT": "D:/vcpkg",
        "VCPKG_DOWNLOADS": "D:/vcpkg-downloads",
        "VCPKG_DEFAULT_BINARY_CACHE": "D:/vcpkg-cache"
      },
      "cacheVariables": {
        "VCPKG_INSTALL_OPTIONS": "--x-buildtrees-root=D:/vcpkg-buildtrees;--x-packages-root=D:/vcpkg-packages"
      }
    },
    { "name": "dev", "inherits": [ "local", "debug", "windows-msvc", "core" ] }
  ],
  "buildPresets": [ { "name": "dev", "configurePreset": "dev" } ],
  "testPresets": [
    { "name": "dev", "configurePreset": "dev", "output": { "outputOnFailure": true } }
  ]
}
```

`debug` alone has no generator or binary directory — those live on the hidden `core` preset,
and the compiler selection on a platform preset (`windows-msvc` here; see the table below for
the others). Missing either from `dev`'s `inherits` list still configures, but silently: CMake
falls back to its platform default generator (Visual Studio, on this machine) and an in-source
binary directory instead of `build/dev`, which is a mess to notice and worse to undo. Inherit
all four.

That keeps vcpkg's working directories off the system drive, which matters because they run to
several gigabytes. Substitute your own paths, and swap `windows-msvc` for whichever
platform/compiler fragment matches your machine.

## Options

| Option | Default | Effect |
|---|---|---|
| `AC3FORGE_BUILD_CLI` | `ON` | Build `ac3cli`. |
| `AC3FORGE_BUILD_GUI` | `ON` | Build `ac3gui`. Requires Qt. |
| `AC3FORGE_BUILD_TESTS` | `ON` | Build the Catch2 suite. Requires Catch2. |
| `AC3FORGE_FETCH_CATCH2` | `ON` | When no local Catch2 3 is found (vcpkg, a distro package, an explicit `CMAKE_PREFIX_PATH`), fetch and build v3.15.3 from source via `FetchContent` instead of failing. Turn off to insist on a package-manager copy — see `tests/CMakeLists.txt`. Irrelevant when `AC3FORGE_BUILD_TESTS` is off. |
| `AC3FORGE_BUILD_EXAMPLES` | `ON` | Build `examples/`, and register them as tests. |
| `AC3FORGE_WITH_ALSA` | `AUTO` | Linux only. `AUTO` builds the ALSA audio backend when libasound's headers are present; `ON` requires them; `OFF` never builds it. See [Linux audio](#linux-audio). |
| `AC3FORGE_SANITIZERS` | empty | Comma-separated `-fsanitize=` value, e.g. `address,undefined` — see `cmake/Sanitizers.cmake`. Empty is a no-op; GCC/Clang only, MSVC is a configure error. Set via the `-asan-ubsan` preset above rather than by hand. |
| `AC3FORGE_BUILD_FUZZERS` | `OFF` | Build the libFuzzer harnesses under `fuzz/`. Clang only (GCC and MSVC ship no libFuzzer); use `fuzz/run.sh` rather than this option directly — it configures a dedicated `build/fuzz` with the right compiler. See [`fuzz/README.md`](../fuzz/README.md). |
| `AC3FORGE_QUARANTINE_SIGNER` | `OFF` | Non-clean-room, local-only. Requires a gitignored `src/quarantine/` overlay that is never committed (a CI job fails the build if it ever is) and embeds a key extracted from Dolby's binary. A normal build neither sees nor references it — listed here only because the option itself is public in `CMakeLists.txt`. |

Building the library and CLI alone, with neither Qt nor vcpkg involved:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_BUILD_GUI=OFF -DAC3FORGE_BUILD_TESTS=OFF
```

The vcpkg toolchain file is still referenced by the preset, so `VCPKG_ROOT` must still point
at a checkout — it simply has nothing to install. To build with no vcpkg at all, configure
without the preset and pass the generator and build type by hand.

## Linux audio

Three of ac3forge's features touch the sound hardware — live capture (`ac3cli devices`,
`record`), monitor playback (`monitor`), and IEC 61937 bitstream passthrough (`outputs`,
`play`). Everything else is file I/O and needs no audio stack at all; `ac3cli spdif` in
particular reaches an AV receiver by writing a WAV, on any machine.

On Linux those three are implemented over **ALSA**, and the dependency is one package:

```bash
sudo apt-get install libasound2-dev
```

(`alsa-lib-devel` on Fedora, `alsa-lib` on Arch.) Nothing else is needed: no PipeWire or
PulseAudio development headers, no vcpkg port, no runtime daemon. Recording from the ALSA
`default` device goes through PipeWire or PulseAudio automatically wherever one is running,
because that is what those install themselves as.

The dependency is **optional and detected**. Configure reports which way it went:

```
-- ALSA 1.2.15.3: live capture, monitor playback and IEC 61937 passthrough enabled
--   Audio backend  : alsa
```

Without the headers, configure succeeds anyway and says so; the build then selects
`src/lib/src/platform/posix/`, whose entry points all return `kNoBackend`, and `ac3cli` marks
the affected commands `UNAVAILABLE HERE` in its usage rather than pretending they exist. Pass
`-DAC3FORGE_WITH_ALSA=ON` to turn a missing libasound into a configure error instead, which is
what a packaging build wants.

### Why ALSA and not PipeWire

Capture and monitor playback are ordinary PCM and every Linux audio API can do them. Passthrough
is the discriminator, and it is what the whole project is for: sending an AC-3 or E-AC-3
elementary stream down an S/PDIF or HDMI link so the receiver decodes it.

That is not a "format" on Linux the way it is on Windows. A bitstream is opened as plain 16-bit
stereo PCM, and what tells the receiver these bytes are Dolby Digital rather than music is the
IEC 60958 **channel status** travelling beside them — specifically the non-audio bit, AES0
bit 1. ALSA is where that bit is expressed (as arguments on the device name,
`iec958:CARD=PCH,DEV=0,AES0=0x06,…`). PulseAudio's `PA_STREAM_PASSTHROUGH` and PipeWire's
`SPA_MEDIA_SUBTYPE_iec958` are both real, and both end in the same ALSA call made by a daemon
instead of by us. So ALSA is not merely the lowest common denominator here — it is the layer
the other two are built on, it is present on every Linux system including ones running no sound
server at all, and its device string is what gives unmixed access to the hardware.

The cost is coexistence: opening a device directly takes it exclusively, so a running sound
server has to have released it. That is the same bargain WASAPI exclusive mode strikes on
Windows, for the same reason — a mixer that resamples or volume-scales a burst stream turns it
into noise. A PipeWire backend would be a reasonable second one to add (as a sibling directory
under `src/lib/src/platform/`, selected the same way); it would buy politeness, not capability.

### What has and has not been verified

Verified on WSL2 Ubuntu 26.04 with gcc 15.2 and clang 21.1, in every configuration: with
libasound present and absent, and under ASan+UBSan with leak detection. The full suite passes
in all of them. The device-independent halves of the backend — device-name construction,
channel-status derivation, the negotiation, the render and capture threads, start/stop, and the
error mapping — were additionally driven end to end against ALSA's software `null` PCM.

**Not verified: any real sound hardware.** WSL2 has no sound devices and no kernel sound
modules, so nothing here has been played to an actual S/PDIF or HDMI output, and no AV receiver
has been asked to lock onto the result. Whether a given output accepts a bitstream is
per-device anyway — `ac3cli outputs` probes each one and says.

## Qt

Qt is a **prebuilt dependency and never a vcpkg port**. Building Qt from source through vcpkg
takes hours and produces a kit that is harder to debug against than the official one.

`cmake/FindQt6.cmake` widens `CMAKE_PREFIX_PATH` to the usual prebuilt-kit install roots — on
Windows `C:/Qt`, `%USERPROFILE%/Qt`, `D:/Qt`; on Linux and macOS `~/Qt`, `/opt/Qt`, Homebrew and
MacPorts prefixes, and so on — newest kit first, and then defers to Qt's own config package. Every
Linux and macOS preset still forces `AC3FORGE_BUILD_GUI=OFF` by default — pass
`-DAC3FORGE_BUILD_GUI=ON` explicitly on a machine that has Qt 6.5+, which is verified to work on
Linux (`ac3gui` builds and passes its headless `--smoke` run against Ubuntu 26.04's Qt 6.10; CI
does not turn it on, since its containers carry no Qt kit) but has never been tried on macOS. See
[Portability](../README.md#portability). If your kit is somewhere else, say so explicitly and it
wins over the search — the project's own `-DAC3FORGE_QT_ROOT=` (or the `AC3FORGE_QT_ROOT`,
`QT_ROOT_DIR` or `QTDIR` environment variables) is the preferred way:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_QT_ROOT=D:/Qt/6.8.3/msvc2022_64
```

Plain `-DCMAKE_PREFIX_PATH=...` or `-DQt6_DIR=...` also work, and take priority over everything
`FindQt6.cmake` does. If you do not want the GUI, `-DAC3FORGE_BUILD_GUI=OFF` removes the
dependency entirely.

## The standards documents

`docs/spec/` is gitignored: the standards are free to download but are not redistributed here.
The build does not need them — every table is already transcribed into the source. They are
needed only to re-run the generators in `tools/`.

To set that up, fetch:

| Document | Why |
|---|---|
| ATSC A/52:2018 | The master standard. E-AC-3 is normative Annex E. |
| ETSI TS 102 366 | Carries the EMDF metadata format in Annex H. |
| ETSI TS 103 420 | Joint Object Coding. |
| `ts_103420v010201p0.zip` | The TS 103 420 companion archive. The JOC Huffman tables are in `ts_103420_tables.c` inside it, and nowhere in the PDF. |

Extract each PDF to page-marked text beside the PDF, with page separators of the form
`===== PDF PAGE n =====`. The generators locate tables by page.

## Verified configuration

Everything in this document was run on:

| | |
|---|---|
| OS | Windows 11 Pro for Workstations 10.0.26200 |
| Compiler | MSVC 14.51.36231 (Visual Studio 2026 Community) |
| CMake | ≥ 3.28, Ninja generator |
| Qt | 6.8.3 msvc2022_64 |
| vcpkg | checkout at `D:/vcpkg` |
| FFmpeg | 8.0.1 |
| Python | 3.14.6 |

Result: configure, build and `ctest` all clean, 256/256 tests passing.

The Linux legs of the table above were verified separately, on WSL2 Ubuntu 26.04 with gcc 15.2.0
and clang 21.1.8 (270/270 there — the extra 14 are `tests/platform/alsa/`, built only when the
ALSA backend is selected) — see [Linux audio](#linux-audio) for what that did and did not prove.
CI (`.github/workflows/ci.yml`) now runs and *requires* windows-msvc, windows-llvm, linux-gcc,
linux-llvm, linux-llvm-asan-ubsan and static-analysis (clang-tidy) on every push; only
macos-llvm remains experimental (`continue-on-error`) — it has never been run anywhere, on CI or
otherwise, because the project has no Mac. `src/lib/CMakeLists.txt` selects the no-backend
platform directory there, so the codec half is expected to work and the three audio-hardware
commands are expected to report themselves unavailable, but neither has been observed.
