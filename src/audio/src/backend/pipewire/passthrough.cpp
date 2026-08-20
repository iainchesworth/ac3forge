#include "ac3/audio/passthrough.hpp"

// The PipeWire passthrough backend. CMake compiles this directory's
// passthrough.cpp on a Linux host that selected pipewire/ over alsa/ (see
// src/audio/CMakeLists.txt) and another directory's everywhere else, so
// there is no #ifdef - the file's path is what says "PipeWire".
//
// ---------------------------------------------------------------------------
// PipeWire's native compressed-format path is real, and this is it
// ---------------------------------------------------------------------------
// PipeWire's own SPA headers (spa/param/audio/iec958.h, current as of
// libpipewire-0.3 1.6.x) define exactly what this backend needs:
// `enum spa_audio_iec958_codec` with SPA_AUDIO_IEC958_CODEC_AC3 and _EAC3
// members, `struct spa_audio_info_iec958 { codec; flags; rate; }`, and
// spa_format_audio_iec958_build()/_parse() to turn one into a
// SPA_MEDIA_SUBTYPE_iec958 format POD. Connecting with PW_STREAM_FLAG_
// EXCLUSIVE and that POD is a real, shipped mechanism - not aspirational
// API surface - confirmed against Kodi's own PipeWire passthrough
// implementation (xbmc PR #22560), which negotiates exactly this way and
// still hands PipeWire pre-packed IEC 61937 burst bytes as opaque S16
// carrier data, the same shape ac3::iec61937 already produces for the ALSA
// backend.
//
// ---------------------------------------------------------------------------
// The real gap, and why enumeration has to probe rather than ask
// ---------------------------------------------------------------------------
// Whether the format POD above is ACCEPTED is a per-node, per-deployment
// question this library cannot answer by reading a static property: an
// ALSA-backed PipeWire sink only offers a compressed codec once its
// `iec958Codecs` control has been explicitly populated, which is session-
// manager configuration (a WirePlumber ALSA-monitor rule, or a one-off
// `pw-cli s <id> Props '{ iec958Codecs: [ AC3 EAC3 ] }'`) that this library
// has no portable way to set on the user's behalf - unlike ALSA's own
// `iec958:CARD=...` device names, which just work unconditionally the
// moment the hardware exists. So on a stock desktop where nobody has ever
// touched that WirePlumber setting, every PipeWire sink genuinely, honestly
// offers no compressed codec at all, and this backend has no way to turn
// that on. That is the real, current shape of the gap, not an excuse for an
// incomplete implementation: enumerate_render_devices() below does not
// pretend to know the answer by inspecting a property (this library never
// found a documented, stable one to read) - it asks, the same way ALSA's own
// enumerate_render_devices() has no IsFormatSupported() and has to open each
// candidate to find out. The cost is the same kind ALSA already accepts:
// probing is intrusive (it briefly takes each candidate exclusively) and
// slower than a property read would be.
//
// Underneath, a PipeWire sink that DOES accept the format still reaches the
// hardware through the very same ALSA `iec958:`/`hdmi:` device the alsa/
// backend would open directly - see docs/building.md's "Why ALSA and not
// PipeWire" section for why ALSA stays the preferred backend when both are
// available, and CMakeLists.txt for the precedence itself.

#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ac3/audio/ring_buffer.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "pipewire_support.hpp"

namespace ac3::audio {

namespace {

using ac3::pipewire::Stream;
using ac3::pipewire::ThreadLoop;

// The carrier is a 2-channel 16-bit stream whatever rides inside it - see
// ac3::pipewire::kCarrierFrameBytes and the ALSA backend's identical
// constant and comment.
constexpr std::size_t kCarrierFrameBytes = ac3::pipewire::kCarrierFrameBytes;

// A real connection attempt (start()) is allowed longer to settle than a
// probe made purely to answer an enumerate_render_devices() question -
// enumeration may run several of these per candidate output.
constexpr int kConnectTimeoutSeconds = 5;
constexpr int kProbeTimeoutSeconds = 2;

std::size_t burst_bytes_for(BitstreamFormat format) {
    return format == BitstreamFormat::kEac3 ? iec61937::kEac3BurstBytes : iec61937::kBurstBytes;
}

enum class ConnectState : int { kPending, kReady, kError };

bool wait_for_connect(pw_thread_loop* loop, std::atomic<ConnectState>& state, int timeout_seconds) {
    for (int waited = 0; waited < timeout_seconds; ++waited) {
        if (state.load(std::memory_order_acquire) != ConnectState::kPending) {
            break;
        }
        pw_thread_loop_timed_wait(loop, 1);
    }
    return state.load(std::memory_order_acquire) == ConnectState::kReady;
}

// The probe-only state callback: enumeration and the empty-device_id
// candidate walk in start() both only need "did it settle", not the full
// process()-wiring PassthroughSink::Impl carries - a plain atomic is enough
// data for this one.
void on_probe_state_changed(void* data, pw_stream_state /*old_state*/, pw_stream_state state,
                             const char* /*error*/) {
    auto& connect_state = *static_cast<std::atomic<ConnectState>*>(data);
    if (state == PW_STREAM_STATE_STREAMING || state == PW_STREAM_STATE_PAUSED) {
        auto expected = ConnectState::kPending;
        connect_state.compare_exchange_strong(expected, ConnectState::kReady);
    } else if (state == PW_STREAM_STATE_ERROR) {
        connect_state.store(ConnectState::kError, std::memory_order_release);
    }
}

const pw_stream_events& probe_events() {
    static const pw_stream_events events = [] {
        pw_stream_events value{};
        value.version = PW_VERSION_STREAM_EVENTS;
        value.state_changed = on_probe_state_changed;
        return value;
    }();
    return events;
}

// Attempts a real, exclusive connection to `node_id` with `params` and tears
// it down immediately whatever the outcome - see this file's header comment
// for why a real connection attempt, not a property read, is the only
// honest way to answer "will this output take this format". Shared by
// enumerate_render_devices()'s three probes per candidate and start()'s
// empty-device_id candidate walk, which is exactly the same question asked
// with a different format POD each time.
bool probe_connect(const std::string& node_id, const spa_pod** params, std::uint32_t n_params,
                    int timeout_seconds) {
    ThreadLoop loop{pw_thread_loop_new("ac3audio-probe", nullptr)};
    if (!loop || pw_thread_loop_start(loop.get()) < 0) {
        return false;
    }

    pw_thread_loop_lock(loop.get());
    std::atomic<ConnectState> state{ConnectState::kPending};

    pw_properties* props =
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                           PW_KEY_MEDIA_ROLE, "Production", PW_KEY_TARGET_OBJECT, node_id.c_str(),
                           nullptr);

    Stream stream{pw_stream_new_simple(pw_thread_loop_get_loop(loop.get()), "ac3forge probe",
                                        props, &probe_events(), &state)};
    if (!stream) {
        pw_thread_loop_unlock(loop.get());
        pw_thread_loop_stop(loop.get());
        return false;
    }

    const auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS);
    if (pw_stream_connect(stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, n_params) <
        0) {
        pw_thread_loop_unlock(loop.get());
        stream.reset();
        pw_thread_loop_stop(loop.get());
        return false;
    }

    const bool ready = wait_for_connect(loop.get(), state, timeout_seconds);
    pw_thread_loop_unlock(loop.get());
    stream.reset();
    pw_thread_loop_stop(loop.get());
    return ready;
}

const spa_pod* build_iec958_pod(spa_pod_builder& builder, BitstreamFormat format,
                                 std::uint32_t carrier) {
    spa_audio_info_iec958 info{};
    info.codec = ac3::pipewire::iec958_codec_for(format);
    info.rate = carrier;
    return spa_format_audio_iec958_build(&builder, SPA_PARAM_EnumFormat, &info);
}

bool probe_iec958(const std::string& node_id, BitstreamFormat format, std::uint32_t sample_rate) {
    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    // The function, not the SPA_POD_BUILDER_INIT macro - see capture.cpp's
    // identical call for why.
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param =
        build_iec958_pod(builder, format, ac3::pipewire::carrier_rate(format, sample_rate));
    const spa_pod* params[1] = {param};
    return probe_connect(node_id, params, 1, kProbeTimeoutSeconds);
}

// The PipeWire analogue of ALSA's supports_exclusive_pcm control probe:
// PipeWire exposes no separate "raw hardware, no plugin chain" name the way
// ALSA's hw: does, so this asks the SAME node whether it will take plain
// stereo PCM exclusively, which still separates the two reasons passthrough
// can fail: a node that refuses even this has exclusive mode unavailable
// (in use, or disabled), while one that takes this but not either codec
// above simply has no compressed codec enabled.
bool probe_exclusive_pcm(const std::string& node_id, std::uint32_t sample_rate) {
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_S16_LE;
    info.channels = 2;
    info.rate = sample_rate;

    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
    const spa_pod* params[1] = {param};
    return probe_connect(node_id, params, 1, kProbeTimeoutSeconds);
}

std::vector<std::pair<std::string, std::string>> candidate_sinks() {
    std::vector<std::pair<std::string, std::string>> candidates;  // (id, friendly name)
    ac3::pipewire::for_each_audio_node([&candidates](std::uint32_t, const spa_dict& props) {
        if (!ac3::pipewire::is_audio_sink(props)) {
            return;
        }
        auto id = ac3::pipewire::node_id(props);
        if (id.empty()) {
            return;
        }
        candidates.emplace_back(std::move(id), ac3::pipewire::node_friendly_name(props));
    });
    return candidates;
}

}  // namespace

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure: return "a PipeWire call failed";
        case PassthroughError::kDeviceNotFound:
            return "no such output: either the named PipeWire node does not exist, or none was "
                   "named and this machine has no Audio/Sink node at all";
        case PassthroughError::kFormatRejected:
            return "no output accepted this bitstream over IEC 61937 - most PipeWire sinks have "
                   "no compressed codec enabled by default; see this file's own header comment "
                   "for the `iec958Codecs` session-manager setting that turns one on";
        case PassthroughError::kExclusiveUnavailable:
            return "the node could not be reached exclusively (another client holds it, or this "
                   "user lacks permission on the underlying device)";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t sample_rate) {
    std::vector<RenderDeviceInfo> devices;

    for (const auto& [id, name] : candidate_sinks()) {
        RenderDeviceInfo info{
            .id = id,
            .name = name,
            .is_default = false,
            .supports_ac3_passthrough = probe_iec958(id, BitstreamFormat::kAc3, sample_rate),
            .supports_eac3_passthrough = probe_iec958(id, BitstreamFormat::kEac3, sample_rate),
            .supports_exclusive_pcm = probe_exclusive_pcm(id, sample_rate),
        };
        devices.push_back(std::move(info));
    }

    // No per-node "this is the default" metadata is read here - the same
    // fallback ac3::audio's PipeWire enumeration and ALSA's own passthrough
    // enumeration both use when they cannot resolve one either.
    if (!devices.empty()) {
        devices.front().is_default = true;
    }

    return devices;
}

// Impl is private (see capture.cpp's identical note on why its callbacks are
// members of Impl rather than free functions - the same reasoning applies
// here).
struct PassthroughSink::Impl {
    ThreadLoop loop;
    Stream stream;
    std::unique_ptr<ByteRingBuffer> queue;
    std::size_t burst_bytes = iec61937::kBurstBytes;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};

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
        if (!impl.queue) {
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

        const std::uint32_t requested =
            buffer->requested > 0 ? static_cast<std::uint32_t>(buffer->requested) *
                                         static_cast<std::uint32_t>(kCarrierFrameBytes)
                                   : spa_buf->datas[0].maxsize;
        const std::size_t byte_count = std::min<std::size_t>(requested, spa_buf->datas[0].maxsize);

        auto* out = static_cast<std::byte*>(spa_buf->datas[0].data);
        const auto got = impl.queue->read(std::span{out, byte_count});
        if (got < byte_count) {
            // A gap on the wire, exactly as underrun handling is described
            // in ac3::audio::PassthroughStats's own documentation - counted,
            // not hidden, matching every other backend's discipline.
            std::fill(out + got, out + byte_count, std::byte{0});
            impl.underruns.fetch_add(1, std::memory_order_relaxed);
        }
        impl.rendered.fetch_add(got / impl.burst_bytes, std::memory_order_relaxed);

        spa_buf->datas[0].chunk->offset = 0;
        spa_buf->datas[0].chunk->stride = static_cast<std::int32_t>(kCarrierFrameBytes);
        spa_buf->datas[0].chunk->size = static_cast<std::uint32_t>(byte_count);

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

PassthroughSink::PassthroughSink() : impl_(std::make_unique<Impl>()) {}

PassthroughSink::~PassthroughSink() {
    stop();
}

bool PassthroughSink::running() const {
    return impl_->running.load(std::memory_order_acquire);
}

PassthroughStats PassthroughSink::stats() const {
    return {.bursts_submitted = impl_->submitted.load(std::memory_order_relaxed),
            .bursts_rendered = impl_->rendered.load(std::memory_order_relaxed),
            .underruns = impl_->underruns.load(std::memory_order_relaxed)};
}

bool PassthroughSink::can_submit() const {
    if (!impl_->queue) {
        return false;
    }
    return impl_->queue->capacity() - impl_->queue->available() > impl_->burst_bytes;
}

bool PassthroughSink::submit(std::span<const std::byte> burst) {
    if (!running() || !impl_->queue || burst.size() != impl_->burst_bytes) {
        return false;
    }
    if (!can_submit()) {
        return false;
    }
    const auto wrote = impl_->queue->write(burst);
    if (wrote != burst.size()) {
        return false;
    }
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PassthroughSink::stop() {
    if (impl_->loop) {
        pw_thread_loop_stop(impl_->loop.get());
    }
    impl_->stream.reset();
    impl_->loop.reset();
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, PassthroughError> PassthroughSink::start(const std::string& device_id,
                                                              std::uint32_t sample_rate,
                                                              BitstreamFormat format_kind) {
    if (running()) {
        return std::unexpected(PassthroughError::kAlreadyRunning);
    }

    ac3::pipewire::ensure_initialized();

    // Pick the target before touching anything, exactly as the ALSA backend
    // does: an empty id means "the default output", which for a bitstream
    // can only mean a sink that has already said yes - see this file's
    // header comment for why that has to be a real connection attempt.
    std::string target = device_id;
    if (target.empty()) {
        const auto candidates = candidate_sinks();
        if (candidates.empty()) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
        for (const auto& [id, name] : candidates) {
            if (probe_iec958(id, format_kind, sample_rate)) {
                target = id;
                break;
            }
        }
        if (target.empty()) {
            return std::unexpected(PassthroughError::kFormatRejected);
        }
    }

    const std::uint32_t carrier = ac3::pipewire::carrier_rate(format_kind, sample_rate);
    const std::size_t burst_bytes = burst_bytes_for(format_kind);

    impl_->loop = ThreadLoop{pw_thread_loop_new("ac3audio-passthrough", nullptr)};
    if (!impl_->loop) {
        return std::unexpected(PassthroughError::kComFailure);
    }
    if (pw_thread_loop_start(impl_->loop.get()) < 0) {
        impl_->loop.reset();
        return std::unexpected(PassthroughError::kComFailure);
    }

    pw_thread_loop_lock(impl_->loop.get());

    // Everything Impl::process()/Impl::state_changed() might touch is set up
    // before pw_stream_connect() below - see capture.cpp's start() for why
    // that ordering is what keeps this race-free.
    impl_->queue = std::make_unique<ByteRingBuffer>(burst_bytes * 40);
    impl_->burst_bytes = burst_bytes;
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->connect_state.store(ConnectState::kPending, std::memory_order_relaxed);

    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE,
        "Production", PW_KEY_TARGET_OBJECT, target.c_str(), nullptr);

    Stream stream{pw_stream_new_simple(pw_thread_loop_get_loop(impl_->loop.get()),
                                        "ac3forge passthrough", props, &Impl::stream_events(),
                                        impl_.get())};
    if (!stream) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(PassthroughError::kComFailure);
    }

    std::array<std::uint8_t, 1024> pod_buffer{};
    spa_pod_builder builder{};
    spa_pod_builder_init(&builder, pod_buffer.data(), static_cast<std::uint32_t>(pod_buffer.size()));
    const spa_pod* param = build_iec958_pod(builder, format_kind, carrier);
    const spa_pod* params[1] = {param};

    const auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_EXCLUSIVE | PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
        PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(stream.get(), PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1) < 0) {
        pw_thread_loop_unlock(impl_->loop.get());
        stream.reset();
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        return std::unexpected(PassthroughError::kComFailure);
    }

    impl_->stream = std::move(stream);
    const bool ready =
        wait_for_connect(impl_->loop.get(), impl_->connect_state, kConnectTimeoutSeconds);

    if (!ready) {
        pw_thread_loop_unlock(impl_->loop.get());
        impl_->stream.reset();
        impl_->queue.reset();
        pw_thread_loop_stop(impl_->loop.get());
        impl_->loop.reset();
        // PipeWire reports a failed negotiation as a state plus a human-
        // readable string, not an errno the way ALSA does (see this file's
        // header comment), so - unlike ac3::alsa::open_failure() - this
        // cannot reliably tell "codec not enabled" apart from "node busy"
        // from that string alone. kFormatRejected is the more common real
        // cause (see the header comment) and the more actionable one to
        // report by default.
        return std::unexpected(PassthroughError::kFormatRejected);
    }

    pw_thread_loop_unlock(impl_->loop.get());

    impl_->running.store(true, std::memory_order_release);
    return {};
}

}  // namespace ac3::audio
