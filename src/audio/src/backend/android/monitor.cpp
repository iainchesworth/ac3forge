#include "ac3/sinks/monitor.hpp"

// The Android monitor backend. CMake compiles this directory's monitor.cpp
// on Android and another platform directory's everywhere else, so there is
// no #ifdef - the file's path is what says "Android".
//
// This is genuine AAudio (the NDK's native audio API, <aaudio/AAudio.h>) -
// shared-mode float PCM playback needs nothing from Java, unlike
// passthrough.cpp in this same directory. See audio_backend.cpp for why
// that split exists.

#include <aaudio/AAudio.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>

#include "ac3/capture/ring_buffer.hpp"

namespace ac3::sinks {

namespace {

// Frames pulled from the queue per AAudioStream_write() call. An arbitrary
// chunk size (~10ms @ 48kHz), not a driver requirement - AAudio's blocking
// write accepts any frame count and simply blocks until it is consumed or
// the timeout below elapses.
constexpr std::int32_t kFramesPerWrite = 480;
constexpr std::int64_t kWriteTimeoutNanos = 20'000'000;  // 20ms

}  // namespace

std::string_view describe(MonitorError error) {
    switch (error) {
        case MonitorError::kNoBackend: return "no monitor backend on this platform";
        case MonitorError::kComFailure: return "an AAudio call failed";
        case MonitorError::kDeviceNotFound: return "AAudio could not open an output stream";
        case MonitorError::kAlreadyRunning: return "monitor playback is already running";
        case MonitorError::kNotRunning: return "monitor playback is not running";
    }
    return "unknown monitor error";
}

struct MonitorSink::Impl {
    AAudioStream* stream = nullptr;
    std::unique_ptr<capture::RingBuffer> queue;
    // std::thread + a manual stop flag, not std::jthread/std::stop_token:
    // NDK r26's bundled libc++ does not implement <stop_token> at all (see
    // docs/platforms/android.md) - the Windows/ALSA backends use jthread
    // because their libstdc++/MSVC STL both have it, but that is not
    // universal yet.
    std::thread worker;
    std::atomic_bool worker_stop_requested{false};
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::uint16_t channels = 0;
};

MonitorSink::MonitorSink() : impl_(std::make_unique<Impl>()) {}

MonitorSink::~MonitorSink() {
    stop();
}

bool MonitorSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

MonitorStats MonitorSink::stats() const {
    return {.frames_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .frames_rendered = impl_->rendered.load(std::memory_order_relaxed),
            .underruns = impl_->underruns.load(std::memory_order_relaxed)};
}

bool MonitorSink::can_submit() const {
    if (!impl_->queue || impl_->channels == 0) {
        return false;
    }
    // Same ~20ms-of-room heuristic as the Windows backend; see that file's
    // comment on why submit() below re-checks against the actual chunk size
    // rather than trusting this fixed threshold alone.
    return impl_->queue->capacity() - impl_->queue->available() >
           static_cast<std::size_t>(impl_->channels) * 960;
}

bool MonitorSink::submit(std::span<const float> interleaved) {
    if (!running() || !impl_->queue || impl_->channels == 0 ||
        interleaved.size() % impl_->channels != 0) {
        return false;
    }
    if (impl_->queue->capacity() - impl_->queue->available() <= interleaved.size()) {
        return false;
    }
    const auto wrote = impl_->queue->write(interleaved);
    if (wrote != interleaved.size()) {
        return false;
    }
    impl_->submitted.fetch_add(interleaved.size() / impl_->channels, std::memory_order_relaxed);
    return true;
}

void MonitorSink::stop() {
    if (impl_->worker.joinable()) {
        impl_->worker_stop_requested.store(true, std::memory_order_release);
        impl_->worker.join();
    }
    if (impl_->stream != nullptr) {
        AAudioStream_requestStop(impl_->stream);
        AAudioStream_close(impl_->stream);
        impl_->stream = nullptr;
    }
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, MonitorError> MonitorSink::start(const std::string& /*device_id*/,
                                                      std::uint32_t sample_rate,
                                                      std::uint16_t channels,
                                                      std::uint32_t /*channel_mask*/) {
    // device_id and channel_mask are accepted (the interface is shared with
    // the WASAPI/ALSA backends) but unused: AAudio has no way to name a
    // render endpoint by string id the way WASAPI does, and no channel-mask
    // input below API 32 - shared-mode output is Android's current system
    // route, and channel POSITION beyond a bare count is not something this
    // preview path needs (it exists to sanity-check levels/timing, not to
    // reproduce exact speaker placement).
    if (running()) {
        return std::unexpected(MonitorError::kAlreadyRunning);
    }
    if (channels == 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || builder == nullptr) {
        return std::unexpected(MonitorError::kComFailure);
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSampleRate(builder, static_cast<std::int32_t>(sample_rate));
    AAudioStreamBuilder_setChannelCount(builder, static_cast<std::int32_t>(channels));
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

    AAudioStream* stream = nullptr;
    const aaudio_result_t open_result = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (open_result != AAUDIO_OK || stream == nullptr) {
        return std::unexpected(MonitorError::kDeviceNotFound);
    }

    // Room for roughly a second of samples, so a caller decoding slightly
    // ahead of real time never has to spin - same sizing rationale as the
    // Windows/ALSA backends.
    impl_->queue =
        std::make_unique<capture::RingBuffer>(static_cast<std::size_t>(channels) * sample_rate);
    impl_->stream = stream;
    impl_->channels = channels;
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);

    if (AAudioStream_requestStart(stream) != AAUDIO_OK) {
        AAudioStream_close(stream);
        impl_->stream = nullptr;
        return std::unexpected(MonitorError::kComFailure);
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->worker_stop_requested.store(false, std::memory_order_relaxed);

    impl_->worker = std::thread([this, channels] {
        std::vector<float> chunk(static_cast<std::size_t>(kFramesPerWrite) * channels);
        while (!impl_->worker_stop_requested.load(std::memory_order_acquire)) {
            const auto got = impl_->queue->read(chunk);
            if (got < chunk.size()) {
                // Nothing queued: emit silence for the remainder, counted
                // rather than hidden, matching the other backends' underrun
                // discipline.
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(), 0.0f);
                impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            }
            const auto written = AAudioStream_write(impl_->stream, chunk.data(), kFramesPerWrite,
                                                     kWriteTimeoutNanos);
            if (written > 0) {
                impl_->rendered.fetch_add(static_cast<std::uint64_t>(written),
                                          std::memory_order_relaxed);
            }
        }
    });

    return {};
}

}  // namespace ac3::sinks
