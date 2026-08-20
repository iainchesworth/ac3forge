#include "ac3/audio/passthrough.hpp"

// The ALSA passthrough backend. CMake compiles this directory's
// passthrough.cpp on a Linux host whose libasound development headers are
// present and another directory's everywhere else, so there is no #ifdef -
// the file's path is what says "ALSA".
//
// ---------------------------------------------------------------------------
// Why this is the backend that decided which Linux audio API to use
// ---------------------------------------------------------------------------
// Capture is an ordinary PCM stream and every Linux audio API can do it.
// Bitstreaming is not, and ALSA is the layer where it is expressed:
//
//   * ALSA does it by opening a plain 16-bit stereo PCM whose IEC 60958
//     channel status has the non-audio bit set. The bit travels with the
//     samples down the S/PDIF or HDMI link and is what makes the receiver
//     decode rather than reproduce them. device_names.hpp is that bit.
//   * PulseAudio does it with PA_STREAM_PASSTHROUGH and an AC-3 pa_format_info
//     - a real capability, but one that ends in the same ALSA call, made by
//     the daemon instead of by us.
//   * PipeWire does it with SPA_MEDIA_SUBTYPE_iec958 and an AC-3 codec, and
//     likewise finishes in ALSA.
//
// So ALSA is not merely the lowest common denominator here; it is the layer
// the other two are implemented on top of, it is present on every Linux system
// including ones with no sound server at all, and its device string is what
// gives a caller direct, unmixed access to the hardware. The cost is
// coexistence: opening a hw: device takes it exclusively, so a running sound
// server has to have released it - which is the same bargain WASAPI exclusive
// mode strikes, and for the same reason. A PipeWire backend is the sensible
// second one to add, as a sibling directory selected the same way; it would
// buy politeness, not capability.
//
// ---------------------------------------------------------------------------
// Exclusive mode, ALSA-style
// ---------------------------------------------------------------------------
// There is no share-mode flag to set. The `iec958` and `hdmi` device names
// resolve to the hardware device directly, with none of dmix's mixing,
// resampling or volume scaling in the way - which is precisely what exclusive
// mode buys on Windows. A device already held by another process simply fails
// to open, and that is reported as kExclusiveUnavailable.
//
// ---------------------------------------------------------------------------
// AC-3 and E-AC-3
// ---------------------------------------------------------------------------
// One difference runs through every function here: a Dolby Digital Plus burst
// is four times the size of a Dolby Digital one and covers the same span of
// time, so its link runs four times as fast. 48 kHz content therefore opens
// the device at 192 kHz - which is why most S/PDIF outputs enumerate with
// supports_eac3_passthrough false while supports_ac3_passthrough is true. An
// optical or coaxial link is not specified past 96 kHz; HDMI is where E-AC-3
// actually goes.

#include <alsa/asoundlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "ac3/audio/ring_buffer.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "alsa_support.hpp"
#include "device_names.hpp"

namespace ac3::audio {

namespace {

using alsa::DigitalOutput;
using alsa::HwParams;
using alsa::Pcm;
using alsa::SwParams;

// The IEC 61937 carrier is a 2-channel 16-bit stream whatever rides inside it,
// so a burst's length in bytes and in sample-frames differ by a constant 4.
// One AC-3 burst is 6144 bytes = 1536 frames; one E-AC-3 burst is four times
// that in both, and travels over a link clocked four times as fast, so it
// still covers the same span of time.
constexpr unsigned kCarrierChannels = 2;
constexpr snd_pcm_format_t kCarrierFormat = SND_PCM_FORMAT_S16_LE;
constexpr std::size_t kCarrierFrameBytes = 4;
constexpr unsigned kPeriodsPerBuffer = 4;
constexpr int kWaitMs = 100;

std::size_t burst_bytes_for(BitstreamFormat format) {
    return format == BitstreamFormat::kEac3 ? iec61937::kEac3BurstBytes : iec61937::kBurstBytes;
}

// Why snd_pcm_open() said no.
//
// alsa-lib returns a negated errno, and the two answers worth telling apart
// are "that device is not here" and "that device is here and someone else has
// it" - the second is the one a user can do something about, by stopping
// whatever holds it. Guessing between them from a single failure code is how
// this backend originally reported a misspelt device name as a busy one.
PassthroughError open_failure(int error) {
    switch (-error) {
        // The name did not resolve to a device, or resolved to one that is
        // not present: a card that has been unplugged, an HDMI output with no
        // display attached, a typo in a device string.
        case ENOENT:
        case ENODEV:
        case ENXIO:
        case EINVAL:
            return PassthroughError::kDeviceNotFound;
        // Present, and not ours to have. EACCES is the same answer with a
        // different cause - the device is there but this user cannot open it,
        // usually for want of membership of the `audio` group.
        case EBUSY:
        case EAGAIN:
        case EACCES:
        case EPERM:
            return PassthroughError::kExclusiveUnavailable;
        default:
            return PassthroughError::kComFailure;
    }
}

// A digital output found by walking the cards, before it has been probed.
struct Candidate {
    int card = 0;
    DigitalOutput kind = DigitalOutput::kNone;
    std::string name;       // "iec958:CARD=PCH,DEV=0" - no channel status yet
    std::string hw_name;    // "hw:CARD=PCH,DEV=1" - the control probe's target
    std::string friendly;   // for a device list a person reads
};

// Configure an open PCM for the IEC 61937 carrier: 16-bit stereo at the link
// rate, no conversion of any kind in the path.
//
// `commit` distinguishes the two callers. Enumeration only wants to know
// whether the parameters would be accepted, and stops before installing them;
// start() installs them and then sets up the software parameters too, sizing
// the period to one whole burst.
bool configure(snd_pcm_t* pcm, std::uint32_t carrier, std::size_t burst_frames, bool commit) {
    HwParams params;
    if (!params || snd_pcm_hw_params_any(pcm, params.get()) < 0) {
        return false;
    }
    if (snd_pcm_hw_params_set_access(pcm, params.get(), SND_PCM_ACCESS_RW_INTERLEAVED) < 0 ||
        snd_pcm_hw_params_set_format(pcm, params.get(), kCarrierFormat) < 0 ||
        snd_pcm_hw_params_set_channels(pcm, params.get(), kCarrierChannels) < 0) {
        return false;
    }
    // set_rate, not set_rate_near: a burst stream at the wrong rate is not a
    // slightly wrong burst stream, it is noise. If the device will not run the
    // link at this rate, this output cannot carry this format - which for
    // E-AC-3 means most S/PDIF outputs, since 192 kHz is beyond what an
    // optical or coaxial link is specified for.
    if (snd_pcm_hw_params_set_rate(pcm, params.get(), carrier, 0) < 0) {
        return false;
    }
    if (!commit) {
        return true;
    }

    auto period = static_cast<snd_pcm_uframes_t>(burst_frames);
    int direction = 0;
    if (snd_pcm_hw_params_set_period_size_near(pcm, params.get(), &period, &direction) < 0) {
        return false;
    }
    snd_pcm_uframes_t buffer = period * kPeriodsPerBuffer;
    if (snd_pcm_hw_params_set_buffer_size_near(pcm, params.get(), &buffer) < 0) {
        return false;
    }
    if (snd_pcm_hw_params(pcm, params.get()) < 0) {
        return false;
    }

    SwParams software;
    if (software && snd_pcm_sw_params_current(pcm, software.get()) >= 0) {
        // Start once the buffer is full rather than on the first write, so the
        // link comes up with a whole buffer of bursts behind it instead of
        // one, and the receiver has the best chance of locking first time.
        snd_pcm_sw_params_set_start_threshold(pcm, software.get(), buffer);
        snd_pcm_sw_params_set_avail_min(pcm, software.get(), period);
        snd_pcm_sw_params(pcm, software.get());
    }
    return true;
}

// Open `name`, check it takes the carrier format, close it again.
//
// ALSA has no IsFormatSupported(): the only way to ask whether a device will
// accept a format is to open it and offer it. So this probe is intrusive in a
// way the WASAPI one is not - it briefly holds the device - and it answers
// "no" for a device that is merely busy. Both are stated in the header's
// wording for supports_exclusive_pcm, which exists to tell those two apart.
bool probe(const std::string& name, std::uint32_t carrier) {
    const alsa::QuietErrors quiet;
    snd_pcm_t* handle = nullptr;
    if (snd_pcm_open(&handle, name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) < 0) {
        return false;
    }
    const Pcm owned{handle};
    return configure(handle, carrier, /*burst_frames=*/0, /*commit=*/false);
}

// Whether `base` will carry `format` at `content_rate`: the device name with
// the right channel status for the link rate that format needs, opened and
// offered the carrier parameters.
bool probe_format(std::string_view base, BitstreamFormat format, std::uint32_t content_rate) {
    const std::uint32_t carrier = alsa::carrier_rate(format, content_rate);
    const auto name = alsa::passthrough_device_name(base, carrier);
    return name.has_value() && probe(*name, carrier);
}

// Every digital output on the machine, in card then device order.
//
// The `hdmi:`/`iec958:` plugins take a logical index - the card's first HDMI
// PCM is hdmi:DEV=0 whatever hardware device number it happens to have - so
// the two are counted separately per card as the walk goes.
std::vector<Candidate> find_candidates() {
    std::vector<Candidate> candidates;
    int counted_card = -1;
    unsigned hdmi_index = 0;
    unsigned spdif_index = 0;

    alsa::for_each_pcm(SND_PCM_STREAM_PLAYBACK, [&](const alsa::PcmEntry& entry) {
        if (entry.card != counted_card) {
            counted_card = entry.card;
            hdmi_index = 0;
            spdif_index = 0;
        }
        const DigitalOutput kind = alsa::classify_digital_output(entry.device_name);
        if (kind == DigitalOutput::kNone) {
            return;
        }
        unsigned& index = kind == DigitalOutput::kHdmi ? hdmi_index : spdif_index;
        candidates.push_back(Candidate{
            .card = entry.card,
            .kind = kind,
            .name = alsa::config_device_name(kind, entry.card_id, index),
            .hw_name = alsa::hw_device_name(entry.card_id, entry.device),
            .friendly = std::format("{}: {}", entry.card_name, entry.device_name),
        });
        ++index;
    });
    return candidates;
}

}  // namespace

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure: return "an ALSA call failed";
        case PassthroughError::kDeviceNotFound:
            return "no such output: either the named ALSA device does not exist, or none was "
                   "named and this machine has no S/PDIF or HDMI output for ALSA to find";
        case PassthroughError::kFormatRejected:
            return "the output will not carry this bitstream over IEC 61937 at this rate "
                   "(E-AC-3 needs a 4x link clock, which usually means HDMI rather than S/PDIF, "
                   "and an HDMI output needs a display connected and awake before its audio "
                   "device accepts anything)";
        case PassthroughError::kExclusiveUnavailable:
            return "the device could not be opened directly (PipeWire, PulseAudio or another "
                   "application holds it - a bitstream cannot share a device with a mixer - or "
                   "this user has no permission on it, which means the `audio` group)";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t sample_rate) {
    const int preferred_card = alsa::default_card();
    bool marked_default = false;

    std::vector<RenderDeviceInfo> devices;
    for (const auto& candidate : find_candidates()) {
        RenderDeviceInfo info{
            .id = candidate.name,
            .name = candidate.friendly,
            .is_default = false,
            .supports_ac3_passthrough =
                probe_format(candidate.name, BitstreamFormat::kAc3, sample_rate),
            .supports_eac3_passthrough =
                probe_format(candidate.name, BitstreamFormat::kEac3, sample_rate),
            // The control probe: the same carrier format on the raw hardware
            // device, with no channel status. A device that takes this but
            // neither of the above cannot bitstream; one that takes none of
            // the three is in use by something else.
            .supports_exclusive_pcm = probe(candidate.hw_name, sample_rate),
        };

        if (!marked_default && candidate.card == preferred_card) {
            info.is_default = true;
            marked_default = true;
        }
        devices.push_back(std::move(info));
    }
    // Nothing on the configured default card, or no configuration to read:
    // the first digital output found is as good a default as exists.
    if (!marked_default && !devices.empty()) {
        devices.front().is_default = true;
    }
    return devices;
}

struct PassthroughSink::Impl {
    std::unique_ptr<ByteRingBuffer> queue;
    std::jthread worker;
    snd_pcm_t* pcm = nullptr;
    // Set by start() and read by submit(): the two burst sizes are four times
    // apart, and a caller that hands over the wrong one is handing over a
    // frame boundary in the wrong place rather than a slightly odd length.
    std::size_t burst_bytes = iec61937::kBurstBytes;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> underruns{0};
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
    if (impl_->worker.joinable()) {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    if (impl_->pcm != nullptr) {
        snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
    }
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, PassthroughError> PassthroughSink::start(const std::string& device_id,
                                                             std::uint32_t sample_rate,
                                                             BitstreamFormat format_kind) {
    if (running()) {
        return std::unexpected(PassthroughError::kAlreadyRunning);
    }

    // The link rate, not the content rate: the same for AC-3 and 4x it for
    // E-AC-3. Everything below - the channel status, the device parameters,
    // the burst size - is expressed in the carrier's terms from here on.
    const std::uint32_t carrier = alsa::carrier_rate(format_kind, sample_rate);
    const std::size_t burst_bytes = burst_bytes_for(format_kind);
    const std::size_t burst_frames = burst_bytes / kCarrierFrameBytes;

    // Pick the device before touching it. An empty id means "the default
    // output", which for a bitstream can only mean a digital one that has
    // already said yes - there is no system-wide "default S/PDIF" setting to
    // consult, and defaulting to the analog output would emit full-scale noise
    // from the speakers.
    std::string base = device_id;
    if (base.empty()) {
        const auto candidates = find_candidates();
        if (candidates.empty()) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
        for (const auto& candidate : candidates) {
            if (probe_format(candidate.name, format_kind, sample_rate)) {
                base = candidate.name;
                break;
            }
        }
        if (base.empty()) {
            return std::unexpected(PassthroughError::kFormatRejected);
        }
    }

    const auto name = alsa::passthrough_device_name(base, carrier);
    if (!name) {
        // Reached only for a carrier rate IEC 60958 has no frequency code for.
        // AC-3's three rates all have one; E-AC-3 at 32 kHz would want a
        // 128 kHz link, and that does not.
        return std::unexpected(PassthroughError::kFormatRejected);
    }

    snd_pcm_t* handle = nullptr;
    if (const int opened = snd_pcm_open(&handle, name->c_str(), SND_PCM_STREAM_PLAYBACK, 0);
        opened < 0) {
        return std::unexpected(open_failure(opened));
    }
    Pcm opened{handle};

    if (!configure(handle, carrier, burst_frames, /*commit=*/true)) {
        return std::unexpected(PassthroughError::kFormatRejected);
    }
    if (snd_pcm_prepare(handle) < 0) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    // Room for roughly a second of bursts, so a caller encoding slightly
    // ahead of real time never has to spin. Counted in bursts rather than
    // bytes so an E-AC-3 session gets the same second, not a quarter of one.
    impl_->queue = std::make_unique<ByteRingBuffer>(burst_bytes * 40);
    impl_->burst_bytes = burst_bytes;
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);
    impl_->running.store(true, std::memory_order_release);
    impl_->pcm = opened.release();

    impl_->worker = std::jthread([this, burst_bytes, burst_frames](const std::stop_token& stop) {
        snd_pcm_t* pcm = impl_->pcm;
        std::vector<std::byte> chunk(burst_bytes);

        while (!stop.stop_requested()) {
            const int ready = snd_pcm_wait(pcm, kWaitMs);
            if (ready < 0) {
                if (snd_pcm_recover(pcm, ready, /*silent=*/1) < 0) {
                    break;
                }
                continue;
            }
            if (ready == 0) {
                continue;  // nothing wanted yet; go back and re-check the stop
            }

            const auto got = impl_->queue->read(chunk);
            if (got < chunk.size()) {
                // Nothing queued: emit silence for the remainder. A receiver
                // that sees a gap in the burst stream usually drops lock, so
                // this is counted, not hidden. The count starts at the moment
                // the device is opened, so a caller that opens the sink before
                // it has any bursts ready will see the first few periods
                // charged here - that is a real gap on the wire.
                std::fill(chunk.begin() + static_cast<std::ptrdiff_t>(got), chunk.end(),
                          std::byte{0});
                impl_->underruns.fetch_add(1, std::memory_order_relaxed);
            }

            const snd_pcm_sframes_t written =
                snd_pcm_writei(pcm, chunk.data(), static_cast<snd_pcm_uframes_t>(burst_frames));
            if (written < 0) {
                if (snd_pcm_recover(pcm, static_cast<int>(written), /*silent=*/1) < 0) {
                    break;
                }
                continue;
            }
            impl_->rendered.fetch_add(got / burst_bytes, std::memory_order_relaxed);
        }

        // drop, not drain: a stop request means stop, and draining would play
        // out a buffer of bursts the caller has already stopped feeding.
        snd_pcm_drop(pcm);
    });

    return {};
}

}  // namespace ac3::audio
