# Linux

ac3forge builds and is tested on Linux today, on both GCC and Clang, CLI and GUI alike. This
page covers what is specific to Linux; for the full preset reference, options list and
troubleshooting, see [Building from source](../building.md).

## Toolchains

Built and tested with **GCC 15.2** and **Clang 21.1** on **Ubuntu 26.04 (WSL2)** — the versions
CI pins. The pin is a CI reproducibility choice, not a hard floor of the code:
`cmake/toolchains/linux.{gcc,llvm}.toolchain.cmake` already `find_program` a fallback list
(`gcc-15, gcc, gcc-14, gcc-13` / `clang-21, clang, clang-20, clang-19`), so an older distro
compiler is picked up automatically — the [Raspberry Pi
validation](raspberry-pi.md#verified-configuration) built and passed the full suite with
GCC 14.2 and Clang 19.1.7.

`config-linux-gcc` and `config-linux-llvm` (each with a `-debug` variant) find the compiler by
that same known-name `find_program` walk, the same way the Windows presets pin MSVC/clang-cl,
rather than trusting whatever is first on `PATH`.

## Audio backend: ALSA

On Linux, live capture (`ac3cli devices`/`record`), monitor playback (`ac3cli monitor`) and IEC
61937 bitstream passthrough (`ac3cli outputs`/`play`) are implemented over **ALSA**. Everything
else is file I/O and needs no audio stack at all — `ac3cli spdif` in particular reaches an AV
receiver by writing a WAV, on any machine.

The dependency is one optional, detected package:

```bash
sudo apt-get install libasound2-dev
```

(`alsa-lib-devel` on Fedora, `alsa-lib` on Arch.) Nothing else is needed — no PipeWire or
PulseAudio development headers, no vcpkg port, no runtime daemon. Without the headers, configure
succeeds anyway and the build selects a no-backend fallback whose entry points return
`kNoBackend`; `ac3cli` marks the affected commands `UNAVAILABLE HERE` in its usage rather than
pretending they exist. `AC3FORGE_WITH_ALSA` defaults to `AUTO` (build it if found); set it to
`ON` to make a missing libasound a configure error instead, which is what a packaging build
wants.

### Why ALSA and not PipeWire

Capture and monitor playback are ordinary PCM, which every Linux audio API can do — passthrough
is the discriminator, and it's what the whole project is for. On Linux, a bitstream isn't a
distinct "format" the way it is on Windows: it's opened as plain 16-bit stereo PCM, and what
tells the receiver these bytes are Dolby Digital rather than music is the IEC 60958 **channel
status** travelling beside them (the non-audio bit, AES0 bit 1). ALSA is the layer where that
bit is expressed, as arguments on the device name (`iec958:CARD=PCH,DEV=0,AES0=0x06,…`), and the
layer PulseAudio's and PipeWire's own passthrough modes are built on — both end in the same ALSA
call, made by a daemon instead of by us. The cost is coexistence: opening a device directly
takes it exclusively, so a running sound server has to have released it first — the same bargain
WASAPI exclusive mode strikes on Windows. The full reasoning, including why a PipeWire backend
would buy politeness rather than capability, is in
[Why ALSA and not PipeWire](../building.md#why-alsa-and-not-pipewire).

### What has and has not been verified

!!! warning "No Linux audio has been tried against real hardware"
    The ALSA backend was verified **headless only** — with libasound present and absent, and
    under ASan+UBSan with leak detection, on WSL2 Ubuntu 26.04. The full test suite passes in
    every configuration tried, and the device-independent halves of the backend (device-name
    construction, channel-status derivation, negotiation, the render/capture threads,
    start/stop, error mapping) were additionally driven end to end against ALSA's software
    `null` PCM device. But WSL2 has no sound devices and no kernel sound modules at all, so
    nothing has ever been bitstreamed to a real S/PDIF or HDMI output on Linux, and no AV
    receiver has been asked to lock onto the result. This is a real, current gap, not a minor
    caveat — whether a given output accepts a bitstream is per-device anyway, and `ac3cli
    outputs` probes each one and reports what it finds.

## GUI: opt-in, not on by default

Unlike Windows, where `AC3FORGE_BUILD_GUI` defaults **ON**, both Linux presets default it
**OFF** — not because the GUI can't be built on Linux (`cmake/FindQt6.cmake` resolves a Linux Qt
kit the same way it resolves a Windows one, and `ac3gui` builds clean and passes its headless
`--smoke` run under both Linux presets in CI), but because a Qt kit isn't assumed to be present
on every Linux machine that builds this project. Opt in explicitly once Qt 6.5+ is installed:

```bash
cmake --preset config-linux-gcc-debug -DAC3FORGE_BUILD_GUI=ON
```

On Debian/Ubuntu:

```bash
sudo apt install qt6-base-dev qt6-base-dev-tools qt6-declarative-dev qt6-declarative-dev-tools
```

Other distros need the equivalent Qt6 base + declarative (QML/Quick) packages (Fedora:
`qt6-qtbase-devel` / `qt6-qtdeclarative-devel`). See [GUI on Linux](../building.md#gui-on-linux)
for the CMake warnings you'll see about unlinked QML plugins (harmless — a property of how
distro-packaged Qt6 is built, not a missing dependency).

## Building

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset config-linux-gcc-debug
cmake --build --preset build-linux-gcc-debug
ctest --preset test-linux-gcc-debug
```

Substitute `linux-llvm` for `linux-gcc` to build with Clang instead. Add
`-DAC3FORGE_BUILD_GUI=ON` to either configure line to build `ac3gui` too, once Qt is installed
(see [GUI](#gui-opt-in-not-on-by-default) above). `VCPKG_ROOT` must point at a vcpkg checkout —
it supplies Catch2, plus Boost and Tracy only if you opt into the `adm`/`profiling` features
(see [building.md](../building.md)), same as on Windows; this project's own convention keeps it
at `/opt/vcpkg`, but any path works.

## Packaging

```bash
cpack --preset pack-linux-gcc
```

(or `pack-linux-llvm` for the Clang build; append `-arm64` for the arm64 presets). Produces a
plain tarball, plus DEB/RPM on top when the corresponding packaging tool is on `PATH`. A local
run packages whatever the tree was configured with — the GUI only if you opted in. Tagged
releases run `pack-linux-gcc` and `pack-linux-gcc-arm64` for real, and CI configures those legs
with `-DAC3FORGE_BUILD_GUI=ON`, so released Linux packages include `ac3gui`; a real arm64 `.deb`
has also been produced and inspected on Raspberry Pi hardware (see
[Raspberry Pi](raspberry-pi.md#verified-configuration)). See
[Packaging](../building.md#packaging).

## CI

`linux-gcc` and `linux-llvm` both run on every push and are **required**; both install a Qt6 kit
and build and smoke-test `ac3gui` in addition to the CLI. A third leg, `linux-llvm-asan-ubsan`
(AddressSanitizer + UndefinedBehaviorSanitizer), is also required but stays **CLI-only on
purpose**, to keep a Qt kit out of the sanitizer leg's install time.

Two more legs, `linux-gcc-arm64` and `linux-llvm-arm64`, run the same matrix on real ARM hardware
(GitHub's `ubuntu-24.04-arm` hosted runner, not QEMU emulation) — see
[Raspberry Pi](raspberry-pi.md), which is the hardware this arch target is validated
against.

The ALSA backend adds 14 tests of its own (`tests/platform/alsa/`) on top of the base suite: a
Linux build with the GUI on and `libasound2-dev` absent runs the same suite as Windows, and ALSA
adds those 14. `ctest --preset test-linux-gcc-debug` (or whichever preset matches your build)
runs the full suite. See [Verified configuration](../building.md#verified-configuration)
for the full CI matrix.
