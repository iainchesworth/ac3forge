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
// THIS PASS uses a fixed, static placement and a synthesized tone - no
// controller input yet (that lands in a later pass; see
// docs/platforms/android.md's build-order notes). The point here is
// isolating whether the loop itself is correctly paced and produces clean,
// glitch-free audio over passthrough, before adding input handling on top.
// Note this is audible even unsigned: AtmosEncoder pans every object into
// the transmitted 5.1 bed (see atmos.hpp's header comment) - a legacy/
// non-JOC decode still hears it panned across the fixed channel layout, it
// just cannot reconstruct the object as a separate height-rendered source.
// The quarantine signer (a later, local-only pass) is what closes that gap.
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

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "ac3/core/tables.hpp"
#include "ac3/oba/atmos.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "ac3/sinks/passthrough.hpp"

namespace {

constexpr char kLogTag[] = "ac3forge.shield.live_cursor";
constexpr int kObjects = 1;
constexpr double kToneHz = 440.0;
constexpr double kSampleRate = 48000.0;

std::thread g_worker;
std::atomic_bool g_stop_requested{false};
std::atomic_bool g_running{false};

// Room center at ear height - see this file's header comment on why a fixed
// position is deliberate for this pass.
ac3::oba::ObjectPlacement static_placement() {
    return {.position = {.x = 0.5, .y = 0.5, .z = 0.0}, .gain = 1.0};
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
    std::vector<float> tone(ac3::kSamplesPerFrame);
    const std::vector<std::span<const float>> views{std::span<const float>(tone)};
    double phase = 0.0;
    const double phase_step = 2.0 * std::numbers::pi * kToneHz / kSampleRate;
    const std::array<ac3::oba::ObjectPlacement, kObjects> placement{static_placement()};

    // Wall-clock frame pacing, not a producer to drain (see header comment):
    // one AC-3 frame is exactly kSamplesPerFrame/48000 seconds.
    const auto frame_period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(ac3::kSamplesPerFrame / kSampleRate));
    auto next_deadline = std::chrono::steady_clock::now();

    g_running.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "encode loop started");

    std::uint64_t frames = 0;
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        for (std::size_t n = 0; n < tone.size(); ++n) {
            tone[n] = static_cast<float>(0.25 * std::sin(phase));
            phase += phase_step;
        }
        // Keep the running phase bounded - it only ever feeds sin(), so
        // this cannot audibly discontinue the waveform (sin is 2*pi
        // periodic), it just stops an unbounded double from slowly losing
        // precision over a long-running session.
        phase = std::fmod(phase, 2.0 * std::numbers::pi);

        const auto unit = encoder.encode_frame(views, placement);
        if (!unit) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "encode_frame failed: %d",
                                static_cast<int>(unit.error()));
            break;
        }

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
