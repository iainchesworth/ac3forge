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

## Input: Shield Controller and basic remote, both

`InputController.kt` supports both devices this app is meant to run under, detected at the event
level rather than requiring the user to pick a mode:

- **Shield Controller** (`SOURCE_JOYSTICK`): both analog sticks, read in `onGenericMotionEvent`
  with a 0.15 deadzone, driving continuous movement scaled by elapsed time via a
  `Choreographer.FrameCallback` ticker — held-stick movement is smooth, not per-event-stepped.
- **Basic Shield remote** (D-pad only, no sticks): `onKeyDown` applies a fixed 0.05 step per press
  for x/y, with `L1`/`R1` (or the controller's bumpers) stepping z and `A`/D-pad-center cycling the
  selected object — the same JNI entry points (`nativeMoveSelectedObject`/`nativeCycleSelectedObject`)
  either device drives.

Either way, input is coalesced to **at most one JNI call per animation frame**, never per raw input
event — the native side (`live_cursor.cpp`'s `LiveCursorState`, mutex-protected) reads one snapshot
per encode frame.

## Visualization

`RoomView.kt` is a plain `View` (not a `SurfaceView` — a handful of `drawCircle`/`drawLine` calls
per frame doesn't justify a `SurfaceView`'s own render thread and `SurfaceHolder` lifecycle),
invalidated once per vsync via `Choreographer.postFrameCallback`. It draws two panels side by side,
both reading the same `NativeBridge.nativeGetObjectState()` snapshot the encode loop just built for
that frame: a top-down X/Y view and a side-elevation X/Z view, with the selected object ringed.
Verified against real device screenshots (`adb shell screencap`) that positions track both
controller/remote input and the encode loop's own state exactly.

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

**Off by default.** A normal build of this app — including anything that might ever reach a public
branch — neither sees nor references `src/quarantine`. Enabling it is a local, personal build
step only, mirroring the existing `local/quarantine-signer` workflow, and — deliberately — not
exposed as a checked-in Gradle property that a routine build could flip on by accident: with
`src/quarantine/` present locally, add `"-DAC3FORGE_QUARANTINE_SIGNER=ON"` to the `debug` build type's
`externalNativeBuild.cmake.arguments` list in `app/build.gradle.kts` (an uncommitted local edit,
right next to the existing `-DCMAKE_BUILD_TYPE=RelWithDebInfo` argument), then build as normal:

```bash
./gradlew assembleDebug --no-daemon
```

The resulting APK, like the signer source itself, must never be distributed — sideload it to your
own Shield via `adb install` and nowhere else, and revert the local `build.gradle.kts` edit
afterward so it doesn't get swept into a future commit.

## Building and running

```bash
cd platform/android
./gradlew assembleDebug --no-daemon
adb connect <shield-ip>:5555          # if not on USB
adb -s <shield-ip>:5555 install -r app/build/outputs/apk/debug/app-debug.apk
adb -s <shield-ip>:5555 shell am start -n com.ac3forge.shield/.MainActivity
```

`local.properties` needs `sdk.dir` pointing at an Android SDK with NDK 26.1.10909125 and CMake
3.31.6 installed (via Android Studio's SDK Manager, or `sdkmanager --install`).

## What has and has not been verified

!!! note "Verified on real hardware"
    Installed, launched, and run on the developer's own Shield (Tegra X1 SoC) connected to a real
    AV receiver over HDMI. The encode loop holds exact real-time cadence (32.0ms/frame, zero underruns) for
    extended runs. An unsigned build's audio is audible through the receiver, including the tone
    associated with each object. Both Shield Controller analog input and D-pad/remote-style input
    (verified via `adb shell input keyevent` injection) move the correct object, the room
    visualization tracks the same state the encode loop reads, and selection cycling works.

!!! warning "Not verified"
    The quarantine-signed build's *object* audio has not been separately confirmed as
    reconstructable/height-rendered on the receiver (as opposed to the always-audible panned bed)
    — see [Two honest limitations](../concepts/atmos-joc.md#two-honest-limitations) for the
    general caveat that applies to every platform. This app is not in CI (Android has no runner
    with real audio hardware attached, and the encode-loop/visualization behavior above is only
    meaningfully verified by ear and by eye on the real device) — `tests/platform/android/` covers only the
    device-free logic (burst sizing, carrier rate, render-device construction), built and run on
    the normal desktop-hosted CTest suite, not the app itself.
