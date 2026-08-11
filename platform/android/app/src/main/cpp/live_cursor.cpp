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
// kObjects objects, each a fixed tone, spread around the room. Exactly one
// is "selected" at a time; NativeBridge.nativeMoveSelectedObject() (called
// from Kotlin's InputController - see that file for the Shield Controller/
// remote input mapping) nudges the selected object's position, and
// nativeCycleSelectedObject() moves the selection to the next one. The
// encode loop reads the current LiveCursorState once per frame - this is
// the actual "live cursor" the file is named for.
//
// Audible even unsigned: AtmosEncoder pans every object into the
// transmitted 5.1 bed (see atmos.hpp's header comment) - a legacy/non-JOC
// decode still hears it panned across the fixed channel layout, it just
// cannot reconstruct the object as a separate height-rendered source. The
// quarantine signer (a later, local-only pass) is what closes that gap.
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
constexpr int kObjects = 3;
constexpr double kSampleRate = 48000.0;
// One tone per object so they stay distinguishable by ear while moving.
constexpr std::array<double, kObjects> kToneHz{440.0, 554.37, 659.25};  // A4, C#5, E5

std::thread g_worker;
std::atomic_bool g_stop_requested{false};
std::atomic_bool g_running{false};

// The live cursor itself: kObjects positions plus which one input currently
// moves. Written by the JNI functions at the bottom of this file (called
// from Kotlin's input-handling thread, roughly once per animation frame -
// see InputController.kt), read once per encode frame by run_loop() below.
// A plain mutex, not atomics-per-field: kObjects is tiny, this is nowhere
// near a contended hot path (one read + at most one write per ~16-32ms), and
// a mutex keeps a whole ObjectPlacement's fields (position.x/y/z, gain)
// consistent with each other, which per-field atomics would not.
class LiveCursorState {
public:
    std::array<ac3::oba::ObjectPlacement, kObjects> snapshot() const {
        std::lock_guard lock(mutex_);
        return placements_;
    }

    void move_selected(double dx, double dy, double dz) {
        std::lock_guard lock(mutex_);
        auto& pos = placements_[static_cast<std::size_t>(selected_)].position;
        // Clamp to oamd.hpp's Position contract: x,y in [0,1] wall-to-wall,
        // z in [-1,1] floor-to-ceiling - see src/lib/include/ac3/oba/oamd.hpp.
        pos.x = std::clamp(pos.x + dx, 0.0, 1.0);
        pos.y = std::clamp(pos.y + dy, 0.0, 1.0);
        pos.z = std::clamp(pos.z + dz, -1.0, 1.0);
    }

    int cycle_selected() {
        std::lock_guard lock(mutex_);
        selected_ = (selected_ + 1) % kObjects;
        return selected_;
    }

    int selected() const {
        std::lock_guard lock(mutex_);
        return selected_;
    }

private:
    mutable std::mutex mutex_;
    // Spread front-to-back and left-to-right so they start audibly and
    // visually distinct; all at ear height (z=0) until moved.
    std::array<ac3::oba::ObjectPlacement, kObjects> placements_{{
        {.position = {.x = 0.25, .y = 0.35, .z = 0.0}, .gain = 1.0},
        {.position = {.x = 0.50, .y = 0.65, .z = 0.0}, .gain = 1.0},
        {.position = {.x = 0.75, .y = 0.35, .z = 0.0}, .gain = 1.0},
    }};
    int selected_ = 0;
};

LiveCursorState& live_cursor_state() {
    static LiveCursorState state;
    return state;
}

void run_loop() {
    ac3::oba::AtmosEncoder encoder({.bitrate_kbps = 448}, kObjects);
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

    g_running.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "encode loop started (%d objects)", kObjects);

    std::uint64_t frames = 0;
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        for (int obj = 0; obj < kObjects; ++obj) {
            auto& tone = tones[static_cast<std::size_t>(obj)];
            auto& ph = phase[static_cast<std::size_t>(obj)];
            const auto step = phase_step[static_cast<std::size_t>(obj)];
            for (std::size_t n = 0; n < tone.size(); ++n) {
                tone[n] = static_cast<float>(0.2 * std::sin(ph));
                ph += step;
            }
            // Keep the running phase bounded - it only ever feeds sin(), so
            // this cannot audibly discontinue the waveform (sin is 2*pi
            // periodic), it just stops an unbounded double from slowly
            // losing precision over a long-running session.
            ph = std::fmod(ph, 2.0 * std::numbers::pi);
        }
        const std::vector<std::span<const float>> views(tones.begin(), tones.end());
        const auto placement = live_cursor_state().snapshot();

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
// frame (~16ms) - NOT once per raw MotionEvent/KeyEvent, so a stick held at
// full deflection moves smoothly rather than in per-event jumps. dx/dy/dz
// are already scaled by the caller (stick magnitude x speed x elapsed time,
// or a fixed D-pad step) - this function only clamps to the room.
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeMoveSelectedObject(JNIEnv* /*env*/, jclass /*clazz*/,
                                                                jfloat dx, jfloat dy, jfloat dz) {
    live_cursor_state().move_selected(dx, dy, dz);
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
