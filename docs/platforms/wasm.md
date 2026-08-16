# WebAssembly (browser decode demo)

WASM support is not `ac3cli` ported to a browser — it is a small, decode-only demo app,
**`platform/wasm/`**, that compiles `ac3::forge`'s AC-3/E-AC-3 decoder to WebAssembly and runs it
client-side in a static HTML page: load a real elementary stream, hear the decoded bed play through
the Web Audio API, and watch real per-channel energy on a speaker-ring visualization. It exists to
prove the decoder runs correctly outside a native process, and to give the documentation site a live
demo (see [Live decode demo](../wasm-demo.md)) — not to be a general-purpose in-browser tool. This
page covers what is specific to WASM; for the core library and the desktop platforms, see
[Building from source](../building.md) and the other pages in this section.

Decode-only, deliberately: WASM-encode is a separate, much larger undertaking (real-time MDCT/bit-
allocation/JOC matrix work in a browser thread) and isn't attempted here.

## Build and run

An [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) on `PATH`
(`source <emsdk>/emsdk_env.sh` / `emsdk_env.bat`/`.ps1`), then:

```bash
cmake --preset config-wasm-emscripten
cmake --build build/config-wasm-emscripten
cd build/config-wasm-emscripten/bin/wasm_decode_demo
python3 -m http.server 8000   # ES modules and fetch() need http(s), not file://
```

Open `http://localhost:8000/`.

## What's reused, what's new

`ac3::forge` (`src/lib/`) — the codec, `FrameDecoder`/`Eac3Decoder`, elementary-stream scanning — is
fully platform-independent and is linked into the demo **unmodified**, the same way `platform/wasm/CMakeLists.txt`
links it as any other consumer would: `add_executable` + `target_link_libraries(... ac3::forge ...)`,
no fork, no `#ifdef`. Unlike `platform/android/`, this doesn't need a separate build system reached
from the other direction — WASM is a plain CMake cross-compile, so `platform/wasm/` is a normal
`add_subdirectory()` from the root `CMakeLists.txt`, gated on `EMSCRIPTEN` (set by
`cmake/toolchains/wasm.emscripten.toolchain.cmake`) rather than an `AC3FORGE_BUILD_*` option.
`ac3::audio` (`src/audio/`) gains **no** WASM backend — there is no live-capture/passthrough
equivalent to add; a browser gets audio playback from the Web Audio API in JavaScript instead, and
`src/audio` is skipped from the configure entirely under `EMSCRIPTEN` (it hard-fails otherwise, for
having no browser platform directory — see `src/audio/CMakeLists.txt`).

Everything else — `decoder_bindings.cpp` (the Embind wrapper), `index.html`/`demo.js` (the page,
Web Audio playback, the Canvas visualization ported from `src/gui/qml/SoundfieldView.qml`) — is new
and lives entirely under `platform/wasm/`, outside anything the desktop tools build from.

## Toolchain

No vcpkg. Every other platform preset in `CMakePresets.json` chainloads through vcpkg for
consistency, but `ac3::forge`'s decode path has zero third-party dependencies (`vcpkg.json`'s own
description says so), so `config-wasm-emscripten`'s toolchain file goes straight to Emscripten's own
`Emscripten.cmake` — see that toolchain file's own header for why going through vcpkg's community
`wasm32-emscripten` triplet would be pure cost for nothing this preset needs.

Verified against **Emscripten 6.0.6**. No version is pinned in the toolchain file itself (unlike the
Android NDK's explicit pin) — there is no CMake-side equivalent of `local.properties`' `sdk.dir` to
pin against yet; whatever `$EMSDK` resolves to is what gets used.

## Release / CI

The demo builds alongside the desktop packages rather than only ever being hand-built locally:
`.github/workflows/_build.yml`'s `build-wasm` job configures and builds it on every push, the same
continuous-smoke-test role `build-android`'s always-on debug APK plays — proving the Emscripten
toolchain and every file it touches still build. Like `build-android`, it's its own job rather than
a `build` matrix entry: this leg has no ctest suite, no cpack package and no gold-reference gate, so
folding it into that matrix would mean threading new `if:` exclusions through most of that job's
steps for no benefit.

**The published demo is rebuilt fresh, not shipped from a committed copy.** `docs/assets/wasm-decode-demo/`
is committed to the repo as a working fallback (so a plain local `mkdocs build` — or this repo's own
PR-time docs check — still has something to embed without anyone needing Emscripten installed just
to preview docs), but `.github/workflows/docs.yml`'s `deploy` job (push to `main` only) installs
Emscripten, rebuilds `platform/wasm/` from source, and overwrites that directory *before* `mkdocs
gh-deploy` runs — so what actually reaches the live site always reflects current source, never a
possibly-stale commit. Both jobs share one Emscripten install step,
`.github/actions/setup-emscripten` (pinned to the same version this page's Toolchain section names),
so the two never drift onto different SDK versions.

`docs.yml`'s trigger `paths:` list includes `platform/wasm/**`, `CMakeLists.txt`,
`CMakePresets.json` and the WASM toolchain file specifically — without them, a source change there
would never trigger a redeploy at all, and the live demo would silently drift from what's in
`platform/wasm/`.

## What has and has not been verified

!!! note "Verified in a real browser"
    Both `cmake --preset config-wasm-emscripten` and the full desktop presets configure and build
    clean from the same source tree (confirmed after merging the changes into `develop`, not just in
    isolation). A real Chromium instance loading the built page — both standalone and embedded in the
    actual `mkdocs build --strict`-built docs site — genuinely decodes a bundled 8-second, 3-object
    Atmos-in-DD+ fixture (`E-AC-3, 48000 Hz, 6 ch (L, C, R, Ls, Rs, LFE), 8.0s`, matching what was
    encoded), plays real audio with `AudioContext.currentTime` genuinely advancing, and paints a
    visualization driven by real, time-varying per-channel RMS (confirmed non-degenerate per channel,
    including a genuinely-silent LFE since nothing was routed to it) that changes with playback
    position and responds to the seek bar.

!!! warning "Not yet verified"
    Built and tested on a Windows host only — the toolchain file itself makes no Windows-specific
    assumption, but a Linux/macOS Emscripten configure hasn't been run. No automated browser test
    runs this in CI (see [Live decode demo](../wasm-demo.md) for what CI does and does not do yet);
    every claim above is manual verification from one session, not a repeatable check. The
    visualization shows real decoded **bed energy**, not object positions — `ac3::forge` has no
    decode-side OAMD/JOC parser (see [Atmos & JOC](../concepts/atmos-joc.md)), so there is nothing
    object-level to verify here yet either.
