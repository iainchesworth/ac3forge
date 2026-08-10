#include "ac3/capture/capture.hpp"

// The ALSA capture backend. CMake compiles this directory's capture.cpp on a
// Linux host whose libasound development headers are present and another
// directory's everywhere else, so there is no #ifdef here - the file's path is
// what says "ALSA", and src/lib/CMakeLists.txt is where the choice is made.
//
// Where WASAPI enumerates endpoints the audio engine has already opened and
// mixed, ALSA enumerates the hardware itself, so the two backends answer the
// same questions from opposite ends:
//
//   * "the default device" is a name, `default`, not a system setting. It is
//     the first entry offered, and on a desktop it is where PipeWire or
//     PulseAudio have installed themselves - so recording from it records
//     through whatever sound server is running, without this file knowing one
//     exists.
//   * every other entry is a real hw: device, which is exclusive: opening it
//     succeeds only while nothing else holds it.
//   * a device_id is passed to snd_pcm_open() verbatim. Anything alsa-lib
//     understands therefore works, including names this enumeration never
//     offers - `plughw:1,0`, or `pulse:DEVICE=<sink>.monitor` to record a
//     PulseAudio/PipeWire monitor source.
//
// Loopback - recording what the machine is playing - is the one capability
// that does not survive the crossing. WASAPI opens any render endpoint in
// loopback mode; ALSA has no equivalent, because there is no mixer to tap.
// What it has is snd-aloop, an optional kernel module presenting a "Loopback"
// card whose capture side carries whatever was played to its playback side, so
// that card's devices are reported as kLoopback and everything else as kInput.
// A machine without snd-aloop loaded reports no loopback devices at all, and
// that is the honest answer rather than a failure.

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <thread>

#include "alsa_support.hpp"
#include "device_names.hpp"

namespace ac3::capture {

namespace {

using alsa::HwParams;
using alsa::Pcm;
using alsa::SampleFormat;
using alsa::SwParams;

// What we ask for. Every one is negotiable: ALSA's _near setters move to the
// closest value the device supports and report back what they chose, and the
// caller is told the result through sample_rate() and channels().
constexpr unsigned kPreferredRate = 48000;
constexpr unsigned kPreferredChannels = 2;
constexpr snd_pcm_uframes_t kPreferredPeriod = 1024;
constexpr unsigned kPeriodsPerBuffer = 4;

// How long the capture thread blocks before looking at its stop token again.
constexpr int kWaitMs = 100;

// snd-aloop names its card this. Matching on the id rather than the driver
// string because the id is what a user types into a device name.
constexpr std::string_view kLoopbackCardId = "Loopback";

// Convert one period into normalised float, in place into `out`.
void convert(const std::byte* data, std::size_t sample_count, SampleFormat format,
             std::vector<float>& out) {
    out.resize(sample_count);
    switch (format) {
        case SampleFormat::kFloat32: {
            std::memcpy(out.data(), data, sample_count * sizeof(float));
            break;
        }
        case SampleFormat::kPcm16: {
            for (std::size_t i = 0; i < sample_count; ++i) {
                std::int16_t value = 0;
                std::memcpy(&value, data + i * 2, sizeof(value));
                out[i] = static_cast<float>(value) / 32768.0f;
            }
            break;
        }
        case SampleFormat::kPcm24Packed: {
            for (std::size_t i = 0; i < sample_count; ++i) {
                const std::byte* p = data + i * 3;
                // Little-endian 24-bit, shifted to the top of a 32-bit word so
                // one divisor covers both integer widths.
                const auto value = static_cast<std::int32_t>(
                    (static_cast<std::uint32_t>(p[0]) << 8) |
                    (static_cast<std::uint32_t>(p[1]) << 16) |
                    (static_cast<std::uint32_t>(p[2]) << 24));
                out[i] = static_cast<float>(value) / 2147483648.0f;
            }
            break;
        }
        case SampleFormat::kPcm32: {
            for (std::size_t i = 0; i < sample_count; ++i) {
                std::int32_t value = 0;
                std::memcpy(&value, data + i * 4, sizeof(value));
                out[i] = static_cast<float>(value) / 2147483648.0f;
            }
            break;
        }
    }
}

// What a negotiation settled on.
struct Negotiated {
    SampleFormat format = SampleFormat::kFloat32;
    std::size_t bytes_per_sample = 4;
    unsigned rate = 0;
    unsigned channels = 0;
    snd_pcm_uframes_t period = 0;
};

// Fill in the hardware parameters for an open capture device, committing them
// unless `probe_only` - enumeration wants the numbers without disturbing a
// device someone else may be about to use.
std::expected<Negotiated, CaptureError> negotiate(snd_pcm_t* pcm, bool probe_only) {
    HwParams params;
    if (!params || snd_pcm_hw_params_any(pcm, params.get()) < 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }
    if (snd_pcm_hw_params_set_access(pcm, params.get(), SND_PCM_ACCESS_RW_INTERLEAVED) < 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }

    const auto format = alsa::choose_format(pcm, params.get());
    if (!format) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }
    Negotiated chosen;
    chosen.format = format->kind;
    chosen.bytes_per_sample = format->bytes;

    unsigned channels = kPreferredChannels;
    if (snd_pcm_hw_params_set_channels_near(pcm, params.get(), &channels) < 0 || channels == 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }
    unsigned rate = kPreferredRate;
    int direction = 0;
    if (snd_pcm_hw_params_set_rate_near(pcm, params.get(), &rate, &direction) < 0 || rate == 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }
    snd_pcm_uframes_t period = kPreferredPeriod;
    direction = 0;
    if (snd_pcm_hw_params_set_period_size_near(pcm, params.get(), &period, &direction) < 0 ||
        period == 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }
    snd_pcm_uframes_t buffer = period * kPeriodsPerBuffer;
    if (snd_pcm_hw_params_set_buffer_size_near(pcm, params.get(), &buffer) < 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }

    chosen.channels = channels;
    chosen.rate = rate;
    chosen.period = period;

    if (probe_only) {
        return chosen;
    }
    if (snd_pcm_hw_params(pcm, params.get()) < 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }

    // Software parameters: wake the reader once a period is in, and let the
    // first read start the stream. Both are alsa-lib's defaults for a capture
    // device; stating them makes snd_pcm_wait()'s behaviour below a property
    // of this file rather than of whichever alsa-lib is installed.
    SwParams software;
    if (software && snd_pcm_sw_params_current(pcm, software.get()) >= 0) {
        snd_pcm_sw_params_set_avail_min(pcm, software.get(), period);
        snd_pcm_sw_params_set_start_threshold(pcm, software.get(), 1);
        snd_pcm_sw_params(pcm, software.get());
    }
    return chosen;
}

}  // namespace

std::string_view describe(CaptureError error) {
    switch (error) {
        case CaptureError::kNoBackend: return "no capture backend on this platform";
        case CaptureError::kComFailure: return "an ALSA call failed";
        case CaptureError::kDeviceNotFound:
            return "the requested capture device was not found (no such ALSA PCM, or it is in "
                   "use by another application)";
        case CaptureError::kFormatUnsupported:
            return "the device offers no sample format this backend can read";
        case CaptureError::kAlreadyRunning: return "capture is already running";
    }
    return "unknown capture error";
}

namespace {

// Fill in what `info.id` would actually deliver, by opening it and running the
// negotiation without committing.
//
// The open can fail - because something else holds the device, or because
// there is no sound card at all - and when it does the two numbers stay at
// zero to mean "unknown". The entry is still listed either way: dropping a
// device from a list a person is about to read, because it happened to be busy
// for the half-millisecond the list was built, would be worse than admitting
// the rate is not known yet.
void fill_in_format(DeviceInfo& info) {
    const alsa::QuietErrors quiet;
    snd_pcm_t* handle = nullptr;
    if (snd_pcm_open(&handle, info.id.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK) < 0) {
        return;
    }
    const Pcm owned{handle};
    if (const auto probed = negotiate(handle, /*probe_only=*/true)) {
        info.sample_rate = probed->rate;
        info.channels = static_cast<std::uint16_t>(probed->channels);
    }
}

}  // namespace

std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices() {
    std::vector<DeviceInfo> devices;

    // `default` first and flagged as the default, because it is: alsa-lib
    // resolves it through the user's configuration, which on a desktop is the
    // running sound server.
    DeviceInfo fallback{
        .id = "default",
        .name = "Default (ALSA `default`, routed by the system configuration)",
        .kind = DeviceKind::kInput,
        .sample_rate = 0,
        .channels = 0,
        .is_default = true,
    };
    fill_in_format(fallback);
    devices.push_back(std::move(fallback));

    alsa::for_each_pcm(SND_PCM_STREAM_CAPTURE, [&devices](const alsa::PcmEntry& entry) {
        const bool loopback = entry.card_id == kLoopbackCardId;
        DeviceInfo info{
            .id = alsa::hw_device_name(entry.card_id, entry.device),
            .name = std::format("{}: {}", entry.card_name, entry.device_name),
            .kind = loopback ? DeviceKind::kLoopback : DeviceKind::kInput,
            .sample_rate = 0,
            .channels = 0,
            .is_default = false,
        };
        if (loopback) {
            info.name += " (loopback)";
        }
        fill_in_format(info);
        devices.push_back(std::move(info));
    });

    return devices;
}

struct Capture::Impl {
    std::unique_ptr<RingBuffer> ring;
    std::jthread worker;
    snd_pcm_t* pcm = nullptr;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> frames_captured{0};
    std::atomic<std::uint64_t> frames_silence{0};
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
};

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
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    // After the join, not before: the worker is the only thread allowed to
    // touch the handle while it runs.
    if (impl_->pcm != nullptr) {
        snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
    }
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, CaptureError> Capture::start(const std::string& device_id, DeviceKind kind,
                                                 std::size_t ring_capacity_samples) {
    if (running()) {
        return std::unexpected(CaptureError::kAlreadyRunning);
    }

    // Resolve the name before opening anything, so a caller that asked for
    // loopback on a machine without snd-aloop is told that rather than being
    // quietly given a microphone.
    std::string name = device_id;
    if (name.empty()) {
        if (kind == DeviceKind::kLoopback) {
            alsa::for_each_pcm(SND_PCM_STREAM_CAPTURE, [&name](const alsa::PcmEntry& entry) {
                if (name.empty() && entry.card_id == kLoopbackCardId) {
                    name = alsa::hw_device_name(entry.card_id, entry.device);
                }
            });
            if (name.empty()) {
                return std::unexpected(CaptureError::kDeviceNotFound);
            }
        } else {
            name = "default";
        }
    }

    // Opened blocking and negotiated on this thread, so a device that is busy
    // or offers nothing readable is reported synchronously rather than through
    // a thread that starts and immediately dies.
    snd_pcm_t* handle = nullptr;
    if (snd_pcm_open(&handle, name.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0) {
        return std::unexpected(CaptureError::kDeviceNotFound);
    }
    Pcm opened{handle};

    const auto negotiated = negotiate(handle, /*probe_only=*/false);
    if (!negotiated) {
        return std::unexpected(negotiated.error());
    }
    if (snd_pcm_prepare(handle) < 0 || snd_pcm_start(handle) < 0) {
        return std::unexpected(CaptureError::kComFailure);
    }

    impl_->ring = std::make_unique<RingBuffer>(ring_capacity_samples);
    impl_->sample_rate = negotiated->rate;
    impl_->channels = static_cast<std::uint16_t>(negotiated->channels);
    impl_->frames_captured.store(0, std::memory_order_relaxed);
    impl_->frames_silence.store(0, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);
    impl_->pcm = opened.release();

    const bool loopback = kind == DeviceKind::kLoopback;
    const auto settings = *negotiated;

    impl_->worker = std::jthread([this, settings, loopback](const std::stop_token& stop) {
        snd_pcm_t* pcm = impl_->pcm;
        const std::size_t samples_per_period =
            static_cast<std::size_t>(settings.period) * settings.channels;

        std::vector<std::byte> raw(samples_per_period * settings.bytes_per_sample);
        std::vector<float> scratch;
        std::vector<float> silence;

        // The loopback timeline, in the same terms the WASAPI backend keeps
        // it: how many frames SHOULD have arrived by now, so a stall can be
        // filled in rather than compressing the recording.
        const auto started = std::chrono::steady_clock::now();
        std::uint64_t timeline_frames = 0;

        while (!stop.stop_requested()) {
            const int ready = snd_pcm_wait(pcm, kWaitMs);
            if (ready < 0) {
                // -EPIPE here is an overrun: the ring behind us filled while
                // the consumer was away. Recoverable, and the frames are gone
                // either way, so recover and carry on.
                if (snd_pcm_recover(pcm, ready, /*silent=*/1) < 0) {
                    break;
                }
                snd_pcm_start(pcm);
                continue;
            }
            if (ready > 0) {
                const snd_pcm_sframes_t frames =
                    snd_pcm_readi(pcm, raw.data(), settings.period);
                if (frames < 0) {
                    if (snd_pcm_recover(pcm, static_cast<int>(frames), /*silent=*/1) < 0) {
                        break;
                    }
                    snd_pcm_start(pcm);
                    continue;
                }
                if (frames > 0) {
                    const auto count = static_cast<std::size_t>(frames);
                    convert(raw.data(), count * settings.channels, settings.format, scratch);
                    impl_->ring->write(scratch);
                    impl_->frames_captured.fetch_add(count, std::memory_order_relaxed);
                    timeline_frames += count;
                }
            }

            if (!loopback) {
                continue;
            }
            // snd-aloop delivers nothing at all while nothing is playing into
            // it, exactly as a WASAPI render endpoint does in loopback mode,
            // so the gap is synthesised for the same reason: downstream wants
            // a continuous timeline, and an encoder fed a compressed one
            // produces a recording that runs fast.
            const auto elapsed = std::chrono::duration<double>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            const auto elapsed_frames =
                static_cast<std::uint64_t>(elapsed * static_cast<double>(settings.rate));
            if (elapsed_frames > timeline_frames + settings.rate / 100) {
                auto missing = elapsed_frames - timeline_frames;
                missing = std::min<std::uint64_t>(missing, settings.rate);  // cap a long stall
                silence.assign(static_cast<std::size_t>(missing) * settings.channels, 0.0f);
                impl_->ring->write(silence);
                impl_->frames_silence.fetch_add(missing, std::memory_order_relaxed);
                timeline_frames += missing;
            }
        }

        snd_pcm_drop(pcm);
    });

    return {};
}

}  // namespace ac3::capture
