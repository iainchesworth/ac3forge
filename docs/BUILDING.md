# Building ac3forge

Every command here has been run on the configuration described under
[Verified configuration](#verified-configuration). Anything not verified is marked as such.

## Requirements

| | Version | Notes |
|---|---|---|
| MSVC | Visual Studio 2026 | Windows only. C++23. `std::expected`, `std::print` and deducing-`this` are all used. |
| GCC | ≥ 15 | Linux only. Same C++23 feature set as MSVC above. |
| Clang | ≥ 21 | Linux only, via `clang-cl` on Windows (MSVC-ABI compatible). |
| CMake | ≥ 3.28 | `cmake_minimum_required(VERSION 3.28...4.3)`. |
| Ninja | any recent | The presets hard-code the Ninja generator. |
| vcpkg | any recent | Supplies Catch2, and nothing else. Needed only when tests are on. |
| Qt | 6.5+ prebuilt | GUI only. **Never from vcpkg** — see [Qt](#qt). |
| ALSA (`libasound2-dev`) | any recent | Linux only, optional. Live capture/monitor/passthrough — see [Linux audio](#linux-audio). |
| Python 3 + numpy | 3.11+ | Only for `tools/`; not part of the build. |
| FFmpeg CLI | 8.x | Only for validation scripts; not part of the build. |

## The short version

From a **Developer PowerShell for VS 2026** (or any shell with the MSVC environment loaded),
with `VCPKG_ROOT` set:

```bash
cmake --preset config-windows-msvc-debug
```

```bash
cmake --build --preset build-windows-msvc-debug
```

```bash
ctest --preset test-windows-msvc-debug
```

Drop `-debug` from all three preset names for a Release build. The `ci-windows-msvc` workflow
preset runs the same three steps in one command: `cmake --workflow --preset ci-windows-msvc`
(Release only — the workflow presets in `CMakePresets.json` don't have `-debug` variants).

See [Building on Linux](#building-on-linux) below for the equivalent on GCC/Clang.

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
grep CMAKE_CXX_COMPILER: build/config-windows-msvc-debug/CMakeCache.txt
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

`CMakePresets.json` is checked in and holds only what is machine-independent. Every concrete,
directly-usable preset is named `config-<platform>-<compiler>[-debug]`, with matching
`build-` and `test-` presets and a binary directory of `build/<config-preset-name>`:

| Platform / compiler | Configure preset (Release) | Configure preset (Debug) |
|---|---|---|
| Windows / MSVC | `config-windows-msvc` | `config-windows-msvc-debug` |
| Windows / clang-cl | `config-windows-llvm` | `config-windows-llvm-debug` |
| Linux / GCC | `config-linux-gcc` | `config-linux-gcc-debug` |
| Linux / Clang | `config-linux-llvm` | `config-linux-llvm-debug` |
| macOS / AppleClang (unverified) | `config-macos-llvm` | `config-macos-llvm-debug` |

`build-<name>` and `test-<name>` presets exist for every row above, and a `ci-<platform>-<compiler>`
workflow preset (Release only) chains configure/build/test in one `cmake --workflow --preset ...`
call. There is also `config-linux-llvm-asan-ubsan` (+ `build-`/`test-`/`ci-`), a Debug preset with
AddressSanitizer and UndefinedBehaviorSanitizer on — see that preset's `description` in
`CMakePresets.json`.

None of these top-level names — `config-windows-msvc-debug` and so on — are inherited directly.
Each is composed from smaller hidden fragments (`debug`/`release` for the build type,
`windows-msvc`/`linux-gcc`/etc. for the toolchain, `core` for the generator and vcpkg wiring).
That composition is also the supported way to build your own machine-local preset: anything
machine-specific belongs in `CMakeUserPresets.json`, which is gitignored. The pattern is a
hidden `local` preset carrying the paths, inherited alongside the checked-in fragments:

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
and the compiler selection on a platform preset (`windows-msvc` here; `linux-gcc`, `linux-llvm`
and `macos-llvm` are the others — see `CMakePresets.json`). Missing either from `dev`'s
`inherits` list still configures, but silently: CMake
falls back to its platform default generator (Visual Studio, on this machine) and an in-source
binary directory instead of `build/dev`, which is a mess to notice and worse to undo. Inherit
all four.

That keeps vcpkg's working directories off the system drive, which matters because they run to
several gigabytes. Substitute your own paths.

## Options

| Option | Default | Effect |
|---|---|---|
| `AC3FORGE_BUILD_CLI` | `ON` | Build `ac3cli`. |
| `AC3FORGE_BUILD_GUI` | `ON` on Windows/macOS presets, `OFF` on Linux presets | Build `ac3gui`. Requires Qt. Off by default on Linux because a Qt kit isn't assumed present there — see [Building on Linux](#building-on-linux). |
| `AC3FORGE_BUILD_TESTS` | `ON` | Build the Catch2 suite. Requires Catch2 from vcpkg. |
| `AC3FORGE_BUILD_EXAMPLES` | `ON` | Build `examples/`, and register them as tests. |
| `AC3FORGE_WITH_ALSA` | `AUTO` | Linux only. `AUTO` builds the ALSA audio backend when libasound's headers are present; `ON` requires them; `OFF` never builds it. See [Linux audio](#linux-audio). |

Building the library and CLI alone, with neither Qt nor vcpkg involved:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_BUILD_GUI=OFF -DAC3FORGE_BUILD_TESTS=OFF
```

The vcpkg toolchain file is still referenced by the preset, so `VCPKG_ROOT` must still point
at a checkout — it simply has nothing to install. To build with no vcpkg at all, configure
without the preset and pass the generator and build type by hand.

## Qt

Qt is a **prebuilt dependency and never a vcpkg port**. Building Qt from source through vcpkg
takes hours and produces a kit that is harder to debug against than the official one.

`cmake/FindQt6.cmake` widens `CMAKE_PREFIX_PATH` to the usual install roots
(`C:/Qt/6.x/msvc2022_64`, `~/Qt`, `/opt/Qt`, and so on) and then defers to Qt's own config
package. If your kit is somewhere else, say so explicitly and it wins over the search:

```bash
cmake --preset config-windows-msvc-debug -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64
```

`-DQt6_DIR=...` also works. If you do not want the GUI, `-DAC3FORGE_BUILD_GUI=OFF` removes the
dependency entirely.

## Building on Linux

`config-linux-gcc` and `config-linux-llvm` (each with a `-debug` variant, same as the Windows
presets) are GCC 15 and Clang 21 respectively. They do **not** share the `debug`/`release` bare
names used elsewhere in this document — there is no `cmake --preset debug` on any platform; see
[Presets](#presets) above.

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset config-linux-gcc-debug
cmake --build --preset build-linux-gcc-debug
ctest --preset test-linux-gcc-debug
```

Substitute `linux-llvm` for `linux-gcc` to build with Clang instead. `VCPKG_ROOT` works the same
way as on Windows: it must point at a vcpkg checkout for the toolchain file the preset
references, even though (as on Windows) it supplies nothing but Catch2. This project's own
convention keeps that checkout under `/opt/vcpkg`, but any path works — there is nothing
Linux-specific about vcpkg here.

### GUI on Linux

Both Linux presets default `AC3FORGE_BUILD_GUI` to `OFF`. That is not because the GUI cannot be
built on Linux — `cmake/FindQt6.cmake` resolves a Linux Qt kit the same way it resolves a
Windows one (distro packages land on CMake's own prefixes; relocated or `aqtinstall` kits are
searched under `~/Qt`, `/opt/Qt` and friends), and `ac3gui` builds clean and passes its headless
`--smoke` run under both Linux presets. It defaults off because, unlike on Windows/macOS, a Qt
kit is not assumed to be present on every Linux machine that builds this project — see
`linux-gcc`'s own `description` in `CMakePresets.json`. Opt in explicitly once Qt is installed:

```bash
cmake --preset config-linux-gcc-debug -DAC3FORGE_BUILD_GUI=ON
```

Qt 6.5+ is required, same as everywhere else. On Debian/Ubuntu:

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools
```

Other distros need the equivalent Qt6 base + declarative (QML/Quick) development packages;
package names vary (Fedora's are `qt6-qtbase-devel` / `qt6-qtdeclarative-devel`, for example).

**The `qmlshapesplugin` / `labsmodelsplugin` / `qmlfolderlistmodelplugin` CMake warnings.**
Configuring with the GUI on prints warnings that these — and, in fact, every other built-in
QML plugin `ac3gui` transitively touches through `QtQuick.Controls` (its styles, dialogs,
layouts, and so on) — "will not be linked", because the `Qt6::<name>plugin` CMake target each
would need does not exist. This is a property of how Ubuntu's apt-packaged Qt6 is built, not of
this project: the official Qt installer exports a static-link CMake target for every built-in
plugin so a fully self-contained executable can embed them, but a distro's dynamically-linked
Qt6 package does not need that and does not export it. It does **not** mean the plugins are
missing. Confirmed on Ubuntu 26.04's Qt 6.10: the `.so` files are installed at the normal QML
import path with valid `qmldir` files, and the QML engine loads them from there at runtime the
same way it loads every other Qt Quick module, independent of whether CMake could statically
link them in. `ac3gui`'s own QML never imports `Qt.labs.*` or `QtQuick.Shapes` directly — the
three named in the warning are pulled in transitively by `QtQuick.Dialogs`' non-native
fallback implementation, which backs `Main.qml`'s `FileDialog`s only when no native/portal
dialog is available, and which a headless `--smoke` run (verified with
`QT_LOGGING_RULES=qt.qml.import.debug=true`) never even requests. Safe to ignore.

### Linux audio

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

#### Why ALSA and not PipeWire

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

#### What has and has not been verified

Verified on WSL2 Ubuntu 26.04 with gcc 15.2 and clang 21.1, in every configuration: with
libasound present and absent, and under ASan+UBSan with leak detection. The full suite passes
in all of them. The device-independent halves of the backend — device-name construction,
channel-status derivation, the negotiation, the render and capture threads, start/stop, and the
error mapping — were additionally driven end to end against ALSA's software `null` PCM.

**Not verified: any real sound hardware.** WSL2 has no sound devices and no kernel sound
modules, so nothing here has been played to an actual S/PDIF or HDMI output, and no AV receiver
has been asked to lock onto the result. Whether a given output accepts a bitstream is
per-device anyway — `ac3cli outputs` probes each one and says.

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

The Windows instructions in this document were run on:

| | |
|---|---|
| OS | Windows 11 Pro for Workstations 10.0.26200 |
| Compiler | MSVC 14.51.36231 (Visual Studio 2026 Community) |
| CMake | ≥ 3.28, Ninja generator |
| Qt | 6.8.3 msvc2022_64 |
| vcpkg | checkout at `D:/vcpkg` |
| FFmpeg | 8.0.1 |
| Python | 3.14.6 |

Result: configure, build and `ctest` all clean, 256/256 tests passing (windows-msvc and
windows-llvm both — see `.github/workflows/ci.yml`'s status comment).

The Linux instructions were run on:

| | |
|---|---|
| OS | Ubuntu 26.04 (WSL2) |
| Compilers | GCC 15.2.0 and Clang 21.1.x, both tried |
| CMake | ≥ 3.28, Ninja generator |
| Qt | 6.10.2, apt-packaged (`qt6-base-dev`, `qt6-declarative-dev`) |
| ALSA | `libasound2-dev`, both present and as the no-ALSA fallback — see [Linux audio](#linux-audio) |
| vcpkg | checkout at `/opt/vcpkg` |

Result: configure, build and `ctest` all clean on both compilers, GUI and ALSA both included —
270/270 tests, `AC3FORGE_WITH_ALSA`'s `tests/platform/alsa/` accounting for the 14-test gap over
the 256/256 a Linux build without `libasound2-dev` gets (same count as Windows, since GUI does
not gate any `ctest` entry — it only adds the separate `ac3gui` build target). `ac3gui --smoke`
also runs clean headless (`QT_QPA_PLATFORM=offscreen`), encoding real audio and instantiating
real QML channel meters. See [Linux audio](#linux-audio) for what the ALSA verification did,
and did not (real hardware), prove.

No macOS host exists for this project, so `config-macos-llvm`/`config-macos-llvm-debug` have
never been configured, built or tested — `src/lib/CMakeLists.txt` selects the no-backend audio
implementations there (same as a Linux machine without `libasound2-dev`), so the codec and GUI
halves are expected to work and the three audio-hardware commands are expected to report
themselves unavailable, but none of that has been observed. Treat macOS as unverified until
someone with a Mac tries it.
