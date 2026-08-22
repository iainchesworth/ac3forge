# Quick start

Clone to first encode in under ten minutes. This page shows the shortest path; see
[building.md](building.md) for the full preset list, building without Qt, the Linux GUI opt-in,
and machine-local preset overrides.

## Prerequisites

| | Version | Notes |
|---|---|---|
| A compiler | MSVC (VS 2026), clang-cl 21, GCC 15, or Clang 21 | C++23. One preset per compiler. |
| CMake | ≥ 3.28 | |
| Ninja | any recent | The presets hard-code the Ninja generator. |
| [vcpkg](https://github.com/microsoft/vcpkg) | any recent, with `VCPKG_ROOT` set | Supplies Catch2 (plus Boost/Tracy only for the opt-in `adm`/`profiling` features — see [building.md](building.md)). |
| Qt | 6.5+ prebuilt | **GUI only.** Never from vcpkg — see [building.md](building.md). |

## Configure, build, test

=== "Windows"

    From any shell — a Developer PowerShell is not required, the presets chainload the MSVC
    environment themselves:

    ```bash
    cmake --preset config-windows-msvc-debug
    cmake --build --preset build-windows-msvc-debug
    ctest --preset test-windows-msvc-debug
    ```

    Swap `msvc` for `llvm` to build with clang-cl instead.

=== "Linux"

    ```bash
    export VCPKG_ROOT=/path/to/vcpkg
    cmake --preset config-linux-gcc-debug
    cmake --build --preset build-linux-gcc-debug
    ctest --preset test-linux-gcc-debug
    ```

    Swap `gcc` for `llvm` to build with Clang instead. The GUI is opt-in here via
    `-DAC3FORGE_BUILD_GUI=ON` rather than on by default.

That's it — the vcpkg toolchain file supplies Catch2, the presets pin the exact compiler, and
`ctest` runs the full suite (the Catch2 tests plus one ctest entry per example program). See
[building.md](building.md) for Release presets, the `ci-<platform>` workflow presets that chain
all three steps in one command, and what to do when no compiler is found.

## Write your first encoder

Two headers and about a dozen lines to encode a frame. This is excerpted from
[`examples/encode_ac3.cpp`](https://github.com/iainchesworthlabs/ac3forge/blob/main/examples/encode_ac3.cpp),
with the error handling elided:

```cpp
#include "ac3/core/tables.hpp"
#include "ac3/encoder/encoder.hpp"

// Heap-allocated: FrameEncoder carries several KB of MDCT scratch/history state.
auto encoder = std::make_unique<ac3::FrameEncoder>(ac3::EncoderConfig{
    .bitrate_kbps = 448,
    .acmod = ac3::Acmod::k3_2,  // L, C, R, SL, SR
    .lfe = true,
});

// Table 5.8 order, LFE last, exactly kSamplesPerFrame (1536) samples each.
std::vector<std::vector<float>> pcm(6, std::vector<float>(ac3::kSamplesPerFrame));
// encode_frame takes a span of spans, so the views must outlive the call.
const std::vector<std::span<const float>> views{pcm.begin(), pcm.end()};

for (int frame = 0; frame < 31; ++frame) {
    fill_with_audio(pcm, frame, 48000.0);
    if (const auto encoded = encoder->encode_frame(views)) {
        write(stream, *encoded);  // one complete syncframe
    }
}
```

## Where to go next

- [Library conventions](library/index.md) — the full API reference: `ac3::eac3::FrameEncoder`
  and `AccessUnitEncoder`, both decoders, `ac3::io::scan`, the spatial object layer, the Atmos
  encoder, and `matroska::mux`.
- [CLI reference](cli/index.md) — `ac3cli`, the twenty-six-command front end, for encoding and
  decoding from the shell without writing any C++.
- [GUI guide](gui/index.md) — `ac3gui`, the Qt Quick front end: file and live-capture encoding,
  a plan view for placing objects, and channel-level metering.
