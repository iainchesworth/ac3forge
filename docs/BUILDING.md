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

Only Windows/MSVC is verified — see [Portability](../README.md#portability) for what CI says
about the rest. There are also five `ci-<platform>` `workflowPresets` (Release only, no `-debug`
variant) that chain configure→build→test in one `cmake --workflow --preset ci-windows-msvc`
call; that is exactly what CI itself runs.

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

`cmake/FindQt6.cmake` widens `CMAKE_PREFIX_PATH` to the usual prebuilt-kit install roots — on
Windows `C:/Qt`, `%USERPROFILE%/Qt`, `D:/Qt`; on Linux and macOS `~/Qt`, `/opt/Qt`, Homebrew and
MacPorts prefixes, and so on — newest kit first, and then defers to Qt's own config package. The
Linux/macOS search exists for future use: every Linux and macOS preset currently forces
`AC3FORGE_BUILD_GUI=OFF` regardless (see [Portability](../README.md#portability)), so today this
only matters on Windows. If your kit is somewhere else, say so explicitly and it wins over the
search — the project's own `-DAC3FORGE_QT_ROOT=` (or the `AC3FORGE_QT_ROOT`, `QT_ROOT_DIR` or
`QTDIR` environment variables) is the preferred way:

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

Result: configure, build and `ctest` all clean, 182/182 tests passing.

No other compiler, OS or Qt version has been tried *here*. `src/lib/CMakeLists.txt` selects stub
implementations of WASAPI capture and passthrough when `WIN32` is false, so a non-Windows build
is intended to be possible, and presets exist for it (see [Presets](#presets)), but only CI has
ever run one — and as of this writing every leg but windows-msvc fails there too, on strict
warnings rather than a codec bug. See [Portability](../README.md#portability) for the current
per-leg status. Assume non-Windows broken until a leg goes green.
