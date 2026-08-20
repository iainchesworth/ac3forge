#include "ac3/audio/capture.hpp"

// The PipeWire capture backend. CMake compiles this directory's capture.cpp
// on a Linux host that selected pipewire/ over alsa/ (see
// src/audio/CMakeLists.txt for the precedence between them) and another
// directory's everywhere else, so there is no #ifdef here - the file's path
// is what says "PipeWire".
//
// Every stream here is native pw_stream, not PipeWire's ALSA-compatibility
// shim (`pipewire-alsa`, which would make this file indistinguishable from
// the alsa/ backend it sits beside) - format negotiation, buffer exchange
// and the connection lifecycle all go through libpipewire-0.3 directly.
//
// Unlike ALSA's raw hw: devices, a PipeWire node negotiates through its own
// audioadapter, which resamples and remixes on the node's behalf - so where
// ac3::alsa::choose_format() and its _near negotiation exist because a raw
// device offers what the hardware can do and nothing else, this backend
// simply asks for float32 at the preferred rate and channel count and gets
// it, almost always exactly. What it does NOT get from the graph is a
// device's native rate or channel count merely by looking at it: a
// PipeWire node has no single fixed format the way an ALSA hw: device
// does, so enumerate_devices() reports 0/0 for both (DeviceInfo's own
// documented meaning for "not established yet") and the real answer comes
// back from the negotiation start() actually runs.
//
// Loopback - recording what the machine is playing - has a real, documented
// native mechanism here, unlike ALSA (which has none of its own and falls
// back to the optional snd-aloop kernel module): the `stream.capture.sink`
// property, pointed at a target Audio/Sink node via PW_KEY_TARGET_OBJECT,
// asks the session manager to route that sink's monitor to this capture
// stream instead of a real input. Every enumerated Audio/Sink node is
// offered as a kLoopback capture device on that basis.
//
// PipeWire's graph is quantum-clocked rather than interrupt-driven: once
// something is linked, every active node processes on every cycle whether
// or not there is a real signal, so a sink monitor ordinarily delivers
// continuous silence rather than nothing at all while the sink is idle -
// unlike ALSA's snd-aloop, which (like a WASAPI loopback endpoint) delivers
// nothing until something plays. The one real gap this leaves is a sink
// that has suspended itself after an idle timeout
// (PW_KEY_NODE_SUSPEND_ON_IDLE, common in the shipped session-manager
// configuration): the same timeline gap-fill ALSA's backend uses for
// snd-aloop is kept here too, so a suspended sink's silence is synthesised
// rather than the recording quietly running short.

#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "pipewire_support.hpp"

namespace ac3::audio {

namespace {

using ac3::pipewire::Stream;
using ac3::pipewire::ThreadLoop;

constexpr std::uint32_t kPreferredRate = 48000;
constexpr std::uint32_t kPreferredChannels = 2;

// How long start() waits for the stream to reach a terminal state
// (PAUSED/STREAMING on success, ERROR on failure) before giving up. PipeWire
// reports connection outcomes as events delivered asynchronously by the
// session manager, not a blocking call that returns once negotiated the way
// snd_pcm_open()/snd_pcm_hw_params() do, so unlike the ALSA backend this
// bound exists to keep start() itself synchronous rather than hanging
// forever on a session manager that never answers.
constexpr int kConnectTimeoutSeconds = 5;

enum class ConnectState : int { kPending, kReady, kError };

// Blocks (releasing and reacquiring the thread loop's lock, per
// pw_thread_loop_wait()'s own contract) until connect_state leaves kPending
// or the timeout elapses. Must be called with the loop already locked.
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

// Impl is private (declared in the shared ac3/audio/capture.hpp every
// backend implements), so the callbacks below - which need to reach it from
// a bare `void*` - are members of Impl itself rather than free functions:
// naming `Capture::Impl` from outside Capture's or Impl's own members is an
// access violation the moment it is more than this struct's own out-of-line
// definition, which is the one exemption every pimpl class relies on.
struct Capture::Impl {
    ThreadLoop loop;
    Stream stream;
    std::unique_ptr<RingBuffer> ring;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> frames_captured{0};
    std::atomic<std::uint64_t> frames_silence{0};
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::chrono::steady_clock::time_point started;
    std::uint64_t timeline_frames = 0;
    bool loopback = false;

    // Touched only while the thread loop's lock is held: connect_state by
    // both sides (start() polls it via wait_for_connect(), which itself only
    // returns with the lock re-acquired; state_changed below sets it, and
    // every stream callback runs with the lock already held, per
    // pw_thread_loop's own contract).
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
        if (!impl.ring) {
            return;  // connect() has not finished setting up yet
        }
        pw_buffer* buffer = pw_stream_dequeue_buffer(impl.stream.get());
        if (buffer == nullptr) {
            return;
        }
        const spa_buffer* spa_buf = buffer->buffer;
        if (spa_buf->n_datas > 0 && spa_buf->datas[0].data != nullptr && impl.channels > 0) {
            const auto* samples = static_cast<const float*>(spa_buf->datas[0].data);
            const auto raw_stride = spa_buf->datas[0].chunk->stride;
            const std::uint32_t stride =
                raw_stride > 0 ? static_cast<std::uint32_t>(raw_stride)
                               : static_cast<std::uint32_t>(sizeof(float)) * impl.channels;
            const std::size_t frame_count = spa_buf->datas[0].chunk->size / stride;
            const std::size_t sample_count = frame_count * impl.channels;

            impl.ring->write(std::span{samples, sample_count});
            impl.frames_captured.fetch_add(frame_count, std::memory_order_relaxed);
            impl.timeline_frames += frame_count;
        }
        pw_stream_queue_buffer(impl.stream.get(), buffer);

        if (!impl.loopback || impl.channels == 0) {
            return;
        }
        // Cover a suspended sink's silence the same way ac3::alsa's
        // snd-aloop path does - see this file's header comment.
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                             impl.started)
                                  .count();
        const auto elapsed_frames =
            static_cast<std::uint64_t>(elapsed * static_cast<double>(impl.sample_rate));
        if (elapsed_frames > impl.timeline_frames + impl.sample_rate / 100) {
            auto missing = elapsed_frames - impl.timeline_frames;
            missing = std::min<std::uint64_t>(missing, impl.sample_rate);
            std::vector<float> silence(static_cast<std::size_t>(missing) * impl.channels, 0.0f);
            impl.ring->write(silence);
            impl.frames_silence.fetch_add(missing, std::memory_order_relaxed);
            impl.timeline_frames += missing;
        }
    }

    static const pw_stream_events& stream_events() {
        // Function-local static: constructed once, lives for the process,
        // so the pointer pw_stream_new_simple stores stays valid for every
        // stream this backend ever creates.
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

std::string_view describe(CaptureError error) {
    switch (error) {
        case CaptureError::kNoBackend: return "no capture backend on this platform";
        case CaptureError::kComFailure: return "a PipeWire call failed";
        case CaptureError::kDeviceNotFound:
            return "the requested capture device was not found (no such PipeWire node, or the "
                   "session manager refused to link it)";
        case CaptureError::kFormatUnsupported:
            return "the device offers no sample format this backend can read";
        case CaptureError::kAlreadyRunning: return "capture is already running";
    }
    return "unknown capture error";
}

std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices() {
    std::vector<DeviceInfo> devices;

    ac3::pipewire::for_each_audio_node([&devices](std::uint32_t /*id*/, const spa_dict& props) {
        const bool is_source = ac3::pipewire::is_audio_source(props);
        const bool is_sink = ac3::pipewire::is_audio_sink(props);
        if (!is_source && !is_sink) {
            return;
        }
        DeviceInfo info{
            .id = ac3::pipewire::node_id(props),
            .name = ac3::pipewire::node_friendly_name(props),
            .kind = is_sink ? DeviceKind::kLoopback : DeviceKind::kInput,
            .sample_rate = 0,
            .channels = 0,
            .is_default = false,
        };
        if (info.id.empty()) {
            return;  // no node.name to target - not something a caller could open anyway
        }
        if (is_sink) {
            info.name += " (loopback)";
        }
        devices.push_back(std::move(info));
    });

    // No per-node "this is the default" metadata is read here (see
    // pipewire_support.hpp's header for why); the same honest fallback ALSA's
    // own enumerate_devices() falls back to when it cannot resolve one either
    // - the first entry found is as good a default as exists.
    if (!devices.empty()) {
        devices.front().is_default = true;
    }

    return devices;
}

Capture::Capture() : impl_(std::make_unique<Impl>()) {}

Capture::~Capture() {
    stop();
}

bool Capture::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

std::uint32_t Capture::sample_rate() const {
    return impl_->sample_rate;
}

std::uint16_t Capture::channels() const {
    return impl_->channels;
}

CaptureStats Capture::stats() const {
    return {.frames_captured = impl_->frames_captured.load(std::memory_order_relaxed),
            .frames_silence_filled = impl_->frames_silence.load(std::memory_order_relaxed),
            .frames_dropped = impl_->ring ? impl_->ring->dropped() /
                                                std::max<std::size_t>(impl_->channels, 1)
                                          : 0};
}

RingBuffer* Capture::buffer() {
    return impl_->ring.get();
}

void Capture::stop() {
    if (impl_->loop) {
        pw_thread_loop_stop(impl_->loop.get());
    }
    impl_->stream.reset();
    impl_->loop.reset();
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, CaptureError> Capture::start(const std::string& device_id, DeviceKind kind,
                                                  std::size_t ring_capacity_samples) {
    if (running()) {
        return std::unexpected(CaptureError::kAlreadyRunning);
    }

    ac3::pipewire::ensure_initialized();

    impl_->loop = ThreadLoop{pw_thread_loop_new("ac3audio-capture", nullptr)};
    if (!impl_->loop) {
        return std::unexpected(CaptureError::kComFailure);
    }
    if (pw_thread_loop_start(impl_->loop.get()) < 0) {
        impl_->loop.reset();
        return std::unexpected(CaptureError::kComFailure);
    }

    pw_thread_loop_lock(impl_->loop.get());

    // Everything Impl::process()/Impl::state_changed() might touch is set up
    // before pw_stream_connect() below, while the loop's lock is still held
    // by this thread - the earliest either callback can run is once that
    // call releases the lock (inside pw_thread_loop_timed_wait()), so there
    // is no window where they see a half-constructed Impl.
    const bool loopback = kind == DeviceKind::kLoopback;
    impl_->ring = std::make_unique<RingBuffer>(ring_capacity_samples);
    impl_->sample_rate = kPreferredRate;
    impl_->channels = static_cast<std::uint16_t>(kPreferredChannels);
    impl_->frames_captured.store(0, std::memory_order_relaxed);
    impl_->frames_silence.store(0, std::memory_order_relaxed);
    impl_->started = std::chrono::steady_clock::now();
    impl_->timeline_frames = 0;
    impl_->loopback = loopback;
    impl_->connect_state.store(ConnectState::kPending, std::memory_order_relaxed);

    pw_properties* props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                                              "Capture", PW_KEY_MEDIA_ROLE, "Production", nullptr);
    if (!device_id.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, device_id.c_str());
    }
    if (loopback) {
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    }

    Stream stream{pw_stream_new_simple(pw_thread_loop_get_loop(impl_->loop.get()),
                                        "ac3forge capture", props, &Impl::stream_events(),
                                        impl_.get())};
    if (!stream) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->ring.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(CaptureError::kComFailure);
    }

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = kPreferredChannels;
    info.rate = kPreferredRate;

    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    // The function, not the SPA_POD_BUILDER_INIT macro it wraps: the macro
    // expands to a C99 compound literal, which -isystem exempts inside
    // PipeWire's own header but would not exempt if expanded directly here
    // under -Werror -Wc99-extensions (Clang, not GCC, treats it as an
    // extension worth flagging).
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
    const spa_pod* params[1] = {param};

    const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                      PW_STREAM_FLAG_MAP_BUFFERS |
                                                      PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(stream.get(), PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1) < 0) {
        pw_thread_loop_unlock(impl_->loop.get());
        stream.reset();
        impl_->ring.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(CaptureError::kComFailure);
    }

    impl_->stream = std::move(stream);
    const bool ready = wait_for_connect(impl_->loop.get(), impl_->connect_state);

    if (!ready) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->stream.reset();
        impl_->ring.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(CaptureError::kDeviceNotFound);
    }

    pw_thread_loop_unlock(impl_->loop.get());

    impl_->running.store(true, std::memory_order_release);
    return {};
}

}  // namespace ac3::audio
