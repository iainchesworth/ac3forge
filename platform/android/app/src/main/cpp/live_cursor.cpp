// The encode loop: this app's first real "live cursor" - the first place in
// the whole ac3forge project that drives AtmosEncoder::encode_frame() from a
// live, externally-set position every frame rather than from an authored
// KeyframePath/OrbitPath or a file. Modeled directly on ac3cli's run_live
// (src/cli/main.cpp) - same per-frame shape (build placement, encode_frame,
// IEC61937-wrap, PassthroughSink::submit with retry+sleep) - but self-paced
// by wall clock instead of run_live's "block on the capture ring buffer"
// mechanism, because this app synthesizes its own object audio; there is no
// upstream producer to drain.
//
// kObjects objects: kInteractiveObjects (currently 1) that a pre-planned
// trajectory carries around the room on its own, with input from
// InputController.kt biasing them off that course - held input pushes,
// releasing it lets the object spring back onto the trajectory (see
// LiveCursorState::deflect_selected/advance below) - plus kAmbientObjects
// that follow their own trajectory untouched by input at all, for the sound
// mixing/interaction a single moving voice can't demonstrate on its own. The
// encode loop calls LiveCursorState::advance() once per frame - this is the
// actual "live cursor" the file is named for.
//
// Audible even unsigned: AtmosEncoder pans every object into the
// transmitted 5.1 bed (see atmos.hpp's header comment) - a legacy/non-JOC
// decode still hears it panned across the fixed channel layout, it just
// cannot reconstruct the object as a separate height-rendered source. The
// quarantine signer (a later, local-only pass) is what closes that gap; see
// run_loop()'s emit_objects (ac3shield::signing_available()) for what an
// unsigned build does instead.
//
// Real-time viability history: this was briefly a pre-encode-then-loop-a-
// buffer diagnostic, because AtmosEncoder::encode_frame() measured at
// ~266ms/frame on this Shield's SoC - traced (Tracy) to the forward MDCT
// recomputing std::cos() fresh inside an O(N^2) loop instead of using a
// precomputed table the way the inverse transform already did (see
// src/lib/src/core/mdct.cpp's ForwardCosTable). Fixed there, not worked
// around here - this is the straight per-frame loop again.

#include <jni.h>

#include <android/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/sinks/passthrough.hpp"
#include "shield_quarantine_hook.hpp"

namespace {

constexpr char kLogTag[] = "ac3forge.shield.live_cursor";
constexpr int kInteractiveObjects = 1;
constexpr int kAmbientObjects = 2;
constexpr int kObjects = kInteractiveObjects + kAmbientObjects;
constexpr double kSampleRate = 48000.0;
// The interactive lead at A4, the two ambient objects a major third and a
// perfect fifth above it (C#5, E5) - an A major triad rather than an
// arbitrary/dissonant set of tones, so the "sound interaction/mixing" the
// ambient objects exist for is pleasant to actually listen to as they and
// the lead move past each other. Ambient objects sit a little quieter than
// the lead so it stays the clear focus of the demo.
constexpr std::array<double, kObjects> kToneHz{440.0, 554.365, 659.255};
constexpr std::array<double, kObjects> kToneGain{0.22, 0.12, 0.12};

// One object's pre-planned path: a circular orbit centred on the room's
// exact middle - oamd.hpp's (0.5, 0.5, 0) - which is also where the JOC/VBAP
// render implicitly assumes the listener sits, so every object's lap carries
// it both in front of AND behind that point rather than staying confined to
// the front half of the room. Height bobs independently and more slowly, so
// the path is a gentle tilted ellipse in 3-space rather than a flat circle.
// Distinct rate/phase/radius per object keeps the three visually and
// audibly distinguishable rather than moving in lockstep.
struct TrajectoryParams {
    double rate_hz;         // xy orbit revolutions per second
    double phase_rad;
    double radius;          // xy radius about the room centre, room units
    double height_amp;      // z bob amplitude, room units ([-1,1] full range)
    double height_rate_hz;  // z bob revolutions per second
};

constexpr std::array<TrajectoryParams, kObjects> kTrajectory{{
    // Interactive lead: one lap every 20s, a slow height bob so it reads as
    // deliberate rather than mechanical.
    {1.0 / 20.0, 0.0, 0.45, 0.5, 1.0 / 41.0},
    // Ambient 1: a smaller, slower orbit, phase-offset 120 degrees so it
    // starts on the opposite side of the room from the lead.
    {1.0 / 33.0, 2.0 * std::numbers::pi / 3.0, 0.28, 0.3, 1.0 / 53.0},
    // Ambient 2: offset a further 120 degrees, and its height bob runs in
    // the opposite sense (negative amplitude) so it and ambient 1 do not
    // mirror each other in height as well as azimuth.
    {1.0 / 27.0, 4.0 * std::numbers::pi / 3.0, 0.28, -0.3, 1.0 / 47.0},
}};

ac3::oba::Position trajectory_position(int obj, double time_s) {
    const auto& p = kTrajectory[static_cast<std::size_t>(obj)];
    const double angle = 2.0 * std::numbers::pi * p.rate_hz * time_s + p.phase_rad;
    const double height_angle =
        2.0 * std::numbers::pi * p.height_rate_hz * time_s + p.phase_rad;
    return {.x = 0.5 + p.radius * std::sin(angle),
            .y = 0.5 - p.radius * std::cos(angle),
            .z = p.height_amp * std::sin(height_angle)};
}

std::thread g_worker;
std::atomic_bool g_stop_requested{false};
std::atomic_bool g_running{false};

// Input-driven bias for one interactive object: how far its actual position
// currently sits from where trajectory_position() says it "should" be. Kept
// separate from the trajectory itself (rather than, say, directly nudging an
// absolute position) precisely so it can decay independently every frame -
// see advance() below - which is what makes "release the stick and it drifts
// back onto its course" work without InputController.kt ever having to tell
// native input has stopped.
struct Deflection {
    double x = 0.0, y = 0.0, z = 0.0;
};

// How far held input can push an object off its trajectory before the clamp
// in deflect_selected() stops it - the "bounding box" the deflection is
// limited by. xy is tighter than z: the xy trajectory radius is already up
// to 0.45 room-units (kTrajectory[0]), so 0.35 more still comfortably clears
// the walls once combined and clamped again in advance() below; z has more
// headroom since the trajectory's own height bob is modest.
constexpr double kMaxDeflectXy = 0.35;
constexpr double kMaxDeflectZ = 0.6;
// Per-encode-frame multiplicative decay applied to a deflection whether or
// not fresh input arrived this frame - the actual "spring-back". Frames are
// kSamplesPerFrame/48000 = 32ms apart; this value is exp(-frame_s / tau) for
// a tau of 1.5s, so a released deflection falls to ~1/e of its size in 1.5s
// and is effectively gone (~5%) after about 4.5s - unhurried enough to watch
// happen, quick enough not to feel unresponsive.
constexpr double kDeflectionDecayPerFrame = 0.9789;

// The live cursor itself: kObjects positions (kInteractiveObjects driven by
// a trajectory plus a decaying input deflection, the rest purely by their
// own trajectory) and which interactive object input currently targets.
// Written by the JNI functions at the bottom of this file (called from
// Kotlin's input-handling thread, roughly once per animation frame - see
// InputController.kt); advance() is called once per encode frame by
// run_loop() below, on the encode thread. A plain mutex, not
// atomics-per-field: this is nowhere near a contended hot path (one
// advance() plus at most a few deflect_selected() calls per ~16-32ms), and a
// mutex keeps a whole Deflection/ObjectPlacement's fields consistent with
// each other, which per-field atomics would not.
class LiveCursorState {
public:
    // Advances every object to `time_s`: the trajectory alone for ambient
    // objects, trajectory-plus-decaying-deflection for interactive ones.
    // Called once per encode frame - this is the only place deflection_
    // decays, so the spring-back happens on its own every frame regardless
    // of whether any input arrived.
    std::array<ac3::oba::ObjectPlacement, kObjects> advance(double time_s) {
        std::lock_guard lock(mutex_);
        for (int i = 0; i < kInteractiveObjects; ++i) {
            const auto base = trajectory_position(i, time_s);
            auto& defl = deflection_[static_cast<std::size_t>(i)];
            // Clamp to oamd.hpp's Position contract on top of the
            // deflection's own bounding-box clamp in deflect_selected():
            // that one keeps the BIAS itself bounded, this one keeps the
            // final trajectory+bias position inside the room even right at
            // the trajectory's own extremes (x,y in [0,1], z in [-1,1] - see
            // src/lib/include/ac3/oba/oamd.hpp).
            placements_[static_cast<std::size_t>(i)] = {
                .position = {.x = std::clamp(base.x + defl.x, 0.0, 1.0),
                            .y = std::clamp(base.y + defl.y, 0.0, 1.0),
                            .z = std::clamp(base.z + defl.z, -1.0, 1.0)},
                .gain = 1.0,
            };
            defl.x *= kDeflectionDecayPerFrame;
            defl.y *= kDeflectionDecayPerFrame;
            defl.z *= kDeflectionDecayPerFrame;
        }
        for (int i = kInteractiveObjects; i < kObjects; ++i) {
            placements_[static_cast<std::size_t>(i)] = {.position = trajectory_position(i, time_s),
                                                         .gain = 1.0};
        }
        return placements_;
    }

    // Adds (dx, dy, dz) to the selected interactive object's deflection,
    // clamped to the bounding box around its trajectory - see
    // InputController.kt for how dx/dy/dz are derived from the stick/D-pad
    // each animation frame.
    void deflect_selected(double dx, double dy, double dz) {
        std::lock_guard lock(mutex_);
        auto& defl = deflection_[static_cast<std::size_t>(selected_)];
        defl.x = std::clamp(defl.x + dx, -kMaxDeflectXy, kMaxDeflectXy);
        defl.y = std::clamp(defl.y + dy, -kMaxDeflectXy, kMaxDeflectXy);
        defl.z = std::clamp(defl.z + dz, -kMaxDeflectZ, kMaxDeflectZ);
    }

    int cycle_selected() {
        std::lock_guard lock(mutex_);
        selected_ = (selected_ + 1) % kInteractiveObjects;
        return selected_;
    }

    int selected() const {
        std::lock_guard lock(mutex_);
        return selected_;
    }

    // For the room visualization: the placements advance() last computed,
    // without advancing anything - RoomView polls this far more often
    // (every UI vsync) than the encode loop actually produces new frames.
    std::array<ac3::oba::ObjectPlacement, kObjects> snapshot() const {
        std::lock_guard lock(mutex_);
        return placements_;
    }

private:
    mutable std::mutex mutex_;
    std::array<Deflection, kInteractiveObjects> deflection_{};
    std::array<ac3::oba::ObjectPlacement, kObjects> placements_{};
    int selected_ = 0;
};

LiveCursorState& live_cursor_state() {
    static LiveCursorState state;
    return state;
}

void run_loop() {
    // A public (stub-hook) build never signs, so an emitted-but-unsigned
    // container would be the hard-refusal case AtmosConfig::emit_object_metadata's
    // own comment warns about, not a graceful 5.1 fallback - omit it entirely
    // instead (see shield_quarantine_hook.hpp's signing_available() comment).
    // Only the local, quarantine-signer-enabled build ever sets this true.
    const bool emit_objects = ac3shield::signing_available();
    ac3::oba::AtmosEncoder encoder({.bitrate_kbps = 448, .emit_object_metadata = emit_objects},
                                   kObjects);
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "object container: %s", emit_objects ? "objects (signed)" : "bed51 (omitted, unsigned build)");
    ac3::sinks::PassthroughSink sink;
    auto started = sink.start("", 48000, ac3::sinks::BitstreamFormat::kEac3);
    if (!started) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "PassthroughSink::start failed: %s",
                            std::string(ac3::sinks::describe(started.error())).c_str());
        return;
    }

    ac3::iec61937::Eac3BurstPacker packer;
    std::array<std::vector<float>, kObjects> tones;
    for (auto& tone : tones) {
        tone.resize(ac3::kSamplesPerFrame);
    }
    std::array<double, kObjects> phase{};
    std::array<double, kObjects> phase_step{};
    for (int obj = 0; obj < kObjects; ++obj) {
        phase_step[static_cast<std::size_t>(obj)] =
            2.0 * std::numbers::pi * kToneHz[static_cast<std::size_t>(obj)] / kSampleRate;
    }

    // Wall-clock frame pacing, not a producer to drain (see header comment):
    // one AC-3 frame is exactly kSamplesPerFrame/48000 seconds.
    const auto frame_period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(ac3::kSamplesPerFrame / kSampleRate));
    auto next_deadline = std::chrono::steady_clock::now();
    // Elapsed wall-clock time since the loop started is the trajectory's own
    // clock (trajectory_position's time_s) - a monotonic clock, not a sample
    // counter, so a stall/resync below (falling behind and skipping ahead)
    // moves the trajectory forward with it rather than the encoded audio and
    // the visible motion drifting apart.
    const auto start_time = std::chrono::steady_clock::now();

    g_running.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "encode loop started (%d interactive + %d ambient objects)",
                        kInteractiveObjects, kAmbientObjects);

    std::uint64_t frames = 0;
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        for (int obj = 0; obj < kObjects; ++obj) {
            auto& tone = tones[static_cast<std::size_t>(obj)];
            auto& ph = phase[static_cast<std::size_t>(obj)];
            const auto step = phase_step[static_cast<std::size_t>(obj)];
            const auto tone_gain = kToneGain[static_cast<std::size_t>(obj)];
            for (std::size_t n = 0; n < tone.size(); ++n) {
                tone[n] = static_cast<float>(tone_gain * std::sin(ph));
                ph += step;
            }
            // Keep the running phase bounded - it only ever feeds sin(), so
            // this cannot audibly discontinue the waveform (sin is 2*pi
            // periodic), it just stops an unbounded double from slowly
            // losing precision over a long-running session.
            ph = std::fmod(ph, 2.0 * std::numbers::pi);
        }
        const std::vector<std::span<const float>> views(tones.begin(), tones.end());
        const double time_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        const auto placement = live_cursor_state().advance(time_s);

        auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "encode_frame failed: %d",
                                static_cast<int>(unit.error()));
            break;
        }

        // Right after encode, before IEC61937 wrapping - unsigned by default
        // (returns false, a no-op) on every public build; see
        // shield_quarantine_hook.hpp. This is the ONLY thing standing
        // between "objects panned into the bed, audible but not
        // reconstructable" and "a real Dolby-licensed decoder actually
        // unlocks the objects" - see [[joc-decoder-auth-gate]].
        (void)ac3shield::maybe_sign_atmos_unit(unit->bytes);

        // push() returns expected<optional<vector<byte>>, WrapError>: the
        // outer expected is a hard wrap error (should not happen with our
        // own encoder's output); the inner optional is empty only until
        // blocks_pending_ reaches 6 - this encoder's frames carry all 6
        // blocks each (kSamplesPerFrame/256), so in practice one push() per
        // encode_frame() yields one burst immediately, not an accumulation
        // across several frames.
        const auto push_result = packer.push(unit->bytes);
        if (!push_result) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "iec61937 wrap failed: %d",
                                static_cast<int>(push_result.error()));
            break;
        }
        if (*push_result && !(*push_result)->empty()) {
            if (frames == 0) {
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "first burst ready: %zu bytes (expect %zu)",
                                    (*push_result)->size(), ac3::iec61937::kEac3BurstBytes);
            }
            int retry_count = 0;
            while (!sink.submit(**push_result)) {
                if (g_stop_requested.load(std::memory_order_acquire)) {
                    break;
                }
                if (++retry_count == 250) {  // ~500ms of retrying one burst
                    __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                        "submit() has retried %d times on frame %llu - "
                                        "AudioTrack is not draining",
                                        retry_count, static_cast<unsigned long long>(frames));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } else if (frames == 0) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "first frame produced no burst yet (accumulating blocks)");
        }

        if (frames % 48 == 0) {  // roughly every 1.5s at 32ms/frame
            const auto stats = sink.stats();
            __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                "frame %llu: bursts submitted=%llu rendered=%llu underruns=%llu",
                                static_cast<unsigned long long>(frames),
                                static_cast<unsigned long long>(stats.bursts_submitted),
                                static_cast<unsigned long long>(stats.bursts_rendered),
                                static_cast<unsigned long long>(stats.underruns));
        }
        ++frames;

        const auto now = std::chrono::steady_clock::now();
        if (now > next_deadline + frame_period) {
            // Running behind by more than one whole frame: resync to now
            // rather than let sleep_until race to catch up on a backlog
            // that will just keep growing (and each catch-up frame would
            // still submit as fast as possible, which is fine for
            // PassthroughSink but pointless if the receiver already lost
            // lock from the earlier gap).
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "frame %llu: encode loop fell behind, resyncing",
                                static_cast<unsigned long long>(frames));
            next_deadline = now;
        }
        next_deadline += frame_period;
        std::this_thread::sleep_until(next_deadline);
    }

    sink.stop();
    g_running.store(false, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "encode loop stopped");
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeStartLiveCursor(JNIEnv* /*env*/, jclass /*clazz*/) {
    if (g_running.load(std::memory_order_acquire)) {
        return JNI_TRUE;
    }
    if (g_worker.joinable()) {
        // A previous run's thread exited (g_running false) but was never
        // joined - stop() below handles the normal path; this only matters
        // if start() is called again without an intervening stop().
        g_worker.join();
    }
    g_stop_requested.store(false, std::memory_order_relaxed);
    g_worker = std::thread(run_loop);
    return JNI_TRUE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeStopLiveCursor(JNIEnv* /*env*/, jclass /*clazz*/) {
    g_stop_requested.store(true, std::memory_order_release);
    if (g_worker.joinable()) {
        g_worker.join();
    }
}

// Called from InputController.kt's animation ticker, roughly once per UI
// frame (~16ms) - NOT once per raw MotionEvent/KeyEvent, so a stick or held
// D-pad direction biases the object smoothly rather than in per-event jumps.
// dx/dy/dz are already scaled by the caller (stick magnitude x speed x
// elapsed time, or a D-pad direction x speed x elapsed time); this function
// only clamps the resulting deflection to its bounding box - see
// LiveCursorState::deflect_selected. The object itself keeps following its
// trajectory throughout; this only biases it off that course, and the bias
// decays back to zero on its own once input stops (LiveCursorState::advance).
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeDeflectSelectedObject(JNIEnv* /*env*/,
                                                                   jclass /*clazz*/, jfloat dx,
                                                                   jfloat dy, jfloat dz) {
    live_cursor_state().deflect_selected(dx, dy, dz);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeCycleSelectedObject(JNIEnv* /*env*/,
                                                                 jclass /*clazz*/) {
    return live_cursor_state().cycle_selected();
}

// For the room visualization (a later pass): one flat array, 4 floats per
// object (x, y, z, 1.0-if-selected-else-0.0), kObjects*4 long.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetObjectState(JNIEnv* env, jclass /*clazz*/) {
    const auto placement = live_cursor_state().snapshot();
    const int selected = live_cursor_state().selected();

    jfloatArray result = env->NewFloatArray(kObjects * 4);
    if (result == nullptr) {
        return nullptr;
    }
    std::array<jfloat, kObjects * 4> flat{};
    for (int obj = 0; obj < kObjects; ++obj) {
        flat[static_cast<std::size_t>(obj * 4 + 0)] =
            static_cast<jfloat>(placement[static_cast<std::size_t>(obj)].position.x);
        flat[static_cast<std::size_t>(obj * 4 + 1)] =
            static_cast<jfloat>(placement[static_cast<std::size_t>(obj)].position.y);
        flat[static_cast<std::size_t>(obj * 4 + 2)] =
            static_cast<jfloat>(placement[static_cast<std::size_t>(obj)].position.z);
        flat[static_cast<std::size_t>(obj * 4 + 3)] = obj == selected ? 1.0f : 0.0f;
    }
    env->SetFloatArrayRegion(result, 0, kObjects * 4, flat.data());
    return result;
}
