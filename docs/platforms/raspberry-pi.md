# Raspberry Pi

ac3forge runs on Raspberry Pi as a plain **aarch64 Debian/Ubuntu Linux** target, not through any
Pi-specific code. This page covers what's specific to that arm64 target and its HDMI output; for the
general Linux picture (toolchains, the ALSA backend, the GUI, packaging), see
[Linux](linux.md) and [Building from source](../building.md) - everything there applies here
unchanged, just with `-arm64` presets.

## Why there's no Raspberry Pi-specific code

The project's platform tree (`src/audio/src/platform/{windows,alsa,posix,android}/`, selected by
`cmake/*/CMakeLists.txt`, never by `#ifdef` - `scripts/check-platform-macros.ps1` enforces this in CI)
branches on **operating system**, not architecture or device. A Raspberry Pi running Raspberry Pi OS
hits exactly the same `if(LINUX)` branch, the same ALSA backend, and the same
[HDMI/S-PDIF passthrough device-naming logic](linux.md#why-alsa-and-not-pipewire) that any x86_64
Debian box does. Enabling this target was almost entirely CMake/vcpkg/CI plumbing - see the two new
`arm64-linux-{gcc,llvm}` vcpkg overlay triplets (`cmake/vcpkg/triplets/`) and the
`config-linux-{gcc,llvm}-arm64[-debug]` presets they back, mirroring the existing `arm64-macos-llvm`
triplet Apple Silicon already uses.

## Supported hardware

| Model | Status |
|---|---|
| Raspberry Pi 4 Model B | Supported. Validated hardware - see [Verified configuration](#verified-configuration) below. |
| Raspberry Pi 5 | Expected to work identically (same BCM27xx SoC family, same `vc4`/`v3d` HDMI/GPU driver stack as the Pi 4) but **not yet validated on real Pi 5 hardware** - don't take this as tested until this page says otherwise. |
| Raspberry Pi 3 | **Not a supported target.** It would resolve through the exact same untested arm64 build path, but its Cortex-A53 CPU is materially weaker than the Pi 4/5's Cortex-A72/A76, and `tests/performance/test_performance.cpp`'s `ac3perf` suite gates on a hard real-time encode budget - there's a real risk some layouts simply don't make it in time on that CPU class. Building it is possible; it just isn't validated or promised to keep up in real time. |

A 64-bit OS is required (`aarch64`, not `armhf`/`armv7`) - the project defines no 32-bit ARM triplet,
and none is planned.

## Requirements

Same as [Linux](linux.md#toolchains) generally, but Raspberry Pi OS's own package archive (Debian
13 "Trixie" as of this writing) doesn't necessarily carry the exact GCC 15 / Clang 21 versions CI
pins. `cmake/toolchains/linux.{gcc,llvm}.toolchain.cmake` already `find_program` a fallback list
(`gcc-15, gcc, gcc-14, gcc-13` / `clang-21, clang, clang-20, clang-19`), so an older distro compiler
is picked up automatically - the version pin is a CI reproducibility choice, not a hard requirement
of the code. See [Verified configuration](#verified-configuration) for what was actually resolved on
real hardware.

```bash
sudo apt install build-essential cmake ninja-build pkg-config git \
    libasound2-dev \
    qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools
```

Building with the LLVM preset instead of GCC additionally needs `clang-<N>` and, less obviously,
**`clang-tools-<N>`** (owns `clang-scan-deps-<N>`, which CMake's Ninja generator shells out to for
every C++20/23 translation unit's module-dependency scan, not just files using `import`/`export`):

```bash
sudo apt install clang-19 lld-19 clang-tools-19
```

Without `clang-tools-<N>`, configure fails confusingly - every `find_package`-driven try_compile
(Qt6, Threads, ALSA) fails too, since they all hit the same missing `clang-scan-deps` binary, which
reads like broken dependency detection rather than a missing package. Substitute Trixie's actual
`clang`/`lld`/`clang-tools` version for `19` if a future Raspberry Pi OS release ships something
newer.

## Building

Identical to [Building on Linux](../building.md#building-on-linux), with `-arm64` appended to the
preset name:

```bash
export VCPKG_ROOT=/opt/vcpkg
cmake --preset config-linux-gcc-arm64-debug -DAC3FORGE_BUILD_GUI=ON
cmake --build --preset build-linux-gcc-arm64-debug
ctest --preset test-linux-gcc-arm64-debug
```

Substitute `linux-llvm-arm64` for `linux-gcc-arm64` to build with Clang instead - same tradeoff as on
x64. `VCPKG_ROOT` only ever supplies Catch2, exactly as on every other platform.

## HDMI output

The Pi's only audio-capable HDMI path is its VideoCore HDMI ALSA card, normally exposed under a name
like `vc4-hdmi` (`bcm2835` on older firmware/kernel combinations). ac3forge doesn't special-case
this name - `src/audio/src/platform/alsa/device_names.hpp`'s `classify_digital_output()` already
recognizes any ALSA PCM whose name contains `hdmi` and builds the IEC 60958 channel-status device
string generically. Find the real name on a given Pi with:

```bash
aplay -L
build/config-linux-gcc-arm64-debug/bin/ac3cli outputs
```

See [Verified configuration](#verified-configuration) for the exact device name found during
hardware validation.

## Packaging

```bash
cpack --preset pack-linux-gcc-arm64
```

Produces an arm64-labeled `.tar.gz`, plus a `.deb` (`Architecture: arm64`, auto-detected by CPack's
DEB generator via `dpkg --print-architecture` on the build host - nothing here hardcodes an
architecture) and `.rpm` on top when the corresponding tool is on `PATH`. See
[Packaging](../building.md#packaging) for the general shape, and
[docs/releasing.md](../releasing.md#what-gets-published) for what a tagged release actually
publishes.

## Verified configuration

Run for real, over SSH, on:

| | |
|---|---|
| Board | Raspberry Pi 4 Model B rev 1.1 (2GB) |
| OS | Raspberry Pi OS 13 "Trixie" (Debian 13.6 base), kernel 6.18.34+rpt-rpi-v8 |
| Compilers | GCC 14.2.0 and Clang 19.1.7 (Trixie's apt archive; Trixie has no `gcc-15`/`clang-21` yet - the toolchain files' fallback `find_program` list picked these up automatically, no configuration needed) |
| CMake | 3.31.6, Ninja 1.12.1 |
| Qt | 6.8.2, apt-packaged (`qt6-base-dev`, `qt6-declarative-dev`) |
| ALSA | `libasound2-dev` 1.2.14 |
| vcpkg | checkout at `/opt/vcpkg` |

Both `config-linux-gcc-arm64[-debug]` and `config-linux-llvm-arm64[-debug]` configure, build and
`ctest` all clean: **440/440 tests passing on both compilers**, including the `Performance` label
(`ac3perf`'s hard real-time encode gate) - both the Atmos/JOC and plain 5.1 encoders stay comfortably
inside their real-time budget on this hardware, and the Qt Quick Test GUI harness
(`ac3gui_qmltests`) passes headless. `ac3gui --smoke` (`QT_QPA_PLATFORM=offscreen`) also runs clean,
encoding real audio and instantiating real QML channel meters. A full Release build (`config-linux-gcc-arm64`)
and `cpack --preset pack-linux-gcc-arm64` were also run for real, producing a `.deb` with
`Architecture: arm64` and a correctly auto-resolved Qt runtime `Depends:` list (`libqt6core6t64`,
`libqt6gui6`, `libqt6qml6`, `libqt6quick6`, `libqt6quickcontrols2-6`, plus `libasound2t64`) - this Pi's
Qt6 came from apt, confirming for the first time on real hardware that `cmake/Packaging.cmake`'s "a
system Qt6 install resolves fine through `dpkg-shlibdeps` alone" note holds in practice, not just in
theory.

Builds are slow on this hardware and RAM-constrained (2GB, 4 cores) - a full `-j2` build (CLI, GUI,
tests, all examples) takes roughly 17-22 minutes depending on Debug/Release and compiler; `-j4`
wasn't tried, to leave headroom against OOM.

**Two real, compiler/distro-specific issues were found and fixed by this validation, not
hypothetical:**

1. GCC 14.2.0 at `-O2`/`-O3` (Release only - not seen at Debug) emits a false-positive
   `-Wnull-dereference` inside libstdc++'s own `<streambuf>`/`<bits/stl_construct.h>` internals,
   promoted to a hard error by this project's `-Werror` policy. Not seen on CI's pinned GCC 15.
   Fixed in `cmake/CompilerWarnings.cmake`, scoped to `GCC < 15` only, so the CI-pinned toolchain
   (both x64 and arm64) is unaffected.
2. CMake's Ninja generator shells out to `clang-scan-deps` for every C++20/23 translation unit's
   module-dependency scan (not just files using `import`/`export`) - without it, every
   `find_package`-driven `try_compile` (Qt6, Threads, ALSA) fails the same way, which reads as
   broken dependency detection rather than a missing package. `clang-scan-deps-<N>` lives in
   `clang-tools-<N>`, a package neither `clang-<N>` nor `llvm-<N>` pull in. Fixed by adding
   `clang-tools-${LLVM_VERSION}` to `.github/toolchain/03-llvm-toolchain.sh`'s package list -
   unconditionally, since every Linux LLVM leg configures the same way regardless of which distro's
   package split happens to be exercising the gap.

**Real ALSA/HDMI device names found**, confirming the generic device-naming logic resolves
correctly against actual Pi hardware:

```
hdmi:CARD=vc4hdmi0,DEV=0   # HDMI port 0
hdmi:CARD=vc4hdmi1,DEV=0   # HDMI port 1 (the 4B has two micro-HDMI outputs)
hw:CARD=Headphones,DEV=0   # bcm2835 analogue out, not HDMI
```

**Not yet verified: live HDMI passthrough to a real receiver.** This Pi's HDMI ports both report
`disconnected` at the DRM level (nothing plugged in during this validation pass), so `ac3cli
outputs` correctly reports no active render endpoints - the ALSA card names above exist and are
correctly classified as HDMI, but no bitstream has actually been sent to a receiver yet, and no
lock-on has been confirmed. This is the same category of gap [Linux](linux.md#what-has-and-has-not-been-verified)
already documents for x64 - real hardware, still no downstream receiver in the loop. Revisit once a
TV/AVR is connected.
