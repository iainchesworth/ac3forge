# Building ac3forge

Every command here has been run on the configuration described under
[Verified configuration](#verified-configuration). Anything not verified is marked as such.

## Requirements

| | Version | Notes |
|---|---|---|
| MSVC | Visual Studio 2026 | C++23. `std::expected`, `std::print` and deducing-`this` are all used. |
| CMake | ≥ 3.28 | `cmake_minimum_required(VERSION 3.28...4.3)`. |
| Ninja | any recent | The presets hard-code the Ninja generator. |
| vcpkg | any recent | Supplies Catch2, and nothing else. Needed only when tests are on. |
| Qt | 6.5+ prebuilt | GUI only. **Never from vcpkg** — see [Qt](#qt). |
| Python 3 + numpy | 3.11+ | Only for `tools/`; not part of the build. |
| FFmpeg CLI | 8.x | Only for validation scripts; not part of the build. |

## The short version

From a **Developer PowerShell for VS 2026** (or any shell with the MSVC environment loaded),
with `VCPKG_ROOT` set:

```bash
cmake --preset debug
```

```bash
cmake --build --preset debug
```

```bash
ctest --preset debug
```

`release` presets exist alongside `debug` and take the same three commands.

## The shell has to have MSVC in it

This is the failure you are most likely to hit, so it comes first.

The presets deliberately do not pin `CMAKE_CXX_COMPILER`. That keeps them portable, but it
means CMake picks the first C++ compiler on `PATH`. On a machine with LLVM installed, that is
usually `clang++`, and the build then fails with a large number of errors that have nothing to
do with your change:

```
error: implicit conversion changes signedness: 'const int' to 'size_type'
      [-Werror,-Wsign-conversion]
```

or, if the C++ standard library headers cannot be found at all:

```
fatal error: cannot open include file: 'cstddef'
```

Neither is a real problem with the source. Check which compiler was configured:

```bash
grep CMAKE_CXX_COMPILER: build/debug/CMakeCache.txt
```

It must be `cl.exe`. If it is anything else, delete the build directory and reconfigure from a
Developer PowerShell — the cached compiler is not something reconfiguring will change on its
own.

To load the MSVC environment into an ordinary PowerShell session:

```powershell
cmd /c "`"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat`" >nul 2>&1 && set" | ForEach-Object { if ($_ -match '^(INCLUDE|LIB|LIBPATH|PATH)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
```

Adjust the edition in that path (`Community`, `Professional`, `Enterprise`) to match your
install.

## Presets

`CMakePresets.json` is checked in and holds only what is machine-independent:

| Preset | Build type | Binary directory |
|---|---|---|
| `debug` | Debug | `build/debug` |
| `release` | Release | `build/release` |

Both inherit a hidden `base` preset that sets the Ninja generator, the vcpkg toolchain file
from `$env{VCPKG_ROOT}`, and `CMAKE_EXPORT_COMPILE_COMMANDS`.

Anything machine-specific belongs in `CMakeUserPresets.json`, which is gitignored. The pattern
is a hidden `local` preset carrying the paths, inherited alongside a checked-in one:

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
several gigabytes. Substitute your own paths.

## Options

| Option | Default | Effect |
|---|---|---|
| `AC3FORGE_BUILD_CLI` | `ON` | Build `ac3cli`. |
| `AC3FORGE_BUILD_GUI` | `ON` | Build `ac3gui`. Requires Qt. |
| `AC3FORGE_BUILD_TESTS` | `ON` | Build the Catch2 suite. Requires Catch2 from vcpkg. |
| `AC3FORGE_BUILD_EXAMPLES` | `ON` | Build `examples/`, and register them as tests. |
| `AC3FORGE_WITH_ALSA` | `AUTO` | Linux only. `AUTO` builds the ALSA audio backend when libasound's headers are present; `ON` requires them; `OFF` never builds it. See [Linux audio](#linux-audio). |

Building the library and CLI alone, with neither Qt nor vcpkg involved:

```bash
cmake --preset debug -DAC3FORGE_BUILD_GUI=OFF -DAC3FORGE_BUILD_TESTS=OFF
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

`cmake/FindQt6.cmake` widens `CMAKE_PREFIX_PATH` to the usual install roots
(`C:/Qt/6.x/msvc2022_64`, `~/Qt`, `/opt/Qt`, and so on) and then defers to Qt's own config
package. If your kit is somewhere else, say so explicitly and it wins over the search:

```bash
cmake --preset debug -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64
```

`-DQt6_DIR=...` also works. If you do not want the GUI, `-DAC3FORGE_BUILD_GUI=OFF` removes the
dependency entirely.

## Packaging

`cmake/Packaging.cmake` wires CPack up behind the platform preset matrix. A plain ZIP is
always produced; NSIS (Windows), DEB/RPM (Linux) and DragNDrop (macOS) are added on top when
the packaging tool for that format is found on `PATH`, so `cpack` degrades gracefully instead
of failing outright on a machine that does not have e.g. `makensis` installed.

From a Developer PowerShell, with `VCPKG_ROOT` set:

```bash
cmake --preset config-windows-msvc
```

```bash
cmake --build --preset build-windows-msvc
```

```bash
cpack --preset pack-windows-msvc
```

The equivalent `pack-<platform>` preset exists for every entry in the platform matrix
(`pack-windows-llvm`, `pack-linux-gcc`, `pack-linux-llvm`, `pack-macos-llvm`), though only the
Windows ones have ever actually been run — see [Verified configuration](#verified-configuration)
and the note in `cmake/Packaging.cmake` about Linux Qt packaging not applying yet, since
`AC3FORGE_BUILD_GUI` defaults off on every non-Windows preset even though Qt itself can be found
there (see [Linux audio](#linux-audio)'s sibling section on Qt, and the linux-gcc preset's own
description in `CMakePresets.json`). Packages land in `packages/` at the repository root.
`cmake --build --preset build-windows-msvc --target pack-ac3forge` runs the same thing from
inside an IDE's target list instead of the command line.

Only `ac3cli` and `ac3gui` are installed/packaged; `ac3::forge` and `matroska::matroska` are
link-only libraries, not a public API this project ships standalone.

CI packages the `windows-msvc` leg on every push and uploads the result as a workflow
artifact (`.github/workflows/_build.yml`), so the packaging path is exercised continuously
rather than only when someone remembers to run it locally.

A tag-triggered release workflow (`.github/workflows/release.yml`) builds, signs, attests and
publishes packages for every platform in the matrix (best-effort beyond windows-msvc) whenever a
`vX.Y.Z` tag is pushed — GPG signing (optional, off until a key is provisioned), keyless
Sigstore/OIDC build provenance, an SPDX SBOM, and a GitHub Release. See
[docs/releasing.md](releasing.md) for the full process, including how to provision the GPG key.

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

Result: configure, build and `ctest` all clean, 182/182 tests passing.

The Linux legs of the table above were run separately, on WSL2 Ubuntu 26.04 with gcc 15.2.0 and
clang 21.1.8 — see [Linux audio](#linux-audio) for what that did and did not prove. macOS has
never been built: nobody working on this has a Mac. `src/lib/CMakeLists.txt` selects the
no-backend implementations there, so the codec half is expected to work and the three
audio-hardware commands are expected to report themselves unavailable, but neither has been
observed.
