# Android (NVIDIA Shield)

Android support is not `ac3cli`/`ac3gui` ported to a phone — it is a separate, small,
Shield-specific demo app, **Shield Atmos Demo** (`platform/android/`), that plays a real Atmos/JOC
stream out through the Shield's HDMI passthrough output to an AV receiver, with a controller or
remote moving one of a few objects around the room live. It exists to prove the encoder's object
audio audibly moves in 3D space on real consumer hardware, not to be a general-purpose encoding
tool. This page covers what is specific to Android; for the core library and the desktop
platforms, see [Building from source](../building.md) and the other pages in this section.

Distribution is **personal sideload only, via `adb install` — never the Play Store**. That is a
deliberate choice, not a placeholder: the app can be built with the [quarantine signer](#the-quarantine-signer-dependency)
enabled, and that build must never leave the user's own device (see below).

## What's reused, what's new

`ac3::forge` (`src/lib/`) — the codec, `AtmosEncoder`, IEC 61937 framing — is fully
platform-independent and is linked into the app **unmodified**, via a thin wrapper
`CMakeLists.txt` (`platform/android/app/src/main/cpp/CMakeLists.txt`) that `add_subdirectory()`s
the real repo root rather than duplicating its target definitions. `ac3::audio` (`src/audio/`)
gains a fourth platform backend, `src/audio/src/platform/android/`, alongside `windows`/`alsa`/
`posix`, selected by CMake's own `ANDROID` variable (set by the NDK toolchain file, a peer check
to the existing `WIN32`/`LINUX`/`APPLE` blocks in `src/audio/CMakeLists.txt`) — no `#ifdef`
anywhere, per the project's [platform-tree convention](../building.md).

Everything else — the Gradle app shell, the JNI bridge, the live encode loop, input handling, the
room visualization — is new and lives entirely under `platform/android/`, outside the CMake
project the desktop tools build from.

## Toolchain

**NDK r26.1.10909125**, pinned explicitly in `app/build.gradle.kts` rather than left to "whichever
NDK Gradle resolves" — a build failure should mean something changed, not that a different NDK got
silently picked. CMake **3.31.6** (the closest available match in the SDK manager's package
repository to the root project's declared `3.28...4.3` range; there is no 3.28.x package for
Android). `ANDROID_STL=c++_shared` — the app's `ac3::forge`/`ac3::audio` are static libraries
linked into one shared object (`ac3forge_jni.so`), and a static STL would duplicate global state
(locale, iostream init) if anything else in the process ever pulled in libc++ too.

`minSdk = 26` (Oreo) is a hard floor, not a target: `monitor.cpp` depends on AAudio outright, which
does not exist below API 26, and there is no fallback path. Real Shield TV hardware (2017 model
onward) ships well above this. Only `arm64-v8a` is built — every real Shield TV is arm64, and
building the other ABIs would only slow local iteration for targets that can never run the app.

## Audio backend: AAudio for monitor, JNI-bridged `AudioTrack` for passthrough

The original plan for this app was "native AAudio engine … using the existing IEC 61937
encapsulation" throughout. That premise turned out to be only half right, and is worth stating
explicitly so it is not rediscovered:

!!! warning "AAudio has no compressed/bitstream passthrough support at all"
    The NDK's AAudio API is PCM-only — confirmed against Oboe's own maintainers' guidance, not
    assumed. There is no AAudio call that hands a pre-framed IEC 61937 burst to an HDMI output and
    asks the receiver to decode it as Dolby Digital/Digital Plus. **Every real Android passthrough
    implementation** — Kodi's `AESinkAUDIOTRACK.cpp`, ExoPlayer's passthrough path — bypasses any
    native/codec API entirely and writes encoded, already-wrapped bursts into a **Java**
    `android.media.AudioTrack`, opened with a compressed encoding
    (`AudioFormat.ENCODING_E_AC3`/`ENCODING_IEC61937` — see below). This app does the same thing,
    just with a modern zero-copy JNI bridge instead of Kodi's older heap-array wrap.

    `AMediaCodec` (the NDK's native codec API) was considered and rejected for the same reason: it
    is a *decode/encode* pipeline API, with no mode for "inject an already-encoded bitstream
    verbatim onto passthrough output."

So the backend is genuinely split, unlike the other three:

- **`monitor.cpp`** — real AAudio (`AAudioStreamBuilder`, PCM float), for local preview. This is
  exactly what AAudio is good at, and the only place in this backend that uses it.
- **`passthrough.cpp`** — a JNI shim implementing `ac3::sinks::PassthroughSink`. `submit()` hands
  each burst to a Kotlin-owned `AudioTrack` via a small round-robin pool of buffers wrapped once
  with `env->NewDirectByteBuffer(...)` and promoted to a `GlobalRef` at startup — one `memcpy` into
  a native buffer per burst, zero further copies, no per-frame `NewDirectByteBuffer`/GC churn.
  Kotlin's `PassthroughBridge.kt` opens the `AudioTrack` with `ENCODING_IEC61937` (bursts arrive
  already framed — `ENCODING_E_AC3` would make Android re-wrap already-wrapped frames) and writes
  with `WRITE_BLOCKING`.
- **`capture.cpp`** — a no-op stub, mirroring the `posix` backend's "no backend" shape. This app
  has no microphone/loopback feature to serve.
- **`audio_backend.cpp`** — reports `capture.available=false` unconditionally; `passthrough` and
  `monitor` availability come from a one-time capability probe at startup
  (`AudioTrack.isDirectPlaybackSupported`/`isPcmSupported`, called separately per format since
  AC-3 and E-AC-3 need different carrier rates — see `carrier_rate()` in `android_support.hpp`),
  not from a static claim.

## Real-time performance: `RelWithDebInfo`, and a real MDCT bug it uncovered

AGP's default for the `debug` build type is `CMAKE_BUILD_TYPE=Debug` (`-O0`). That is fine for
`jni_entry.cpp`'s smoke tests but nowhere near real-time for `live_cursor.cpp`'s actual per-frame
work (`AtmosEncoder::encode_frame`'s MDCT/bit-allocation/JOC matrix, once every 32ms). Confirmed on
this Shield's Tegra X1: `-O0` took **~425ms/frame**, over 13x the budget — bursts arrived in huge
sparse gaps instead of a steady stream, which is exactly why the receiver's HDMI link stayed
flashing (video locked, audio never did). `app/build.gradle.kts`'s `debug` build type now overrides
this to `-DCMAKE_BUILD_TYPE=RelWithDebInfo`, which keeps the APK debuggable (`isDebuggable` stays
on, no separate release signing needed to `adb install`) while actually optimizing the native side.

That override alone only bought back ~1.6x — nowhere near enough. Profiling with
[Tracy](https://github.com/wolfpld/tracy) (`vcpkg`'s `profiling` manifest feature,
`AC3FORGE_ENABLE_TRACY`) traced the remaining gap to `mdct_forward_core`: it recomputed `std::cos()`
fresh, every iteration, inside an O(N²) loop, while the *inverse* transform right next to it already
used a precomputed table. Fixing the forward transform to do the same (`ForwardCosTable` in
`src/lib/src/core/mdct.cpp`) gave a further ~3.8x — this is a real library-level fix, verified
bit-exact against the full test suite, not an Android-specific workaround, and it benefits every
platform's Atmos encode path. With both fixes, the Shield holds an exact 32.0ms/frame cadence with
zero underruns. See [Performance trend](../performance-trend.md) for the CI regression gate this
bug prompted (`tests/performance/`'s hard real-time gate plus the `ac3bench` trend tracker).

## Objects: one interactive lead, two ambient, all on pre-planned orbits

`live_cursor.cpp` no longer holds a single object at a fixed point. Every object
(`kInteractiveObjects` + `kAmbientObjects`, currently 1 + 2) follows its own closed-form orbit —
`trajectory_position()`, a circle in the room's x/y plane centred on the room's *exact* middle
(`(0.5, 0.5)`, per `oamd.hpp` — also where the JOC/VBAP render implicitly assumes the listener
sits) with an independent, slower height bob — so every object's lap carries it both in front of
and behind the listening position, not confined to the front half of the room. Rate/phase/radius
differ per object (`kTrajectory`) so the three stay visually and audibly distinct.

The two ambient objects (a major third and a perfect fifth above the lead's A4, forming an A major
triad rather than an arbitrary tone set — `kToneHz`/`kToneGain`) are **never touched by input** —
they exist purely so the demo has more than one voice to show sound mixing/interaction between.
Only the lead is driven by [Input](#input-shield-controller-and-basic-remote-both) below.

## Input: Shield Controller and basic remote, both

`InputController.kt` supports both devices this app is meant to run under, detected at the event
level rather than requiring the user to pick a mode. Every input source **biases the lead object
off its own trajectory** rather than moving it outright (`NativeBridge.nativeDeflectSelectedObject`
→ `LiveCursorState::deflect_selected`, clamped to a bounding box around the trajectory) — release
the input and `LiveCursorState::advance()` decays that bias back toward zero every encode frame
(`kDeflectionDecayPerFrame`, an exp(-t/1.5s) time constant) whether or not any more input arrives,
so the object drifts back onto its planned course on its own rather than needing an explicit
"input stopped" signal from Kotlin.

- **Shield Controller** (`SOURCE_JOYSTICK`): both analog sticks, read in `onGenericMotionEvent`
  with a 0.15 deadzone, driving continuous deflection scaled by elapsed time via a
  `Choreographer.FrameCallback` ticker — held-stick input biases smoothly, not per-event-stepped.
  `L1`/`R1` add continuous height deflection independent of the D-pad's axis mode below.
- **D-pad** (present on both the Controller and the basic remote): now held-continuous rather than
  one-shot-per-press, unified into the same per-frame ticker as the analog sticks. Left/right
  always biases x; up/down biases **either** y (further into/out of the room) **or** z (height),
  depending on `axisMode` — the remote's only way to reach height at all, since it has no second
  stick or shoulder buttons.
- **Axis-mode toggle**: a **long press** of D-pad-center/Enter/A (`onKeyLongPress`) flips
  `axisMode` between X/Y and X/Z; a **short press** of the same key still cycles the selected
  object (`onKeyUp`, only fires the cycle if the long-press branch didn't already consume the
  press). `RoomView.kt` shows the current mode live, bottom-left of the room panels.

Either way, input is coalesced to **at most one JNI call per animation frame**, never per raw input
event — the native side (`live_cursor.cpp`'s `LiveCursorState`, mutex-protected) advances once per
encode frame, independent of how often Kotlin's ticker calls in.

## Visualization

`RoomView.kt` is a plain `View` (not a `SurfaceView` — a handful of `drawCircle`/`drawLine` calls
per frame doesn't justify a `SurfaceView`'s own render thread and `SurfaceHolder` lifecycle),
invalidated once per vsync via `Choreographer.postFrameCallback`. It draws two panels side by side,
both reading the same `NativeBridge.nativeGetObjectState()` snapshot the encode loop just built for
that frame: a top-down X/Y view and a side-elevation X/Z view, with the selected (lead) object
ringed. Verified against real device screenshots (`adb shell screencap`) that positions track both
controller/remote input and the encode loop's own state exactly (prior to today's trajectory/
deflection rewrite — see [What has and has not been verified](#what-has-and-has-not-been-verified)).

Three demoability additions on top of the raw positions: a white diamond marking the listener at
the room's exact centre (both panels' (0.5, 0.5) — see [Objects](#objects-one-interactive-lead-two-ambient-all-on-pre-planned-orbits)
above), a faint dashed guide circle on the top-down panel showing the lead object's planned orbit
so a viewer can see it pushed off course and springing back rather than just a dot moving with no
reference (`kTrajectoryGuideRadius`, duplicated from `live_cursor.cpp`'s `kTrajectory[0].radius` —
kept in sync by comment on both ends, not queried over JNI, since it's fixed at compile time on
both), and a live axis-mode readout at the bottom of the view.

## The quarantine signer dependency

!!! warning "Object motion is audible even without this — but not as reconstructable objects"
    `AtmosEncoder` pans every object into the transmitted 5.1 bed regardless of signing status
    (see `atmos.hpp`), so a plain, unsigned build of this app already produces audible movement
    on any decoder — panned across the fixed channel layout. What the quarantine signer adds is
    the *object* audio: a real Dolby-licensed decoder gates JOC object decode on a keyed HMAC
    over the EMDF protection field, which the clean-room encoder cannot itself produce (that key
    is extracted from Dolby's own binary — not derivable from the spec).

The signer lives in `src/quarantine/`, a gitignored, local-only, RE-derived (non-clean-room)
overlay — the same one `ac3cli` already optionally links, gated by the same
`AC3FORGE_QUARANTINE_SIGNER` CMake option and the same `FATAL_ERROR` guard if the option is on but
the overlay is absent. The app's own seam (`platform/android/app/src/main/cpp/shield_quarantine_hook.{hpp,cpp}`
+ `_stub.cpp`/`_enabled.cpp`) mirrors `src/cli/quarantine_hook*` exactly in shape, differing only in
call site: the CLI signs a whole batch after encoding; this app signs **per frame**, immediately
after `encode_frame()` and before IEC 61937 wrapping, since it streams live rather than writing a
file. No `#ifdef` at the call site either way — the project's own `scripts/check-platform-macros.ps1`
forbids that — exactly one of the stub/enabled translation units is ever compiled, selected in
`platform/android/app/src/main/cpp/CMakeLists.txt`.

**Unsigned builds omit the object container entirely, not just leave it unsigned.** An unsigned
but *present* EMDF container is not a safe degraded mode — per `AtmosConfig::emit_object_metadata`'s
own comment, a decoder that validates the `emdf_protection` field treats the container's sync word
as a commitment to object decoding and refuses the whole stream if it doesn't validate, rather than
falling back to plain 5.1. `shield_quarantine_hook.hpp`'s `signing_available()` (`true` only in the
enabled translation unit, and only once `AC3FORGE_SIGN` is actually set) lets `live_cursor.cpp`
decide this once at startup: `emit_object_metadata` is set to `signing_available()`, so the stub
(public/CI) build runs the same `bed51` mode `ac3cli --mode bed51` exposes — no container at all,
always safe, on every receiver — while only the enabled build ever emits and signs one.

**Off by default, and a checked-in file can no longer flip it on by accident.**
`app/build.gradle.kts` reads a `quarantineSignerEnabled` flag from `platform/android/local.properties`
— itself gitignored and per-machine already (it also holds `sdk.dir`) — rather than hardcoding the
CMake argument. The committed `build.gradle.kts` is therefore always safe to build in CI or for a
public release: `local.properties` doesn't exist there, so the flag reads `false` and every build
produces the `bed51`-equivalent unsigned app above. To sign on your own machine, add one line to
your own `local.properties` (never edit `build.gradle.kts` itself):

```properties
ac3forge.quarantineSigner=true
```

then build as normal:

```bash
./gradlew assembleDebug --no-daemon
```

The resulting APK, like the signer source itself, must never be distributed — sideload it to your
own Shield via `adb install` and nowhere else. There is nothing to revert afterward: the toggle
lives entirely in the gitignored `local.properties`, never in a file `git status` would ever show
as modified.

## Building and running

```bash
cd platform/android
./gradlew assembleDebug --no-daemon
adb connect <shield-ip>:5555          # if not on USB
adb -s <shield-ip>:5555 install -r app/build/outputs/apk/debug/app-debug.apk
adb -s <shield-ip>:5555 shell am start -n com.ac3forge.shield/.MainActivity
```

`local.properties` needs `sdk.dir` pointing at an Android SDK with NDK 26.1.10909125 and CMake
3.31.6 installed (via Android Studio's SDK Manager, or `sdkmanager --install`), plus, for a local
signed build, the `ac3forge.quarantineSigner=true` line described
[above](#the-quarantine-signer-dependency).

## Release / CI

The app builds alongside the desktop packages rather than only ever being hand-built locally:
`.github/workflows/_build.yml`'s `build-android` job builds the **debug** variant on every push
(no Android SDK/NDK setup beyond what `ubuntu-latest` ships plus an explicit pin of the exact NDK
version, `26.1.10909125`, the same "don't trust whatever the image happens to cache" reasoning
every other toolchain step in that workflow already follows) — a continuous smoke test proving the
Gradle/CMake/NDK toolchain and every native source file still build, the same role `windows-msvc`'s
always-on packaging step plays for the desktop legs. It never signs: CI has neither `src/quarantine/`
nor a `local.properties` entry to turn the signer on even if it did.

For an actual release (`release.yml`, `do_package: true`), the same job also builds and stages the
**release** variant — debug-keystore signed (this app is sideload-only, never the Play Store, so
there is no separate release keystore to provision), `CMAKE_BUILD_TYPE=Release` — as
`ac3forge-shield-<version>.apk`, uploaded as a `packages-android` artifact and folded into the
GitHub Release alongside the Windows/Linux/macOS packages (checksummed, GPG-signed, and
build-provenance-attested exactly like every other package — `release.yml`'s artifact globs all
include `*.apk`).

This job is marked `continue-on-error: true` — it has been validated to parse as valid YAML and to
mirror the project's own established toolchain-pinning conventions, but, unlike the desktop legs,
has not yet actually run on GitHub's hosted runners even once. Drop that line once a real CI run
has gone green, the same promotion process every other experimental leg in `_build.yml` has
followed (see that file's own header comment).

## What has and has not been verified

!!! note "Verified on real hardware"
    Installed, launched, and run on the developer's own Shield (Tegra X1 SoC) connected to a real
    AV receiver over HDMI. The encode loop holds exact real-time cadence (32.0ms/frame, zero
    underruns) for extended runs. Both Shield Controller analog input and D-pad/remote-style input
    (verified via `adb shell input keyevent` injection) move the correct object and the room
    visualization tracks the encode loop's own state — all against the app's earlier, single-fixed-object
    shape, before today's trajectory/deflection/ambient-object rewrite (below).

    **The quarantine-signed build's object audio has been confirmed reconstructable on the real
    receiver** — not just the always-audible panned bed. With the delta-bit-allocation fix
    described in the signer's own history (an unrelated bit-tracking bug that had been silently
    corrupting a large fraction of signed frames) and the real receiver powered on and HDMI-linked,
    the receiver's own front-panel display read **Atmos/DD+, 48kHz in, 5.0.4 out**, and the object's
    motion was audible. This resolves what had been an open question in
    [Two honest limitations](../concepts/atmos-joc.md#two-honest-limitations) for this specific
    encoder/signer pair, though the general caveat there still applies to any *other* clean-room
    encoder without its own quarantine signer.

    Both the release (unsigned) and local-signed debug builds install and launch without crashing;
    the release build's logcat confirms `object container: bed51 (omitted, unsigned build)` — the
    safe public default actually takes effect, not just compiles.

!!! warning "Not yet verified"
    Today's rewrite — the pre-planned orbit trajectory, input-driven deflection with spring-back,
    the two ambient objects, and the D-pad axis-mode toggle — has been built and installed but not
    yet exercised live: the real AV receiver was in use for something else by the time this landed,
    so the on-device motion/audio verification above is from the app's prior, simpler shape. Compile-
    and static-review-verified only for the new logic (`LiveCursorState::advance`/`deflect_selected`,
    `InputController`'s long-press disambiguation) until the receiver is free again. The
    `build-android` CI job itself is likewise unverified end to end — see
    [Release / CI](#release-ci) above. `tests/platform/android/` covers only the device-free logic
    (burst sizing, carrier rate, render-device construction), built and run on the normal
    desktop-hosted CTest suite, not the app itself.
