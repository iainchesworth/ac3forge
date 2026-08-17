#include "ac3/sinks/monitor.hpp"

// The PipeWire monitor backend. CMake compiles this directory's monitor.cpp
// on a Linux host that selected pipewire/ over alsa/ (see
// src/audio/CMakeLists.txt) and another directory's everywhere else, so
// there is no #ifdef - the file's path is what says "PipeWire".
//
// This is the easy one of the three, and for the same reason it is easy on
// every other backend: a preview wants to share the output with everything
// else on the machine, so it can accept whatever conversion the graph
// offers instead of demanding the hardware exactly. An ordinary pw_stream
// output already IS that - PipeWire's own audioadapter resamples, remixes
// and mixes with everything else targeting the same sink - which is why
// this backend has no ALSA-style `plug` fallback: there is nothing a raw
// request could fail to negotiate the way a raw ALSA hw: device can.
// channels/sample_rate are requested exactly, and PipeWire gets them
// exactly, because the graph - not this backend - is doing the conversion.

#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "ac3/capture/ring_buffer.hpp"
#include "pipewire_support.hpp"

namespace ac3::sinks {

namespace {

using ac3::pipewire::Stream;
using ac3::pipewire::ThreadLoop;

// See capture.cpp's identical constant for why this bound exists at all.
constexpr int kConnectTimeoutSeconds = 5;

enum class ConnectState : int { kPending, kReady, kError };

bool wait_for_connect(pw_thread_loop* loop, std::atomic<ConnectState>& state) {
    for (int waited = 0; waited < kConnectTimeoutSeconds; ++waited) {
        if (state.load(std::memory_order_acquire) != ConnectState::kPending) {
            break;
        }
        pw_thread_loop_timed_wait(loop, 1);
    }
    return state.load(std::memory_order_acquire) == ConnectState::kReady;
}

}  // namespace

// Impl is private (see capture.cpp's identical note on why its callbacks are
// members of Impl rather than free functions - the same reasoning applies
// here).
struct MonitorSink::Impl {
    ThreadLoop loop;
    Stream stream;
    std::unique_ptr<capture::RingBuffer> queue;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
    std::uint16_t channels = 0;

    // See capture.cpp's Impl for the locking discipline this shares.
    std::atomic<ConnectState> connect_state{ConnectState::kPending};

    static void state_changed(void* data, pw_stream_state /*old_state*/, pw_stream_state state,
                               const char* /*error*/) {
        auto& impl = *static_cast<Impl*>(data);
        if (state == PW_STREAM_STATE_STREAMING || state == PW_STREAM_STATE_PAUSED) {
            auto expected = ConnectState::kPending;
            impl.connect_state.compare_exchange_strong(expected, ConnectState::kReady);
        } else if (state == PW_STREAM_STATE_ERROR) {
            impl.connect_state.store(ConnectState::kError, std::memory_order_release);
        }
    }

    static void process(void* data) {
        auto& impl = *static_cast<Impl*>(data);
        if (!impl.queue || impl.channels == 0) {
            return;
        }
        pw_buffer* buffer = pw_stream_dequeue_buffer(impl.stream.get());
        if (buffer == nullptr) {
            return;
        }
        spa_buffer* spa_buf = buffer->buffer;
        if (spa_buf->n_datas == 0 || spa_buf->datas[0].data == nullptr) {
            pw_stream_queue_buffer(impl.stream.get(), buffer);
            return;
        }

        const std::uint32_t stride = static_cast<std::uint32_t>(sizeof(float)) * impl.channels;
        const std::uint32_t requested =
            buffer->requested > 0 ? static_cast<std::uint32_t>(buffer->requested)
                                   : spa_buf->datas[0].maxsize / stride;
        const std::size_t sample_count = static_cast<std::size_t>(requested) * impl.channels;

        auto* out = static_cast<float*>(spa_buf->datas[0].data);
        const auto got = impl.queue->read(std::span{out, sample_count});
        if (got < sample_count) {
            std::fill(out + got, out + sample_count, 0.0f);
            impl.underruns.fetch_add(1, std::memory_order_relaxed);
        }
        impl.rendered.fetch_add(got / impl.channels, std::memory_order_relaxed);

        spa_buf->datas[0].chunk->offset = 0;
        spa_buf->datas[0].chunk->stride = static_cast<std::int32_t>(stride);
        spa_buf->datas[0].chunk->size = static_cast<std::uint32_t>(sample_count) * sizeof(float);

        pw_stream_queue_buffer(impl.stream.get(), buffer);
    }

    static const pw_stream_events& stream_events() {
        static const pw_stream_events events = [] {
            pw_stream_events value{};
            value.version = PW_VERSION_STREAM_EVENTS;
            value.state_changed = &Impl::state_changed;
            value.process = &Impl::process;
            return value;
        }();
        return events;
    }
};

std::string_view describe(MonitorError error) {
    switch (error) {
        case MonitorError::kNoBackend: return "no monitor backend on this platform";
        case MonitorError::kComFailure: return "a PipeWire call failed";
        case MonitorError::kDeviceNotFound:
            return "the requested playback device was not found (no such PipeWire node, or the "
                   "session manager refused to link it)";
        case MonitorError::kAlreadyRunning: return "monitor playback is already running";
        case MonitorError::kNotRunning: return "monitor playback is not running";
    }
    return "unknown monitor error";
}

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
    if (impl_->loop) {
        pw_thread_loop_stop(impl_->loop.get());
    }
    impl_->stream.reset();
    impl_->loop.reset();
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, MonitorError> MonitorSink::start(const std::string& device_id,
                                                      std::uint32_t sample_rate,
                                                      std::uint16_t channels,
                                                      std::uint32_t /*channel_mask*/) {
    // channel_mask is a WASAPI speaker mask with no PipeWire counterpart -
    // see the ALSA backend's identical note on this parameter. Accepted and
    // ignored rather than removed, since the header is shared.
    if (running()) {
        return std::unexpected(MonitorError::kAlreadyRunning);
    }
    if (channels == 0) {
        return std::unexpected(MonitorError::kComFailure);
    }

    ac3::pipewire::ensure_initialized();

    impl_->loop = ThreadLoop{pw_thread_loop_new("ac3audio-monitor", nullptr)};
    if (!impl_->loop) {
        return std::unexpected(MonitorError::kComFailure);
    }
    if (pw_thread_loop_start(impl_->loop.get()) < 0) {
        impl_->loop.reset();
        return std::unexpected(MonitorError::kComFailure);
    }

    pw_thread_loop_lock(impl_->loop.get());

    // See capture.cpp's start() for why this all happens before connect():
    // the earliest Impl::process()/Impl::state_changed() can run is once connect()
    // releases the lock, so Impl is always complete by then.
    impl_->queue =
        std::make_unique<capture::RingBuffer>(static_cast<std::size_t>(channels) * sample_rate);
    impl_->channels = channels;
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->connect_state.store(ConnectState::kPending, std::memory_order_relaxed);

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                                              "Playback", PW_KEY_MEDIA_ROLE, "Production", nullptr);
    if (!device_id.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, device_id.c_str());
    }

    Stream stream{pw_stream_new_simple(pw_thread_loop_get_loop(impl_->loop.get()),
                                        "ac3forge monitor", props, &Impl::stream_events(),
                                        impl_.get())};
    if (!stream) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(MonitorError::kComFailure);
    }

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = channels;
    info.rate = sample_rate;

    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    // The function, not the SPA_POD_BUILDER_INIT macro - see capture.cpp's
    // identical call for why.
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
    const spa_pod* params[1] = {param};

    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                      PW_STREAM_FLAG_MAP_BUFFERS |
                                                      PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        pw_thread_loop_unlock(impl_->loop.get());
        stream.reset();
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(MonitorError::kComFailure);
    }

    impl_->stream = std::move(stream);
    const bool ready = wait_for_connect(impl_->loop.get(), impl_->connect_state);

    if (!ready) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->stream.reset();
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(MonitorError::kDeviceNotFound);
    }

    pw_thread_loop_unlock(impl_->loop.get());

    impl_->running.store(true, std::memory_order_release);
    return {};
}

}  // namespace ac3::sinks
