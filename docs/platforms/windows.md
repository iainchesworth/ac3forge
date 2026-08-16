# Windows

ac3forge is built and tested on Windows today — both toolchains, CLI and GUI alike, are
required, green CI legs. This page covers what is specific to Windows; for the full preset
reference, options list and troubleshooting, see [Building from source](../building.md).

## Toolchains

Built and tested with **MSVC 14.51** and **clang-cl 21.1** on **Windows 11**, via Visual Studio
2026 (MSVC) or clang-cl.

Every Windows preset chainloads a toolchain file that locates `cl.exe`/`clang-cl.exe` and
`link.exe` itself (via `vswhere` and `vcvarsall.bat` if a Developer PowerShell hasn't already
set one up), so configuring works from an ordinary shell — no need to launch a Developer
Command Prompt first. clang-cl is MSVC-ABI compatible, so it links the same vcpkg packages and
the same prebuilt Qt kit as the MSVC build. See
[The compiler is pinned, not PATH-found](../building.md#the-compiler-is-pinned-not-path-found)
for the mechanics.

## Audio backend: WASAPI

On Windows, the three features that touch sound hardware are all implemented over **WASAPI**:

- **`ac3::capture`** — live input/loopback capture through a lock-free SPSC ring.
- **`ac3::sinks::PassthroughSink`** — exclusive-mode/direct bitstream output, for both AC-3 and
  E-AC-3 burst framing (IEC 61937).
- **`ac3::sinks::MonitorSink`** — shared-mode PCM playback: a non-bitstreamed preview/monitor
  path that decodes what is being encoded and plays it back on an ordinary output.

These three are not equally verified against real hardware, and the project's own documentation
is deliberately explicit about the difference.

!!! note "MonitorSink is confirmed against real hardware"
    `ac3cli monitor` / `ac3cli live --monitor` have actually played decoded AC-3 and E-AC-3
    (including an Atmos stream's 5.1 bed) through a real Realtek output in real time, and a live
    microphone capture→encode→monitor session has run end to end. Building this path against
    real hardware surfaced two genuine bugs that neither unit tests nor silent/synthetic input
    would have caught — a fixed submit-readiness threshold smaller than an actual chunk, which
    let the ring buffer silently perform a partial write while reporting failure, and the live
    pipeline's Atmos metering step writing past the end of a buffer sized for the object count
    rather than the bed's fixed six channels. Both are fixed; see
    `src/audio/src/platform/windows/monitor.cpp` and `run_live` in `src/cli/main.cpp`.

!!! warning "Exclusive-mode passthrough bitstreaming has never been confirmed against a real receiver"
    No S/PDIF or HDMI endpoint behind an actual AV receiver has been available during
    development. `IsFormatSupported` correctly answers no everywhere it has been tried, for
    both `KSDATAFORMAT_SUBTYPE_IEC61937_DOLBY_DIGITAL` and `..._DOLBY_DIGITAL_PLUS`, and neither
    descriptor has been accepted by a real device. What *is* verified: the exclusive-mode path
    itself works (a Realtek endpoint accepts an exclusive PCM format), the AC-3 bursts are
    byte-exact against FFmpeg's `spdif` muxer, and the E-AC-3 burst framing (data type 0x15, the
    24576-byte/4x-carrier-rate burst, multi-syncframe accumulation, `Pd` in bytes not bits) is
    independently verified against both FFmpeg's `spdif_header_eac3` and Microsoft's own
    "Representing Formats for IEC 61937 Transmissions" documentation, plus round-trip and
    real-audio unit tests.
    A receiver has been confirmed to lock onto AC-3, but only via a different code path (playing
    the bursts as a PCM16 WAV through a passthrough output, not through `PassthroughSink`
    itself); the same trick now exists for E-AC-3 (`ac3cli spdif`/`monitor`/`live`, branching on
    bsid) but has not itself been tried against a receiver either.

## Qt (GUI only)

`ac3gui` needs a **prebuilt Qt 6.5+ kit**, discovered by `cmake/FindQt6.cmake` — never from
vcpkg. `AC3FORGE_BUILD_GUI` defaults **ON** on Windows. `FindQt6.cmake` widens
`CMAKE_PREFIX_PATH` to the usual install roots (`C:/Qt`, `%USERPROFILE%/Qt`, `D:/Qt`); to point
at a specific kit explicitly:

```bash
cmake --preset config-windows-msvc-debug -DAC3FORGE_QT_ROOT=D:/Qt/6.8.3/msvc2022_64
```

See [Qt](../building.md#qt) for the full discovery order and options.

## Building

```bash
cmake --preset config-windows-msvc-debug
cmake --build --preset build-windows-msvc-debug
ctest --preset test-windows-msvc-debug
```

Drop `-debug` for a Release build. Swap `msvc` for `llvm` throughout to build with clang-cl
instead:

```bash
cmake --preset config-windows-llvm-debug
cmake --build --preset build-windows-llvm-debug
ctest --preset test-windows-llvm-debug
```

`VCPKG_ROOT` must point at a vcpkg checkout (it supplies Catch2 — plus Boost and Tracy only if
you opt into the `adm`/`profiling` features; see [building.md](../building.md)). See
[Presets](../building.md#presets) for the full preset table and the `ci-windows-msvc` /
`ci-windows-llvm` workflow presets that chain all three steps.

## Packaging

```bash
cpack --preset pack-windows-msvc
```

Produces a ZIP, plus an NSIS installer on top if `makensis` is on `PATH`. `pack-windows-llvm` is
the clang-cl equivalent. `windows-msvc` is the only leg packaged continuously — CI packages it on
every push and uploads the result as a workflow artifact, a standing smoke test of the packaging
path; tagged releases package all four `release_package` legs (Windows, Linux x64/arm64, macOS).
See [Packaging](../building.md#packaging).

## CI

Both Windows legs — `windows-msvc` and `windows-llvm` — run on every push and are **required**,
alongside every other required leg — see the full matrix in [Verified
configuration](../building.md#verified-configuration), including the Linux and macOS legs and
the coverage/FFmpeg-validation legs. CI runs the CLI and GUI on both Windows legs.
