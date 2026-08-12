# macOS (Apple Silicon, Homebrew LLVM)

!!! note "Verified in CI only — no Mac host is available to this project"
    There is no macOS host available to this project locally; everything on this page has been
    exercised exclusively by `macos-llvm`, a required CI leg on GitHub's `macos-latest` (Apple
    Silicon) runners, configuring the `config-macos-llvm` / `config-macos-llvm-debug` preset
    pair. It is no longer experimental: its first-ever run surfaced one genuine, fully-understood
    issue (Homebrew's unpinned `llvm` formula flagging Catch2's `__COUNTER__` usage under
    `-Wc2y-extensions` — see `cmake/CompilerWarnings.cmake`), fixed in one commit, followed by two
    consecutive clean runs. The `continue-on-error` escape hatch has since been removed
    (see [`.github/workflows/_build.yml`](https://github.com/iainchesworth/ac3forge/blob/develop/.github/workflows/_build.yml)),
    so a `macos-llvm` failure blocks like every other required leg now.

## Toolchain

Homebrew-installed LLVM (`cmake/toolchains/macos.llvm.toolchain.cmake` prefers it over Apple's
bundled clang), on the `arm64-macos-llvm` vcpkg triplet. Unlike the Linux/Windows LLVM legs, this
one isn't pinned to an exact version: Homebrew's core `llvm` formula has no versioned sibling to
pin against the way `apt.llvm.org` or the official Windows installer do, so CI installs and
reports whatever Homebrew currently ships rather than asserting a specific one.

## What has and has not been verified

Build, `ctest` (344/344 tests — no GUI leg on macOS yet, so no `ac3gui_qml_tests` entry; see
[Verified configuration](../building.md#verified-configuration) for how that count differs from
Windows/Linux) and the [gold-reference correctness
gate](../building.md#gold-reference-correctness-gate) all pass on real GitHub Actions runners —
not a simulation or a local guess. Real SNR numbers from that CI run: 61.81/61.82 dB on macOS,
against 67.84/67.82 dB on Linux and Windows for the same material — a real but modest
cross-compiler floating-point difference (Homebrew LLVM's libm vs. glibc's/MSVC's), comfortably
clear of the gate's 30 dB floor.

`src/lib/CMakeLists.txt` selects the same no-backend audio implementation on macOS that a Linux
machine without `libasound2-dev` gets: capture, monitor playback and IEC 61937 passthrough all
report themselves unavailable rather than failing to link. `AC3FORGE_BUILD_GUI` also defaults
off here (no CI leg builds `ac3gui` on macOS), so the GUI and the three audio-hardware commands
remain untested on macOS specifically — same as everywhere else without real hardware or a Qt
kit. See [Building from source](../building.md#verified-configuration) for the full picture
across every platform.

## Packaging

`pack-macos-llvm` exists, and `macos-llvm` is one of the three `release_package` legs
(alongside `windows-msvc` and `linux-gcc`) that actually package on a real tagged release
(`release.yml`, `do_package: true`) — DragNDrop on top of a plain ZIP if the packaging tool is
found on the runner, the same way NSIS is on Windows and DEB/RPM are on Linux. No release has
shipped yet — the API isn't stable and no `vX.Y.Z` tag has been pushed — so that path is wired
up but has not been exercised for real. Only `windows-msvc`'s continuous per-push packaging
smoke test (`ci.yml`) has actually run and been inspected end to end; see
[Packaging](../building.md#packaging).

---

If you get a Mac, that's still useful information for this project — running these instructions
on real local hardware, or trying the GUI on macOS for the first time, would be genuinely new.
Consider filing an issue with what you found.
