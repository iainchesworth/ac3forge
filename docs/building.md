# Building ac3forge

Every command here has been run on the configuration described under
[Verified configuration](#verified-configuration). Anything not verified is marked as such.

## Requirements

| | Version | Notes |
|---|---|---|
| A compiler | MSVC (VS 2026), clang-cl 21, GCC 15, or Clang 21 | C++23. `std::expected`, `std::print` and deducing-`this` are all used. One [preset](#presets) per compiler; all seven platform/compiler legs are required, green CI (GCC 15 covers two of them — `linux-gcc` and `linux-gcc-arm64`; Clang 21 covers three — `linux-llvm`, `linux-llvm-arm64` and `macos-llvm`, each as a separate leg, though `macos-llvm` deliberately tracks Homebrew's unpinned `llvm` formula, currently also 21, rather than an exact pin) — see [Verified configuration](#verified-configuration). |
| CMake | ≥ 3.28 | `cmake_minimum_required(VERSION 3.28...4.3)`. |
| Ninja | any recent | The presets hard-code the Ninja generator. |
| vcpkg | any recent | Supplies Catch2 (needed only when tests are on); with `-DVCPKG_MANIFEST_FEATURES=adm`, the Boost header libraries `AC3FORGE_BUILD_ADM=ON` needs; and with `-DVCPKG_MANIFEST_FEATURES=profiling`, the Tracy profiler `AC3FORGE_ENABLE_TRACY=ON` needs — see [Options](#options). None of the three is required for a default build. |
| Qt | 6.5+ prebuilt | GUI only. **Never from vcpkg** — see [Qt](#qt). |
| ALSA (`libasound2-dev`) | any recent | Linux only, optional. Live capture/monitor/passthrough — see [Linux audio](#linux-audio). |
| PipeWire (`libpipewire-0.3-dev`) | any recent | Linux only, optional, used only when ALSA is not — see [Linux audio](#linux-audio). |
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

Drop `-debug` from all three preset names for a Release build, or swap `msvc` for `llvm` to
build with clang-cl instead. The `ci-windows-msvc` workflow preset runs the same three steps in
one command: `cmake --workflow --preset ci-windows-msvc` (Release only — the workflow presets in
`CMakePresets.json` don't have `-debug` variants).

See [Building on Linux](#building-on-linux) below for the equivalent on GCC/Clang.

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
  `AC3FORGE_BUILD_CLI`/`AC3FORGE_BUILD_TESTS` pinned `ON`, and the vcpkg overlay triplets under
  `cmake/vcpkg/triplets/`. (`CMAKE_EXPORT_COMPILE_COMMANDS` comes from the top-level
  `CMakeLists.txt` itself, not the presets.)
- `debug` / `release` — just `CMAKE_BUILD_TYPE`.
- `windows-msvc`, `windows-llvm`, `linux-gcc`, `linux-llvm`, `linux-gcc-arm64`,
  `linux-llvm-arm64`, `macos-llvm` — one per platform/compiler pair. Each sets
  `VCPKG_TARGET_TRIPLET`, chainloads that platform's toolchain
  file (see [above](#the-compiler-is-pinned-not-path-found)) via `VCPKG_CHAINLOAD_TOOLCHAIN_FILE`,
  and is gated by a `condition` on `hostSystemName` so only the presets for the machine you're on
  even appear. `AC3FORGE_BUILD_GUI` is `ON` for the two Windows ones and `OFF` for the rest — see
  [Verified configuration](#verified-configuration).

Fourteen concrete `config-<platform>[-debug]` presets inherit `[ release|debug, <platform>, core ]`,
each with a matching `build-<platform>[-debug]` and `test-<platform>[-debug]` preset:

| Platform | Compiler | Configure preset | Build preset | Test preset |
|---|---|---|---|---|
| Windows | MSVC | `config-windows-msvc[-debug]` | `build-windows-msvc[-debug]` | `test-windows-msvc[-debug]` |
| Windows | clang-cl | `config-windows-llvm[-debug]` | `build-windows-llvm[-debug]` | `test-windows-llvm[-debug]` |
| Linux | GCC 15 | `config-linux-gcc[-debug]` | `build-linux-gcc[-debug]` | `test-linux-gcc[-debug]` |
| Linux | Clang 21 | `config-linux-llvm[-debug]` | `build-linux-llvm[-debug]` | `test-linux-llvm[-debug]` |
| Linux (arm64) | GCC 15 | `config-linux-gcc-arm64[-debug]` | `build-linux-gcc-arm64[-debug]` | `test-linux-gcc-arm64[-debug]` |
| Linux (arm64) | Clang 21 | `config-linux-llvm-arm64[-debug]` | `build-linux-llvm-arm64[-debug]` | `test-linux-llvm-arm64[-debug]` |
| macOS | Homebrew LLVM | `config-macos-llvm[-debug]` | `build-macos-llvm[-debug]` | `test-macos-llvm[-debug]` |

The two `-arm64` rows are the same `linux.gcc.toolchain.cmake`/`linux.llvm.toolchain.cmake` files as
their x64 counterparts — only the vcpkg triplet (`arm64-linux-gcc`/`arm64-linux-llvm`) differs; the
toolchain files already resolve aarch64 vs x86_64 from `VCPKG_TARGET_ARCHITECTURE`. See
[Raspberry Pi](platforms/raspberry-pi.md), the primary hardware this target is validated against.

There is a fifteenth configure/build/test trio, Debug-only and not part of the table above
because it isn't a platform/compiler pair but an instrumented variant of `linux-llvm`:
`config-linux-llvm-asan-ubsan` / `build-linux-llvm-asan-ubsan` / `test-linux-llvm-asan-ubsan`,
which inherits `linux-llvm` plus a `sanitize-asan-ubsan` fragment setting
`AC3FORGE_SANITIZERS=address,undefined` (see `cmake/Sanitizers.cmake`; MSVC is rejected outright,
so this only exists for GCC/Clang). See [Verified configuration](#verified-configuration) for what CI says
about all seventeen. There are also eight `ci-<platform>` `workflowPresets` (Release except for the
asan-ubsan one, which is Debug-only) that chain configure→build→test in one
`cmake --workflow --preset ci-windows-msvc` call; that is exactly what CI itself runs.

There is a sixteenth trio, `config-linux-gcc-coverage` / `build-linux-gcc-coverage` /
`test-linux-gcc-coverage`, the same shape as the asan-ubsan one: an instrumented variant of
`linux-gcc`, Debug-only, not a platform/compiler pair. It inherits a `coverage` fragment setting
`AC3FORGE_ENABLE_COVERAGE=ON` (see `cmake/Coverage.cmake`, GCC/Clang's `--coverage` gcov
instrumentation; other compilers just warn and skip it) plus `AC3FORGE_BUILD_CLI=OFF` and
`AC3FORGE_BUILD_EXAMPLES=OFF` — `ac3cli` and the twenty `examples/` executables also link the
now-instrumented `ac3::forge`, and turning them off avoids having to wire `ac3::coverage` into
them too just to resolve its gcov runtime symbols at link time for targets nobody is measuring
coverage of anyway. `.github/workflows/ci.yml`'s `coverage` job runs `gcovr` over `src/forge/*`
after `ctest` and gates on line/branch percentage — see that job's own comment for the current
thresholds and why they sit below the measured baseline.

There is a seventeenth trio, `config-linux-llvm-shared` / `build-linux-llvm-shared` /
`test-linux-llvm-shared`, same shape again: an instrumented variant of `linux-llvm`, Debug-only.
It inherits a `shared-libs` fragment setting `BUILD_SHARED_LIBS=ON`, proving `ac3::forge_shared`/
`matroska::matroska_shared` actually work — not just that the CMake topology configures, but that
every in-tree consumer (`ac3cli`, `ac3gui`, `ac3tests`, `examples/`) links and runs against the
real `.so`. `.github/workflows/_build.yml` runs it as an extra step inside the existing
`linux-llvm` leg rather than a new matrix entry, the same shape as the ASan/UBSan pass.

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
and the compiler selection on a platform preset (`windows-msvc` here; `windows-llvm`,
`linux-gcc`, `linux-llvm`, `linux-gcc-arm64`, `linux-llvm-arm64` and `macos-llvm` are the
others — see `CMakePresets.json`). Missing either from `dev`'s
`inherits` list still configures, but silently: CMake
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
| `AC3FORGE_BUILD_GUI` | `ON` on the two Windows presets, `OFF` on Linux and macOS | Build `ac3gui`. Requires Qt. Off by default outside Windows because a Qt kit isn't assumed present there — see [Building on Linux](#building-on-linux). |
| `AC3FORGE_BUILD_TESTS` | `ON` | Build the Catch2 suite. Requires Catch2. |
| `AC3FORGE_FETCH_CATCH2` | `ON` | When no local Catch2 3 is found (vcpkg, a distro package, an explicit `CMAKE_PREFIX_PATH`), fetch and build v3.15.3 from source via `FetchContent` instead of failing. Turn off to insist on a package-manager copy — see `tests/CMakeLists.txt`. Irrelevant when `AC3FORGE_BUILD_TESTS` is off. |
| `AC3FORGE_BUILD_EXAMPLES` | `ON` | Build `examples/`, and register them as tests. |
| `AC3FORGE_BUILD_MATROSKA` | `ON` | Build `matroska::matroska` (`src/matroska`), the standalone Matroska container writer. `OFF` only makes sense with the CLI, GUI, tests and examples all `OFF` too — they link it unconditionally, and configure fails with a clear message otherwise (see the root `CMakeLists.txt` guard). |
| `AC3FORGE_BUILD_MP4` | `ON` | Build `mp4::mp4` (`src/mp4`), the standalone MP4/ISOBMFF container writer. Same all-off constraint as `AC3FORGE_BUILD_MATROSKA`. |
| `AC3FORGE_BUILD_MPEGTS` | `ON` | Build `mpegts::mpegts` (`src/mpegts`), the standalone MPEG-TS container writer. Same all-off constraint as `AC3FORGE_BUILD_MATROSKA`. |
| `AC3FORGE_BUILD_ADM` | `OFF` | Build `ac3adm::ac3adm` (`src/ac3adm`), the standalone BW64/RF64 + ADM parser — see [ADM / BW64 reading](library/adm.md). Off by default, unlike every other library component: it vendors libbw64/libadm via `FetchContent`, and libadm needs several Boost header libraries, resolved separately via `-DVCPKG_MANIFEST_FEATURES=adm` (`vcpkg.json`'s `adm` feature) — turning this `ON` without also selecting that feature fails with a clear configure-time message rather than a bare "Boost not found". |
| `AC3FORGE_WITH_ALSA` | `AUTO` | Linux only. `AUTO` builds the ALSA audio backend when libasound's headers are present; `ON` requires them; `OFF` never builds it. Takes precedence over `AC3FORGE_WITH_PIPEWIRE` when both are found — see [Linux audio](#linux-audio). |
| `AC3FORGE_WITH_PIPEWIRE` | `AUTO` | Linux only. `AUTO` builds the PipeWire audio backend when libpipewire-0.3's headers are present *and* ALSA was not selected; `ON` requires the headers (independently of ALSA); `OFF` never builds it. See [Linux audio](#linux-audio). |
| `AC3FORGE_SANITIZERS` | empty | Comma-separated `-fsanitize=` value, e.g. `address,undefined` — see `cmake/Sanitizers.cmake`. Empty is a no-op; GCC/Clang only, MSVC is a configure error. Set via the `-asan-ubsan` preset above rather than by hand. |
| `AC3FORGE_ENABLE_COVERAGE` | `OFF` | `--coverage` gcov instrumentation over every target it's linked into — see `cmake/Coverage.cmake`. Off is a no-op; GCC/Clang only, other compilers get a configure-time warning and no instrumentation. Set via the `-coverage` preset above rather than by hand. |
| `AC3FORGE_ENABLE_TRACY` | `OFF` | Tracy profiler instrumentation (`ac3::tracy` — see `cmake/Tracy.cmake`). Needs vcpkg's `profiling` manifest feature (`-DVCPKG_MANIFEST_FEATURES=profiling`), which supplies Tracy itself; off is a no-op. |
| `AC3FORGE_BUILD_FUZZERS` | `OFF` | Build the libFuzzer harnesses under `fuzz/`. Clang only (GCC and MSVC ship no libFuzzer); use `fuzz/run.sh` rather than this option directly — it configures a dedicated `build/fuzz` with the right compiler. See [`fuzz/README.md`](https://github.com/iainchesworthlabs/ac3forge/blob/main/fuzz/README.md). |

Building the library and CLI alone, with neither Qt nor vcpkg involved:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_BUILD_GUI=OFF -DAC3FORGE_BUILD_TESTS=OFF
```

The vcpkg toolchain file is still referenced by the preset, so `VCPKG_ROOT` must still point
at a checkout — it simply has nothing to install. To build with no vcpkg at all, configure
without the preset and pass the generator and build type by hand.

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

On Linux those three are implemented over **ALSA** when its headers are present, and over
**PipeWire** when they are not but PipeWire's are — see [Why ALSA still comes
first](#why-alsa-still-comes-first) for the precedence between them. Installing ALSA's headers
is one package:

```bash
sudo apt-get install libasound2-dev
```

(`alsa-lib-devel` on Fedora, `alsa-lib` on Arch.) Nothing else is needed: no PulseAudio
development headers, no vcpkg port, no runtime daemon. Recording from the ALSA `default` device
goes through PipeWire or PulseAudio automatically wherever one is running, because that is what
those install themselves as.

PipeWire's headers are the alternative, for a machine without ALSA's:

```bash
sudo apt-get install libpipewire-0.3-dev
```

(`pipewire-devel` on Fedora.) Located via pkg-config rather than a CMake find module, since
PipeWire ships none of its own.

Both dependencies are **optional and detected**. Configure reports which one it picked:

```
-- ALSA 1.2.15.3: live capture, monitor playback and IEC 61937 passthrough enabled
--   Audio backend  : alsa
```

```
-- PipeWire 1.6.2: live capture and monitor playback enabled; IEC 61937 passthrough negotiates
   for real but needs a compressed codec enabled on the target node by the session manager first
   - see src/audio/src/backend/pipewire/passthrough.cpp
--   Audio backend  : pipewire
```

Without either set of headers, configure succeeds anyway and says so; the build then selects
`src/audio/src/backend/posix/`, whose entry points all return `kNoBackend`, and `ac3cli` marks
the affected commands `UNAVAILABLE HERE` in its usage rather than pretending they exist. Pass
`-DAC3FORGE_WITH_ALSA=ON` and/or `-DAC3FORGE_WITH_PIPEWIRE=ON` to turn a missing set of headers
into a configure error instead, which is what a packaging build wants.

#### Why ALSA still comes first

Capture and monitor playback are ordinary PCM and both backends do them for real — native
`pw_stream` on PipeWire's side, not its ALSA-compatibility shim. Passthrough is the
discriminator, and it is what the whole project is for: sending an AC-3 or E-AC-3 elementary
stream down an S/PDIF or HDMI link so the receiver decodes it.

That is not a "format" on Linux the way it is on Windows. A bitstream is opened as plain 16-bit
stereo PCM, and what tells the receiver these bytes are Dolby Digital rather than music is the
IEC 60958 **channel status** travelling beside them — specifically the non-audio bit, AES0
bit 1. ALSA is where that bit is expressed (as arguments on the device name,
`iec958:CARD=PCH,DEV=0,AES0=0x06,…`), and it works the moment compatible hardware exists — no
extra configuration.

PipeWire has its own real, current, native mechanism for the same thing —
`SPA_MEDIA_SUBTYPE_iec958`, `spa_format_audio_iec958_build()`, `PW_STREAM_FLAG_EXCLUSIVE` — not
aspirational API surface; `src/audio/src/backend/pipewire/passthrough.cpp`'s own header comment cites a
real shipped client (Kodi's PipeWire passthrough support) that negotiates exactly this way. What
it does not have is ALSA's "just works": a PipeWire sink only offers a compressed codec once its
`iec958Codecs` control has been populated by the session manager (a WirePlumber ALSA-monitor
rule, or a one-off `pw-cli` call) — configuration this library has no portable way to perform on
a caller's behalf. On a stock desktop where nobody has touched that setting, every PipeWire sink
honestly has no compressed codec enabled, even though the exact same hardware is reachable
directly through ALSA underneath the very PipeWire daemon that's running.

That is why ALSA keeps first precedence in `src/audio/CMakeLists.txt` whenever both are found,
rather than PipeWire winning by default for being the modern norm on most current desktops:
preferring it unconditionally would silently regress `ac3cli outputs`/`play` on exactly the
common case where nobody has configured `iec958Codecs`. The explicit escape hatch for a machine
where PipeWire's compressed codecs genuinely are configured is
`-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON`, the same shape `-DAC3FORGE_WITH_ALSA=OFF`
alone already has today.

#### What has and has not been verified

**ALSA.** Verified on WSL2 Ubuntu 26.04 with gcc 15.2 and clang 21.1, in every configuration:
with libasound present and absent, and under ASan+UBSan with leak detection. The full suite
passes in all of them. The device-independent halves of the backend — device-name construction,
channel-status derivation, the negotiation, the render and capture threads, start/stop, and the
error mapping — were additionally driven end to end against ALSA's software `null` PCM.

**What real hardware has and has not shown.** ALSA device enumeration has since run on real
hardware — a Raspberry Pi 4B enumerated and correctly classified its `vc4hdmi` HDMI outputs
(see [Raspberry Pi](platforms/raspberry-pi.md#verified-configuration)). What remains unverified
on Linux is bitstreaming to a real receiver: WSL2 has no sound devices and no kernel sound
modules, so nothing here has been played to an actual S/PDIF or HDMI output, and no AV receiver
has been asked to lock onto the result. Whether a given output accepts a bitstream is
per-device anyway — `ac3cli outputs` probes each one and says.

**PipeWire.** Verified on the same WSL2 Ubuntu 26.04 host with libpipewire-0.3 1.6.2, gcc 15.2
and clang 21.1, with `-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON` forcing the
selection (WSL2's image has both sets of headers installed, and ALSA wins by default — see
above). The full suite passes on both compilers. There is no PipeWire session running in that
environment at all (no `pipewire`/`wireplumber` daemon, confirmed by `pw_context_connect()`
failing fast rather than hanging), so `enumerate_devices()`/`enumerate_render_devices()` were
exercised against that "no session" path — returning an empty list rather than erroring, the
PipeWire-side equivalent of ALSA's "no sound card" case — not against a real graph with real
nodes to enumerate, negotiate with, or bitstream to. Nothing here has connected to a live
PipeWire session, requested a compressed format from a real node, or been played to a real
receiver.

## Qt

Qt is a **prebuilt dependency and never a vcpkg port**. Building Qt from source through vcpkg
takes hours and produces a kit that is harder to debug against than the official one.

`cmake/FindQt6.cmake` widens `CMAKE_PREFIX_PATH` to the usual prebuilt-kit install roots — on
Windows `C:/Qt`, `%USERPROFILE%/Qt`, `D:/Qt`; on Linux and macOS `~/Qt`, `/opt/Qt`, Homebrew and
MacPorts prefixes, and so on — newest kit first, and then defers to Qt's own config package. Every
Linux and macOS preset still forces `AC3FORGE_BUILD_GUI=OFF` by default — pass
`-DAC3FORGE_BUILD_GUI=ON` explicitly on a machine that has Qt 6.5+, which is verified to work on
Linux both locally (see [GUI on Linux](#gui-on-linux) above) and in CI, which installs a Qt6 kit
and turns the flag on for the four Linux build legs (x64 and arm64, GCC and Clang) plus
`macos-llvm` (Homebrew's `qt` formula — see [macOS](platforms/macos.md#gui-on-macos)). See
[Verified configuration](#verified-configuration). If your kit is somewhere else, say so explicitly and it
wins over the search — the project's own `-DAC3FORGE_QT_ROOT=` (or the `AC3FORGE_QT_ROOT`,
`QT_ROOT_DIR` or `QTDIR` environment variables) is the preferred way:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_QT_ROOT=D:/Qt/6.8.3/msvc2022_64
```

Plain `-DCMAKE_PREFIX_PATH=...` or `-DQt6_DIR=...` also work, and take priority over everything
`FindQt6.cmake` does. If you do not want the GUI, `-DAC3FORGE_BUILD_GUI=OFF` removes the
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
(`pack-windows-llvm`, `pack-linux-gcc`, `pack-linux-llvm`, `pack-linux-gcc-arm64`,
`pack-linux-llvm-arm64`, `pack-macos-llvm`). A pack preset reuses whatever the matching build
tree was configured with — on a non-Windows preset that includes the GUI only if you opted in
(`-DAC3FORGE_BUILD_GUI=ON`, which is exactly what CI's Linux and macOS packaging legs pass — see
[GUI on Linux](#gui-on-linux) and [macOS](platforms/macos.md#gui-on-macos)). Beyond `windows-msvc`'s continuous per-push packaging, the
`release_package` legs have run for real on tagged releases, and `pack-linux-gcc-arm64` has
additionally been run by hand on a real Raspberry Pi 4B with the resulting `.deb` inspected —
see [Raspberry Pi](platforms/raspberry-pi.md#verified-configuration). Packages land in
`packages/` at the repository root.
`cmake --build --preset build-windows-msvc --target pack-ac3forge` runs the same thing from
inside an IDE's target list instead of the command line.

`ac3cli`/`ac3gui` and `ac3::forge`/`matroska::matroska` are both installed and packaged, as
independent CPack components (`runtime`, plus `library`/`libruntime` for the codec itself) - a
second `ac3forge-dev-*` archive alongside the usual end-user one, for a third party consuming the
codec via `find_package(ac3forge)` rather than running it as a program; on Linux, `library`/
`libruntime` also become real `libFOO`/`libFOO-dev`-style DEB/RPM packages rather than only an
archive. See
[Using ac3::forge](library/index.md) for the CMake side and
[docs/releasing.md](releasing.md#what-gets-published) for exactly what ships where.
`ac3::audio` (live capture/monitor/passthrough, `src/audio/`) stays link-only and unpackaged -
a CLI/GUI implementation detail, not part of either component.

CI packages the `windows-msvc` leg on every push and uploads the result as a workflow
artifact (`.github/workflows/_build.yml`), so the packaging path is exercised continuously
rather than only when someone remembers to run it locally.

A tag-triggered release workflow (`.github/workflows/release.yml`) builds, signs, attests and
publishes packages for the four `release_package` legs — `windows-msvc`, `linux-gcc`,
`linux-gcc-arm64` and `macos-llvm`, one canonical build per OS/architecture — whenever a
`vX.Y.Z` tag is pushed; a packaging failure on any of them blocks the release like any other
required leg. The release carries GPG signing (optional, off until a key is provisioned),
keyless Sigstore/OIDC build provenance, an SPDX SBOM, and a GitHub Release; four beta releases
(v0.2.0-beta.1 through v0.5.0-beta.1) have shipped through this path for real. See
[docs/releasing.md](releasing.md) for the full process, including how to provision the GPG key.

There is also a staged, unpublished vcpkg port at `packaging/vcpkg-port/ac3forge/`
(`portfile.cmake`, `usage`, its own `vcpkg.json`), pending submission to the curated
`microsoft/vcpkg` registry. Today it exposes one feature, `matroska` (on by default); a consumer
uses the installed package via `find_package(ac3forge)` exactly as
[Using ac3::forge](library/index.md) documents. The per-release submission flow is in
[docs/releasing.md](releasing.md#vcpkg-port).

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

Result: configure, build and `ctest` all clean — the full suite passes, windows-msvc and
windows-llvm both.

The Linux instructions were run on:

| | |
|---|---|
| OS | Ubuntu 26.04 (WSL2) |
| Compilers | GCC 15.2.0 and Clang 21.1.x, both tried |
| CMake | ≥ 3.28, Ninja generator |
| Qt | 6.10.2, apt-packaged (`qt6-base-dev`, `qt6-declarative-dev`) |
| ALSA | `libasound2-dev`, both present and as the no-ALSA fallback — see [Linux audio](#linux-audio) |
| PipeWire | `libpipewire-0.3-dev` 1.6.2, present and forced selected (`-DAC3FORGE_WITH_ALSA=OFF -DAC3FORGE_WITH_PIPEWIRE=ON`) — see [Linux audio](#linux-audio) |
| vcpkg | checkout at `/opt/vcpkg` |

Result: configure, build and `ctest` all clean on both compilers, GUI and ALSA both included.
The base suite is `ac3tests` and `ac3perf`'s Catch2 cases plus one ctest entry per example
program; `AC3FORGE_WITH_ALSA`'s `tests/platform/alsa/` adds 14 entries (or, on a build that
selected pipewire/ instead, `tests/platform/pipewire/` adds 5), and the GUI's Qt Quick
Test harness (`ac3gui_qmltests`, `apps/gui/tests/CMakeLists.txt`) adds one more — unlike every
other GUI-related target, that one harness *does* register its own `ctest` entry, gated on both
`AC3FORGE_BUILD_GUI` and `AC3FORGE_BUILD_TESTS`. A Linux build with neither ALSA nor the GUI
runs the base suite; with the GUI on and ALSA off it matches Windows exactly. `ac3gui --smoke`
also runs clean headless (`QT_QPA_PLATFORM=offscreen`), encoding real audio and instantiating
real QML channel meters. See [Linux audio](#linux-audio) for what the ALSA verification did,
and did not (real hardware), prove.

linux-gcc, linux-llvm, linux-gcc-arm64, linux-llvm-arm64, linux-llvm-asan-ubsan, macos-llvm,
static-analysis (clang-tidy), coverage (gcovr over `src/forge`, via `config-linux-gcc-coverage`),
adm-validate (the opt-in ADM module) and ffmpeg-validate all run on every push, as does
build-android (the Shield app's debug APK) — the four Linux build legs install the same
Qt6/ALSA packages and build/smoke-test the GUI too. ffmpeg-validate is a
separate, CLI-only linux-llvm build that runs FFmpeg as an independent oracle against the full
layout/tool/metadata option space (see
[CONTRIBUTING.md's Oracles section](https://github.com/iainchesworthlabs/ac3forge/blob/main/CONTRIBUTING.md#oracles)) — a different question from the
[gold-reference gate](#gold-reference-correctness-gate) below, which every leg runs against one
fixed sample to check output *quality*; ffmpeg-validate instead checks that every option
combination produces a *structurally correct* stream at all, plus a numeric fidelity floor for
the Annex E tool combinations the one fixed gold-reference sample does not itself exercise. No
leg remains experimental.

The coverage job's own gate — 81.3% line / 72.0% branch measured on a real GitHub Actions run,
80%/70% required — uses the same GCC 15 pin as the other Linux legs; see the coverage job's own
comment in `ci.yml` for the thresholds and why they sit below the measured baseline.

No macOS host exists for this project, so `config-macos-llvm`/`config-macos-llvm-debug` are only
ever exercised by CI (`macos-latest`, Apple Silicon) — never locally. That CI leg is green:
configure, build and `ctest` all clean, using a Homebrew-installed LLVM
(`cmake/toolchains/macos.llvm.toolchain.cmake` prefers it over Apple's bundled clang) rather than
a version-pinned one — Homebrew's core `llvm` formula has no versioned sibling the way
apt.llvm.org or the official Windows installer do, so unlike the other LLVM legs this one tracks
whatever Homebrew currently ships. The gold-reference correctness gate
(`scripts/verify-gold-reference.sh` — see [Gold-reference correctness gate](#gold-reference-correctness-gate)
below) also passes: real SNR numbers from that CI run were 61.81/61.82 dB on macOS, against
67.84/67.82 dB on Linux and Windows for the same material - a real but modest cross-compiler
floating-point difference, comfortably clear of the 30 dB gate. `macos-llvm` now builds the GUI
too (Homebrew's `qt` formula — see [GUI on macOS](platforms/macos.md#gui-on-macos)), which adds
`ac3gui_qmltests` to that same suite the same way it does on Linux: confirmed on a real run, 582
ctest entries total, 100% passing, `ac3gui_qmltests` itself in 39.74s (56.81s for the whole
suite) — the first time that number has existed for macOS at all, so there is no prior baseline
to compare it against the way the ~15s Windows number has one. Getting there needed two real
fixes, not just turning the option on: `QSG_RENDER_LOOP=basic` (`apps/gui/tests/CMakeLists.txt`,
`APPLE` only) for a Qt Quick threaded-render-loop deadlock that hung the suite outright before a
single test ran, and forcing the `Fusion` style in `qml_test_main.cpp` — matching what `main.cpp` already does —
for a second, narrower hang in a native `ComboBox` populated by real capture-device data once a
test entered live-session mode: the same native-style-under-offscreen fragility a comment in
`apps/gui/qml/Main.qml` already documents one earlier instance of, on Windows, in a different
control (a `Repeater`'s per-device `Button`, worked around there directly in QML rather than at
the style level). See `apps/gui/tests/CMakeLists.txt` and `qml_test_main.cpp` for the full detail
on both macOS fixes.

`src/audio/CMakeLists.txt` selects a real CoreAudio backend on macOS (`src/audio/src/backend/macos/`,
built on the Audio HAL — `AudioObjectID`/`AudioDeviceIOProc` — the same layer WASAPI and ALSA
occupy on their own platforms), not the no-backend stub it fell back to before. `AC3FORGE_BUILD_GUI`
still defaults off there (`macos-llvm` opts it on in CI the same way the Linux legs do — see
[GUI on macOS](platforms/macos.md#gui-on-macos)) — capture, monitor playback and IEC 61937
passthrough compile and link for real either way, and `ac3tests` exercises the backend's
device-free logic (format matching, sample conversion) directly. What CI cannot exercise is a
real device: the hosted runner enumerates
whatever HAL objects macOS itself reports and touches nothing beyond that, same as ALSA's own
"verified headless" story below — see [Linux audio](#linux-audio) for the general shape of what
that does and does not prove, and [macOS](platforms/macos.md) for the backend's own header
comments on where its research came from (three independent real-world CoreAudio passthrough
implementations, surveyed since no Mac is available to try it on directly).

The `-arm64` presets are exercised in CI on GitHub's hosted `ubuntu-24.04-arm` runner (real ARM
hardware, not QEMU) and, separately, on a real Raspberry Pi 4B — see
[Raspberry Pi](platforms/raspberry-pi.md#verified-configuration) for the on-device numbers, which
are tracked there rather than duplicated here since that page is the canonical source for
Pi-specific hardware findings (real ALSA/HDMI device names, resolved compiler versions on Raspberry
Pi OS, and so on).

## Gold-reference correctness gate

`scripts/verify-gold-reference.sh` (invoked in CI on every leg except linux-llvm-asan-ubsan,
which stays diagnostic-only) is the first real implementation of the project's original
validation-pyramid design (now [docs/verification.md](verification.md)), which had never been
wired into CI before this: encode a fixed, checked-in 5.1 WAV
(`tests/golden/audio/reference_51.wav`, synthesized once by `tools/gen_gold_reference_wav.py` —
independent of this codec's own encoder/decoder, not bootstrapped from one of our own encodes),
strict-decode the result with FFmpeg (`-err_detect crccheck+bitstream+buffer+explode`, checked
via stderr content rather than exit code — confirmed locally that ffmpeg's own process exits 0
even on a CRC mismatch), decode it again with ac3cli's own decoder, and assert the two decodes
agree via `scripts/compare_wav.py`'s delay-compensated SNR (stdlib-only Python, no numpy — every
CI-hosted runner already ships Python 3, so this needs no new provisioning). The gate is
perceptual/SNR-based rather than a bit-exact bitstream comparison deliberately: nothing in this
project verifies that Homebrew LLVM, GCC and MSVC round the codec's floating-point
pipeline identically, and the real numbers above show they in fact do not, by a small but
measurable margin.

This is a narrow, cross-platform *quality* check — one sample, two codecs, every OS — not a
conformance sweep. `tools/check_matrix_coverage.py`, `tools/quality_race.py`'s `ci` mode and the
rest of the `ffmpeg-validate` CI leg (Linux-only, see [Verified configuration](#verified-configuration)
above) cover the *correctness* question instead: does every layout, every Annex E tool token and
every metadata option actually produce a structurally valid, spec-conformant stream, across the
full option space this gate does not attempt.
