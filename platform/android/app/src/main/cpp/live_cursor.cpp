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

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
// the lead move past each other. Ambient objects sit quieter than the lead
// so it stays the clear focus of the demo, but loud enough to still be
// heard clearly - two rounds of real-device testing on a Shield + AVR each
// found the previous level still needed the receiver driven meaningfully
// hotter than normal listening level. These gains are deliberately chosen
// alongside soft_limit() below rather than kept safely under a linear 1.0
// ceiling on their own - see that function's own comment for why a limiter
// was the only way to get materially louder without just clipping.
constexpr std::array<double, kObjects> kToneHz{440.0, 554.365, 659.255};
constexpr std::array<double, kObjects> kToneGain{0.95, 0.30, 0.30};

// A soft-knee limiter, applied to every object's final sample below - pure
// linear gain increases run straight into a hard ceiling (any sample over
// +-1.0 hard-clips, an audible crackle, since this encoder does no internal
// limiting of its own), so getting meaningfully louder without distortion
// needs the peaks themselves shaped, not just a bigger multiplier. Fully
// transparent (identity) below kLimiterThreshold; only the excess above it
// is compressed, smoothly approaching but never reaching +-1.0 - ordinary
// low-level content is completely unaffected, only genuine peaks are
// tamed. tanh() only runs for samples that actually exceed the threshold,
// not on every sample.
constexpr float kLimiterThreshold = 0.72f;
float soft_limit(float x) {
    const float sign = x < 0.0f ? -1.0f : 1.0f;
    const float ax = std::abs(x);
    if (ax <= kLimiterThreshold) {
        return x;
    }
    const float excess = (ax - kLimiterThreshold) / (1.0f - kLimiterThreshold);
    const float compressed = kLimiterThreshold + (1.0f - kLimiterThreshold) * std::tanh(excess);
    return sign * std::min(compressed, 0.999f);
}

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

// height_amp is close to the full [-1,1] range for the lead (0.85, not the
// original 0.5) so its bob genuinely swings up near the ceiling and down
// near the floor rather than only ever reaching halfway - real-device
// testing found the original range read as barely-there movement rather
// than a source passing overhead. Lap rate is also faster (12s, not 20s) so
// the motion is perceptible within a short listening window rather than
// requiring a patient, full-lap wait to notice.
constexpr std::array<TrajectoryParams, kObjects> kTrajectory{{
    // Interactive lead: one lap every 12s.
    {1.0 / 12.0, 0.0, 0.45, 0.85, 1.0 / 25.0},
    // Ambient 1: a smaller, slower orbit, phase-offset 120 degrees so it
    // starts on the opposite side of the room from the lead.
    {1.0 / 33.0, 2.0 * std::numbers::pi / 3.0, 0.28, 0.45, 1.0 / 53.0},
    // Ambient 2: offset a further 120 degrees, and its height bob runs in
    // the opposite sense (negative amplitude) so it and ambient 1 do not
    // mirror each other in height as well as azimuth.
    {1.0 / 27.0, 4.0 * std::numbers::pi / 3.0, 0.28, -0.45, 1.0 / 47.0},
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

// Distance-based loudness: without this, an object sounded exactly as loud
// swinging past the listener at the room centre as it did out at the far
// edge of its orbit - correct panning direction, but no actual sense of
// "coming toward me" versus "far away". Inverse-square-ish, clamped with a
// floor so the far end of an orbit (~1 room-unit out, given the widened
// trajectory radii/heights above) is quieter but never silent - going
// fully silent would fight the "isolate and track it" point of the
// pause/mute feature rather than complement it.
constexpr double kDistanceFalloffK = 1.0;
constexpr double kDistanceAttenFloor = 0.4;
double distance_attenuation(const ac3::oba::Position& pos) {
    const double dx = pos.x - 0.5;
    const double dy = pos.y - 0.5;
    const double dz = pos.z;  // z is already centred on the listener's ear height
    const double dist_sq = dx * dx + dy * dy + dz * dz;
    const double atten = 1.0 / (1.0 + kDistanceFalloffK * dist_sq);
    return std::max(atten, kDistanceAttenFloor);
}

// The lead object's voice: a plain sine, however correctly panned, is a
// genuinely bad choice for demonstrating precise 3D localization by ear -
// real-device testing confirmed it as "muddy", not a discrete point source.
// Two independent reasons, not one:
//  - A single continuous tone with no onsets gives the ear a weak azimuth
//    (interaural time/level difference) cue - there is no transient to lock
//    onto, only a slowly-panning steady drone.
//  - Elevation localization in human hearing is driven almost entirely by
//    pinna-filtered high-frequency/broadband spectral cues - a 440Hz sine
//    has essentially no energy up there at all, so "hear it go overhead"
//    cannot work no matter how correct the encoder's height panning is.
// A rhythmic, sharpened amplitude envelope (kLeadRotorHz, a rotor-blade-like
// "thump" rather than a smooth tremolo) mixed with broadband noise addresses
// both: real onset transients for azimuth, and real high-frequency content
// for elevation. Ambient objects deliberately stay plain sines - a
// contrasting, non-percussive texture that's easy to tune out, so the lead
// reads as the one distinct, locatable voice by timbre alone, not just by
// having asked the listener to trust the panning.
constexpr double kLeadRotorHz = 5.0;
constexpr double kLeadRotorSharpness = 3.0;  // higher = shorter, more percussive pulses
constexpr float kLeadToneMix = 0.7f;
constexpr float kLeadNoiseMix = 0.3f;

// Cheap, deterministic PRNG (xorshift32) for the lead's noise texture - a
// fixed seed rather than time-seeded, since the exact sequence never
// matters, only that it sounds like noise; avoids <random>'s per-sample
// engine overhead in what is otherwise a tight, allocation-free hot loop.
std::uint32_t g_noise_state = 0x9e3779b9u;
float next_noise_sample() {
    g_noise_state ^= g_noise_state << 13;
    g_noise_state ^= g_noise_state >> 17;
    g_noise_state ^= g_noise_state << 5;
    return static_cast<float>(static_cast<std::int32_t>(g_noise_state)) * (1.0f / 2147483648.0f);
}

// The lead object's bundled voice: a seamlessly-looping, offline-rendered
// rotorcraft sample (rotor thump + tail rotor + engine drone + blade-slap
// noise + rumble - see tools/gen_lead_voice.py-style generation, richer than
// this tight real-time loop could afford per-sample on this SoC) rather than
// the live tone+noise synthesis above. Set once from Kotlin
// (MainActivity.onCreate, before nativeStartLiveCursor) via
// nativeSetAssetManager - a plain AAssetManager*, not wrapped in any
// lifetime-tracking type, since the Java-side AssetManager (and therefore
// this pointer) outlives the whole process once set. A raw pointer, not a
// GlobalRef-held jobject: AAssetManager_fromJava's returned native handle
// stays valid independent of any JNI local/global ref bookkeeping.
std::atomic<AAssetManager*> g_asset_manager{nullptr};
constexpr char kLeadVoiceAsset[] = "lead_voice_48k_mono_s16le.raw";

// Loads kLeadVoiceAsset fully into memory as [-1,1] float samples. Returns an
// empty vector (never throws/aborts) if no AAssetManager has been set yet, if
// the asset is missing from this particular build/packaging, or if the read
// comes back short - run_loop() below treats an empty result as "fall back
// to the live rotor+noise synthesis", not as a fatal error, since a missing
// bundled asset is a real, recoverable packaging-boundary condition, not
// something internal code can just assume never happens.
std::vector<float> load_lead_voice_sample() {
    AAssetManager* mgr = g_asset_manager.load(std::memory_order_relaxed);
    if (mgr == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "no AAssetManager set - lead object will use live-synthesized voice");
        return {};
    }
    AAsset* asset = AAssetManager_open(mgr, kLeadVoiceAsset, AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "asset '%s' not found - lead object will use live-synthesized voice",
                            kLeadVoiceAsset);
        return {};
    }
    const off_t length = AAsset_getLength(asset);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(length) / sizeof(std::int16_t));
    const int read_bytes = AAsset_read(asset, pcm.data(), static_cast<std::size_t>(length));
    AAsset_close(asset);
    if (read_bytes != length || pcm.empty()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "asset '%s' read incomplete (%d/%lld bytes) - lead object will use "
                            "live-synthesized voice",
                            kLeadVoiceAsset, read_bytes, static_cast<long long>(length));
        return {};
    }
    std::vector<float> samples(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        samples[i] = static_cast<float>(pcm[i]) * (1.0f / 32768.0f);
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "loaded lead voice asset: %zu samples (%.2fs)",
                        samples.size(), static_cast<double>(samples.size()) / kSampleRate);
    return samples;
}

std::thread g_worker;
std::atomic_bool g_stop_requested{false};
std::atomic_bool g_running{false};
// run_loop()'s own start_time, published so nativeGetFutureLeadTrajectory()
// (a JNI call from the UI thread, not the encode-loop thread) can compute
// "now" in the exact same time base trajectory_position() uses without
// touching anything mutex-protected - trajectory_position() is a pure
// function of time, so this is the only state that call actually needs.
std::atomic<std::int64_t> g_start_time_ns{0};

// Live stream stats + the ambient-mute control, both read/written from
// different threads (RoomView.kt's UI-thread poll, the remote's play/pause
// keys, the encode-loop thread that updates the stats every frame) - plain
// atomics, not LiveCursorState's mutex, since every field here is
// independent and there is nothing that needs to stay consistent ACROSS
// fields the way a whole ObjectPlacement does.
struct StreamStats {
    std::atomic<std::uint64_t> frames{0};
    std::atomic<std::uint64_t> bursts_submitted{0};
    std::atomic<std::uint64_t> bursts_rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::atomic_bool signed_stream{false};
    // Set by the remote's pause/play keys (InputController.kt) - muting the
    // two ambient objects' audio (not their position/trajectory, which keeps
    // advancing in the background) so a listener can isolate the lead
    // object's sound and hear its own movement without the ambient wash on
    // top of it. "Restart everything that was paused" on play is therefore
    // just un-muting: the ambient objects never stopped moving, only sounding.
    std::atomic_bool ambient_muted{false};
    // How long the last encode_frame() call actually took - for the on-
    // screen "encode headroom" readout (real-time viability was a genuine,
    // previously-hit problem on this SoC - see this file's own header - so
    // showing the live number rather than just asserting it stays fast is
    // worth doing).
    std::atomic<float> encode_ms{0.0f};
    // AC-3 coded order (L, C, R, Ls, Rs, LFE) - see AtmosEncoder::bed()'s own
    // comment. RMS level per channel of the REAL, actually-encoded 5.1 bed,
    // not an approximation from the room-position math - see run_loop()'s
    // own comment on where this is computed. For the speaker-activity meter.
    std::array<std::atomic<float>, 6> channel_levels{};
};

StreamStats& stream_stats() {
    static StreamStats s;
    return s;
}

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
// to 0.45 room-units (kTrajectory[0]), so 0.45 more still comfortably clears
// the walls once combined and clamped again in advance() below (real-device
// testing found the original, tighter 0.35 made a deliberate push feel
// underwhelming); z has even more headroom since the trajectory's own
// height bob, even widened above, only reaches +-0.85.
constexpr double kMaxDeflectXy = 0.45;
constexpr double kMaxDeflectZ = 0.75;
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

    // Instantly zeroes the selected object's deflection, rather than waiting
    // out kDeflectionDecayPerFrame's own ~1.5s time constant - called from a
    // long-press of A/center (InputController.kt), replacing what used to be
    // "cycle selected object" there (a no-op with a single interactive
    // object anyway) with something a presenter can actually use: a crisp,
    // on-demand "and... reset" moment instead of watching it drift back.
    void snap_selected() {
        std::lock_guard lock(mutex_);
        deflection_[static_cast<std::size_t>(selected_)] = Deflection{};
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
    stream_stats().signed_stream.store(emit_objects, std::memory_order_relaxed);
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
    // Only the lead (kInteractiveObjects == 1) has a rotor envelope; see
    // kLeadRotorHz's own comment.
    double rotor_phase = 0.0;
    const double rotor_step = 2.0 * std::numbers::pi * kLeadRotorHz / kSampleRate;

    // The lead's bundled voice, if this build has one packaged and an
    // AAssetManager was registered before this thread started (see
    // load_lead_voice_sample()'s own comment). Empty means "use the live
    // rotor+tone+noise synthesis below instead" - never a fatal condition.
    const std::vector<float> lead_sample = load_lead_voice_sample();
    std::size_t lead_sample_pos = 0;

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
    g_start_time_ns.store(
        std::chrono::duration_cast<std::chrono::nanoseconds>(start_time.time_since_epoch())
            .count(),
        std::memory_order_relaxed);

    g_running.store(true, std::memory_order_release);
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "encode loop started (%d interactive + %d ambient objects)",
                        kInteractiveObjects, kAmbientObjects);

    std::uint64_t frames = 0;
    while (!g_stop_requested.load(std::memory_order_acquire)) {
        // Placement is advanced FIRST, before the tone synthesis below reads
        // it for distance_attenuation() - this frame's positions have to be
        // known before this frame's samples can reflect how far each object
        // currently sits from the listener. time_s is computed here too
        // (moved up from after synthesis) for the same reason: the position
        // this loop iteration encodes should be timestamped from the START
        // of the work it does, not after several hundred microseconds of
        // sample generation have already elapsed.
        const double time_s =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        const auto placement = live_cursor_state().advance(time_s);

        const bool ambient_muted = stream_stats().ambient_muted.load(std::memory_order_relaxed);
        for (int obj = 0; obj < kObjects; ++obj) {
            auto& tone = tones[static_cast<std::size_t>(obj)];
            auto& ph = phase[static_cast<std::size_t>(obj)];
            const auto step = phase_step[static_cast<std::size_t>(obj)];
            const bool is_lead = obj < kInteractiveObjects;
            // Muting only silences the ambient objects' audio, never the
            // lead's - see StreamStats::ambient_muted's own comment.
            // distance_attenuation() reads THIS frame's own placement -
            // real position-based loudness, not merely correct panning
            // direction; see that function's own comment.
            const auto tone_gain =
                kToneGain[static_cast<std::size_t>(obj)] *
                ((!is_lead && ambient_muted) ? 0.0 : 1.0) *
                distance_attenuation(placement[static_cast<std::size_t>(obj)].position);
            if (is_lead && !lead_sample.empty()) {
                // The bundled asset already has its own rotor/tail-rotor/
                // engine/blade-slap structure baked in offline - no live
                // envelope or noise mixing needed here, just loop playback
                // through the same tone_gain/soft_limit chain every voice
                // goes through.
                for (std::size_t n = 0; n < tone.size(); ++n) {
                    tone[n] = soft_limit(static_cast<float>(tone_gain) * lead_sample[lead_sample_pos]);
                    lead_sample_pos = (lead_sample_pos + 1) % lead_sample.size();
                }
            } else if (is_lead) {
                for (std::size_t n = 0; n < tone.size(); ++n) {
                    // See kLeadRotorHz's own comment for why this is a
                    // rhythmic, sharpened envelope over a tone+noise mix
                    // rather than a bare sine. Floor raised to 0.55 (from an
                    // earlier 0.35): real-device testing asked for more
                    // loudness twice over, and a shallower dip between
                    // pulses raises the average level while still leaving a
                    // clearly audible ~5dB swing for the percussive "thump"
                    // itself - most of the extra loudness instead comes from
                    // kToneGain and soft_limit() below.
                    const double pulse = std::max(0.0, std::sin(rotor_phase));
                    const double envelope = 0.55 + 0.45 * std::pow(pulse, kLeadRotorSharpness);
                    const float voice = kLeadToneMix * static_cast<float>(std::sin(ph)) +
                                        kLeadNoiseMix * next_noise_sample();
                    tone[n] = soft_limit(static_cast<float>(tone_gain * envelope) * voice);
                    ph += step;
                    rotor_phase += rotor_step;
                }
                rotor_phase = std::fmod(rotor_phase, 2.0 * std::numbers::pi);
            } else {
                for (std::size_t n = 0; n < tone.size(); ++n) {
                    tone[n] = soft_limit(static_cast<float>(tone_gain * std::sin(ph)));
                    ph += step;
                }
            }
            // Keep the running phase bounded - it only ever feeds sin(), so
            // this cannot audibly discontinue the waveform (sin is 2*pi
            // periodic), it just stops an unbounded double from slowly
            // losing precision over a long-running session.
            ph = std::fmod(ph, 2.0 * std::numbers::pi);
        }
        const std::vector<std::span<const float>> views(tones.begin(), tones.end());

        // Timed for the on-screen "encode headroom" readout - this loop's
        // own real-time viability was a genuine, previously-hit problem on
        // this SoC (see this file's own header comment on the MDCT fix), so
        // showing the live number is worth more here than it would be in
        // most encode loops.
        const auto encode_start = std::chrono::steady_clock::now();
        auto unit = encoder.encode_frame(views, placement);
        const auto encode_elapsed_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                       encode_start)
                .count();
        stream_stats().encode_ms.store(static_cast<float>(encode_elapsed_ms),
                                       std::memory_order_relaxed);
        if (!unit) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "encode_frame failed: %d",
                                static_cast<int>(unit.error()));
            break;
        }

        // For the speaker-activity meter: RMS level of each of the six REAL
        // bed channels this frame actually encoded (AtmosEncoder::bed(), AC-3
        // coded order L/C/R/Ls/Rs/LFE) - not a guess derived from the room
        // position math, the literal audio a legacy decoder hears. x4.0 is a
        // fixed display gain (typical per-channel RMS with three panned
        // objects sits well under 1.0; without some boost the meter would
        // barely move) - visual only, has no effect on the encoded audio.
        {
            const auto bed = encoder.bed();
            for (std::size_t ch = 0; ch < bed.size() && ch < 6; ++ch) {
                double sum_sq = 0.0;
                for (const float sample : bed[ch]) {
                    sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
                }
                const double rms =
                    bed[ch].empty() ? 0.0 : std::sqrt(sum_sq / static_cast<double>(bed[ch].size()));
                const float level = static_cast<float>(std::clamp(rms * 4.0, 0.0, 1.0));
                stream_stats().channel_levels[ch].store(level, std::memory_order_relaxed);
            }
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

        // Published every frame, not just the 48-frame logcat interval below
        // - RoomView.kt's on-screen stats readout polls this once per vsync
        // and should read live, not update in visible 1.5s jumps.
        {
            const auto stats = sink.stats();
            stream_stats().frames.store(frames, std::memory_order_relaxed);
            stream_stats().bursts_submitted.store(stats.bursts_submitted, std::memory_order_relaxed);
            stream_stats().bursts_rendered.store(stats.bursts_rendered, std::memory_order_relaxed);
            stream_stats().underruns.store(stats.underruns, std::memory_order_relaxed);
            if (frames % 48 == 0) {  // roughly every 1.5s at 32ms/frame
                __android_log_print(ANDROID_LOG_INFO, kLogTag,
                                    "frame %llu: bursts submitted=%llu rendered=%llu underruns=%llu",
                                    static_cast<unsigned long long>(frames),
                                    static_cast<unsigned long long>(stats.bursts_submitted),
                                    static_cast<unsigned long long>(stats.bursts_rendered),
                                    static_cast<unsigned long long>(stats.underruns));
            }
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

// Called once from MainActivity.onCreate, before nativeStartLiveCursor - see
// g_asset_manager's own comment. AAssetManager_fromJava's returned handle is
// tied to the passed-in AssetManager object's lifetime; MainActivity passes
// its own Context.getAssets() result, which lives for the whole process, so
// no GlobalRef/cleanup is needed here.
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeSetAssetManager(JNIEnv* env, jclass /*clazz*/,
                                                              jobject asset_manager) {
    g_asset_manager.store(AAssetManager_fromJava(env, asset_manager), std::memory_order_relaxed);
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

// Called from InputController.kt's long-press handling. See
// LiveCursorState::snap_selected's own comment.
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeSnapSelectedToCourse(JNIEnv* /*env*/,
                                                                  jclass /*clazz*/) {
    live_cursor_state().snap_selected();
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

// Called from InputController.kt's play/pause key handling. See
// StreamStats::ambient_muted's own comment for what this actually does
// (mutes the ambient objects' audio, not their motion).
extern "C" JNIEXPORT void JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeSetAmbientMuted(JNIEnv* /*env*/, jclass /*clazz*/,
                                                              jboolean muted) {
    stream_stats().ambient_muted.store(muted != JNI_FALSE, std::memory_order_relaxed);
}

// For the room visualization's 3D trail view: `sample_count` (x,y,z) points
// along the LEAD object's own pre-planned trajectory - deliberately its base
// path only, with no deflection - starting now and running `seconds_ahead`
// seconds into the future. Deflection is excluded on purpose: it decays back
// to zero on its own (LiveCursorState::advance) and future input can't be
// known, so the only honest "path ahead" to show is the course the object
// is actually heading back toward, not a guess at what a listener might do
// with the stick next. trajectory_position() is a pure function of time -
// this needs none of LiveCursorState's mutex-protected state, only
// g_start_time_ns to translate "now" into the same elapsed-seconds time
// base run_loop() itself uses.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetFutureLeadTrajectory(JNIEnv* env,
                                                                     jclass /*clazz*/,
                                                                     jfloat seconds_ahead,
                                                                     jint sample_count) {
    if (sample_count <= 0) {
        return env->NewFloatArray(0);
    }
    const auto start_ns = g_start_time_ns.load(std::memory_order_relaxed);
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    const double now_s = static_cast<double>(now_ns - start_ns) / 1.0e9;

    jfloatArray result = env->NewFloatArray(sample_count * 3);
    if (result == nullptr) {
        return nullptr;
    }
    std::vector<jfloat> flat(static_cast<std::size_t>(sample_count) * 3);
    const int steps = sample_count > 1 ? sample_count - 1 : 1;
    for (int i = 0; i < sample_count; ++i) {
        const double t = now_s + (static_cast<double>(i) / static_cast<double>(steps)) *
                                     static_cast<double>(seconds_ahead);
        const auto pos = trajectory_position(0, t);  // object 0: the lead
        flat[static_cast<std::size_t>(i) * 3 + 0] = static_cast<jfloat>(pos.x);
        flat[static_cast<std::size_t>(i) * 3 + 1] = static_cast<jfloat>(pos.y);
        flat[static_cast<std::size_t>(i) * 3 + 2] = static_cast<jfloat>(pos.z);
    }
    env->SetFloatArrayRegion(result, 0, sample_count * 3, flat.data());
    return result;
}

// For the on-screen stats overlay (RoomView.kt) - a single formatted line
// rather than several numeric fields, since there is exactly one caller and
// building the string here avoids Kotlin needing its own copy of the same
// formatting logic.
extern "C" JNIEXPORT jstring JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetStreamStatsText(JNIEnv* env, jclass /*clazz*/) {
    auto& s = stream_stats();
    // Frame count dropped and "lost" only appended when actually nonzero -
    // the 3D track card that hosts this text got considerably narrower once
    // top-down/elevation moved beside it instead of behind it (see
    // RoomView.kt's layout rebalance), and the full "frame N | bursts ...
    // (0 lost) | ..." string no longer fit at any font size worth reading -
    // confirmed clipped on a real device screenshot. Frame count was the
    // least useful field to a viewer anyway; "0 lost" is the expected,
    // non-event case and not worth the width when it's the common case.
    const auto underruns = s.underruns.load(std::memory_order_relaxed);
    char loss_buf[24] = "";
    if (underruns > 0) {
        std::snprintf(loss_buf, sizeof loss_buf, " (%llu lost)",
                      static_cast<unsigned long long>(underruns));
    }
    char buf[192];
    std::snprintf(buf, sizeof buf,
                  "bursts %llu/%llu%s | encode %.1fms/32ms | %s%s",
                  static_cast<unsigned long long>(s.bursts_rendered.load(std::memory_order_relaxed)),
                  static_cast<unsigned long long>(s.bursts_submitted.load(std::memory_order_relaxed)),
                  loss_buf,
                  static_cast<double>(s.encode_ms.load(std::memory_order_relaxed)),
                  s.signed_stream.load(std::memory_order_relaxed) ? "Atmos (signed)"
                                                                   : "5.1 bed (unsigned)",
                  s.ambient_muted.load(std::memory_order_relaxed) ? " | ambient muted" : "");
    return env->NewStringUTF(buf);
}

// For the speaker-activity meter (RoomView.kt / a small dedicated channel-
// meter view) - 6 floats, AC-3 coded order (L, C, R, Ls, Rs, LFE), see
// StreamStats::channel_levels's own comment for what they represent.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_com_ac3forge_shield_NativeBridge_nativeGetChannelLevels(JNIEnv* env, jclass /*clazz*/) {
    jfloatArray result = env->NewFloatArray(6);
    if (result == nullptr) {
        return nullptr;
    }
    std::array<jfloat, 6> flat{};
    auto& s = stream_stats();
    for (std::size_t ch = 0; ch < 6; ++ch) {
        flat[ch] = s.channel_levels[ch].load(std::memory_order_relaxed);
    }
    env->SetFloatArrayRegion(result, 0, 6, flat.data());
    return result;
}
