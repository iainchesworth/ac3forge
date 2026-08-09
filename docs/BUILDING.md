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

Building the library and CLI alone, with neither Qt nor vcpkg involved:

```bash
cmake --preset debug -DAC3FORGE_BUILD_GUI=OFF -DAC3FORGE_BUILD_TESTS=OFF
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
cmake --preset debug -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64
```

`-DQt6_DIR=...` also works. If you do not want the GUI, `-DAC3FORGE_BUILD_GUI=OFF` removes the
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

No other compiler, OS or Qt version has been tried. `src/lib/CMakeLists.txt` selects stub
implementations of WASAPI capture and passthrough when `WIN32` is false, so a non-Windows
build is intended to be possible, but it has not been attempted and should be assumed broken
until someone tries it.
