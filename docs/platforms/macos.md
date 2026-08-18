# macOS (Apple Silicon, Homebrew LLVM)

!!! note "Verified in CI only — no Mac host is available to this project"
    There is no macOS host available to this project locally; everything on this page has been
    exercised exclusively by `macos-llvm`, a required CI leg on GitHub's `macos-latest` (Apple
    Silicon) runners, configuring the `config-macos-llvm` / `config-macos-llvm-debug` preset
    pair. It is no longer experimental: its first-ever run surfaced one genuine, fully-understood
    issue (Homebrew's unpinned `llvm` formula flagging Catch2's `__COUNTER__` usage under
    `-Wc2y-extensions` — see `cmake/CompilerWarnings.cmake`), fixed in one commit, followed by two
    consecutive clean runs. The `continue-on-error` escape hatch has since been removed
    (see [`.github/workflows/_build.yml`](https://github.com/iainchesworthlabs/ac3forge/blob/develop/.github/workflows/_build.yml)),
    so a `macos-llvm` failure blocks like every other required leg now.

## Toolchain

Homebrew-installed LLVM (`cmake/toolchains/macos.llvm.toolchain.cmake` prefers it over Apple's
bundled clang), on the `arm64-macos-llvm` vcpkg triplet. Unlike the Linux/Windows LLVM legs, this
one isn't pinned to an exact version: Homebrew's core `llvm` formula has no versioned sibling to
pin against the way `apt.llvm.org` or the official Windows installer do, so CI installs and
reports whatever Homebrew currently ships rather than asserting a specific one.

## Audio backend: CoreAudio

`src/audio/CMakeLists.txt` selects a real CoreAudio backend on macOS, `src/audio/src/platform/macos/`
— capture, monitor playback and IEC 61937 passthrough are built on the Audio HAL
(`AudioObjectID`/`AudioDeviceIOProc`), the same layer WASAPI and ALSA occupy on their own
platforms, rather than the no-backend stub that used to fall back to here. Its passthrough
mechanism is genuinely different from both: CoreAudio has no per-open bitstream flag the way
WASAPI's exclusive-mode subformat or ALSA's channel-status device name are, so bitstreaming means
taking hog mode on a digital output and retuning its *physical* stream format
(`kAudioStreamPropertyPhysicalFormat`) to `kAudioFormat60958AC3` for AC-3 — see
`src/audio/src/platform/macos/passthrough.cpp`'s own header for the full mechanism, cross-checked
against three independent real-world implementations of the same thing (MythTV, mpv, VLC) while
writing it, since there was no Mac available locally to try it on directly. For E-AC-3, the same
walk additionally probes a stream's available physical formats for `kAudioFormatEnhancedAC3`:
Apple's own documentation confirms Dolby Digital Plus/Atmos HDMI passthrough exists on Apple
Silicon Macs without documenting the HAL mechanism behind it, so where a driver doesn't publish
that format (older hardware, a non-HDMI output, an Intel Mac) the backend simply reports E-AC-3
passthrough unavailable rather than claiming it everywhere — see `passthrough.cpp`'s own "AC-3
and E-AC-3" section.

The backend is CI-verified only: the parts that need no live device — enumeration on a machine
with none, format matching, sample conversion — run under `ac3tests` on the hosted runner, same
as everywhere else without real hardware, but no real Mac has ever run this code against an
actual digital output, and no receiver has been asked to lock onto its output.

## Building

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake --preset config-macos-llvm-debug
cmake --build --preset build-macos-llvm-debug
ctest --preset test-macos-llvm-debug
```

Drop `-debug` from all three preset names for a Release build; the `ci-macos-llvm` workflow
preset chains the same three steps in one command. Homebrew's `llvm` formula must be installed
(CI runs `brew install llvm`), and `VCPKG_ROOT` must point at a vcpkg checkout — it supplies
Catch2, plus Boost and Tracy only if you opt into the `adm`/`profiling` features (see
[building.md](../building.md)). `AC3FORGE_BUILD_GUI` defaults **OFF** here, as on Linux — see
[GUI on macOS](#gui-on-macos) below to opt in.

## GUI on macOS

`config-macos-llvm` defaults `AC3FORGE_BUILD_GUI` to `OFF` for the same reason the Linux presets
do (see [GUI on Linux](../building.md#gui-on-linux)): a Qt kit isn't assumed present on every
Mac, not because `ac3gui` cannot be built here. `cmake/FindQt6.cmake` already searches Homebrew's
Apple Silicon prefixes (`/opt/homebrew/opt/qt`, `/opt/homebrew/opt/qt6`), and
`src/gui/CMakeLists.txt`'s `APPLE` branch — `MACOSX_BUNDLE`, the `.icns` bundle icon, and
`qt_generate_deploy_qml_app_script()` for packaging — was written for this from the start; it
was simply never exercised until the `macos-llvm` CI leg turned the option on. Opt in explicitly
once Qt is installed:

```bash
brew install qt
cmake --preset config-macos-llvm-debug -DAC3FORGE_BUILD_GUI=ON
```

Homebrew's `qt` formula is the umbrella Qt6 package — one install pulls in QtDeclarative/QtQuick
and their build-time tooling (`qmlcachegen`) alongside QtCore/QtGui, unlike apt's split
`qt6-base-dev`/`qt6-declarative-dev` packages. The built app is a bundle,
`build/config-macos-llvm/bin/ac3gui.app`; `ac3gui --smoke` (the same headless check the other
platforms run — see [Verified configuration](../building.md#verified-configuration)) lives at
`ac3gui.app/Contents/MacOS/ac3gui`, not directly under `bin/`, because `MACOSX_BUNDLE` relocates
the executable there — the same property Windows' `WIN32_EXECUTABLE` sits beside but which only
takes effect on `APPLE`.

## Packaging

```bash
cpack --preset pack-macos-llvm
```

Produces a DragNDrop image on top of a plain ZIP when the packaging tool is found, the same way
NSIS is on Windows and DEB/RPM are on Linux. `macos-llvm` is one of the four `release_package`
legs (alongside `windows-msvc`, `linux-gcc` and `linux-gcc-arm64`) that package on a real tagged
release (`release.yml`, `do_package: true`). That path has been exercised for real: four beta
releases, v0.2.0-beta.1 through v0.5.0-beta.1, have shipped through the tag-triggered workflow,
macOS packages included. `cmake/Packaging.cmake` needed no change for `ac3gui` to join that
`.dmg`: which targets end up in a package is decided entirely by which `install()` rules ran, and
`ac3gui`'s already runs whenever `AC3FORGE_BUILD_GUI` is `ON` — the DragNDrop generator itself is
unconditional on `APPLE`, GUI or not. No stable (non-beta) release has been tagged yet. See
[Packaging](../building.md#packaging).

## CI: what has and has not been verified

Build, `ctest` (see [Verified configuration](../building.md#verified-configuration) for how the
suite's composition differs from Windows/Linux, including whether `ac3gui_qmltests` registers)
and the [gold-reference correctness gate](../building.md#gold-reference-correctness-gate) all
pass on real GitHub Actions runners — not a simulation or a local guess. Real SNR numbers from
the CI run that first proved the gate on macOS: 61.81/61.82 dB, against 67.84/67.82 dB on Linux
and Windows for the same material — a real but modest cross-compiler floating-point difference
(Homebrew LLVM's libm vs. glibc's/MSVC's), comfortably clear of the gate's 30 dB floor. That
number predates the GUI leg and is unaffected by it — the codec paths it measures don't change
when `ac3gui` is built alongside them.

---

If you get a Mac, that's still useful information for this project — running these instructions
on real local hardware, or actually launching `ac3gui.app` and using it (CI's `--smoke` run
proves it starts, loads its QML and drives a real encode headlessly, not that the interactive
experience is right), would be genuinely new. Consider filing an issue with what you found.
