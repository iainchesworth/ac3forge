#include "ac3/sinks/passthrough.hpp"

// The macOS passthrough backend. CMake compiles this directory's
// passthrough.cpp under APPLE and another platform directory's everywhere
// else, so there is no #ifdef here - the file's path is what says "macOS".
//
// ---------------------------------------------------------------------------
// CoreAudio's approach to IEC 60958/61937 bitstreaming
// ---------------------------------------------------------------------------
// Where ALSA bitstreams by opening a plain PCM device whose channel status
// carries the non-audio bit (see platform/alsa/passthrough.cpp's own
// header) and WASAPI bitstreams by negotiating a WAVEFORMATEXTENSIBLE with
// an IEC 61937 SubFormat GUID in exclusive mode, CoreAudio does neither:
// there is no per-open "bitstream flag" and no separate compressed
// subformat to hand IAudioClient-style negotiation. What a digital
// (HDMI/optical) output exposes instead is a set of PHYSICAL stream
// formats - the literal bit layout the hardware link runs, as opposed to
// the VIRTUAL format the HAL presents to an application - and one
// AudioFormatID a driver can publish among them is kAudioFormat60958AC3
// ('cac3'), AC-3 already packaged for an IEC 60958 link. Bitstreaming AC-3
// on macOS means retuning the device's PHYSICAL format to that one, for as
// long as this sink runs, and putting it back afterward - the mechanism
// confirmed against three independent, real implementations of exactly
// this while researching this backend: MythTV's audiooutputca.cpp, mpv's
// ao_coreaudio_exclusive.c, and VLC's auhal.c all walk
// kAudioDevicePropertyStreams -> kAudioStreamPropertyAvailablePhysicalFormats
// looking for the same AudioFormatID and commit it the same way, through
// kAudioStreamPropertyPhysicalFormat.
//
// The property read that finds the format is passive - unlike ALSA's
// probe(), which has to snd_pcm_open() a candidate device to learn what it
// accepts (see that file's own comment on why that makes a busy device
// indistinguishable from an incapable one), kAudioStreamPropertyAvailablePhysicalFormats
// is a property of the STREAM OBJECT, readable without opening or hogging
// anything - closer to WASAPI's IsFormatSupported in spirit, and actually
// less invasive than even that, since IsFormatSupported still requires an
// activated IAudioClient. enumerate_render_devices() below never takes hog
// mode or touches a single sample.
//
// Committing the format is a different story, and the one genuinely
// higher-stakes step this backend has that neither of the other two does:
// setting kAudioStreamPropertyPhysicalFormat on a stream retunes THE WHOLE
// DEVICE, not a private handle to it the way opening a WASAPI or ALSA
// device does - every other application using that output hears the
// change too (typically as the device briefly drops out while the hardware
// relocks). That is why hog mode (kAudioDevicePropertyHogMode) is taken
// first: it does not prevent the format change from being device-wide, but
// it does keep another process from opening the device and fighting this
// one over what its format should be, which is the same coexistence
// bargain platform/alsa/passthrough.cpp's own header describes ALSA's hw:
// device striking with a running sound server - "a running sound server
// has to have released it."
//
// Physical format changes are also documented as asynchronous:
// AudioObjectSetPropertyData can return success before the hardware has
// actually retuned. Every implementation surveyed above confirms the
// change with a follow-up read rather than trusting the set call alone;
// coreaudio_support.hpp's wait_for_physical_format does the same, polling
// rather than registering an AudioObjectAddPropertyListener callback - see
// that header's own comment for why a poll is enough for a change that
// only ever happens once, synchronously, inside start().
//
// ---------------------------------------------------------------------------
// AC-3 and E-AC-3
// ---------------------------------------------------------------------------
// kAudioFormat60958AC3 is long-documented and exercised by every real
// implementation cited above. E-AC-3 has no comparably established IEC
// 60958-wrapped physical-format constant in CoreAudioTypes.h - the closest
// published one is kAudioFormatEnhancedAC3 ('ec-3'), the same fourCC this
// project's own ac3::io::build_codec_config_box uses for a raw E-AC-3
// elementary stream in an MP4 sample entry, not a documented S/PDIF/HDMI
// wire format. Apple's own support documentation confirms Dolby Digital
// Plus and Dolby Atmos (E-AC-3 JOC) HDMI passthrough exists on Apple
// Silicon Macs, without documenting the HAL mechanism behind it - so this
// backend probes kAudioStreamPropertyAvailablePhysicalFormats for
// kAudioFormatEnhancedAC3 exactly as it does kAudioFormat60958AC3, and
// where a driver does not offer it (older hardware, a non-HDMI output, an
// Intel Mac), supports_eac3_passthrough simply comes back false - the same
// "a platform can gain one and not the other" contract sinks::RenderDeviceInfo
// already documents for the AC-3/E-AC-3 split, not a claim that every
// digital output on every Mac carries Dolby Digital Plus.
//
// ---------------------------------------------------------------------------
// No worker thread
// ---------------------------------------------------------------------------
// See coreaudio_support.hpp's own header comment: AudioDeviceCreateIOProcID
// hands control to a realtime thread the OS owns, so there is no jthread
// here the way the ALSA/Windows backends each run one - the IOProc callback
// start() registers runs on Apple's I/O thread, not one this library
// spawned. It is a captureless lambda defined inside start() itself, not a
// free function - see platform/macos/capture.cpp's own start() for why
// (the private Impl type is only reachable from there).

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ac3/capture/ring_buffer.hpp"
#include "ac3/sinks/iec61937.hpp"
#include "coreaudio_names.hpp"
#include "coreaudio_support.hpp"

namespace ac3::sinks {

namespace {

constexpr auto kFormatChangeTimeout = std::chrono::milliseconds{2000};

std::size_t burst_bytes_for(BitstreamFormat format) {
    return format == BitstreamFormat::kEac3 ? iec61937::kEac3BurstBytes : iec61937::kBurstBytes;
}

struct Candidate {
    AudioObjectID device = kAudioObjectUnknown;
    AudioStreamID stream = 0;
    AudioStreamBasicDescription format{};
};

// The first output stream on `device` offering `format_id` at `carrier_hz`,
// across every one of its output streams - a device can have more than one
// (a multi-port HDMI/optical combo card, say), and only some may carry it.
std::optional<Candidate> find_stream(AudioObjectID device, AudioFormatID format_id,
                                     Float64 carrier_hz) {
    for (const auto stream : coreaudio::device_streams(device, kAudioDevicePropertyScopeOutput)) {
        const auto formats = coreaudio::available_physical_formats(stream);
        if (const auto match = coreaudio::find_physical_format(formats, format_id, carrier_hz)) {
            return Candidate{.device = device, .stream = stream, .format = *match};
        }
    }
    return std::nullopt;
}

}  // namespace

std::string_view describe(PassthroughError error) {
    switch (error) {
        case PassthroughError::kNoBackend: return "no passthrough backend on this platform";
        case PassthroughError::kComFailure: return "a Core Audio HAL call failed";
        case PassthroughError::kDeviceNotFound:
            return "no such output: either the named Core Audio device does not exist, or none "
                   "was named and this machine has no digital (HDMI/optical) output Core Audio "
                   "reports";
        case PassthroughError::kFormatRejected:
            return "the output will not carry this bitstream over IEC 61937 at this rate (no "
                   "kAudioStreamPropertyAvailablePhysicalFormats entry offers it, or the device "
                   "did not retune to it in time)";
        case PassthroughError::kExclusiveUnavailable:
            return "hog mode could not be acquired (another process already owns the device, or "
                   "the request itself failed)";
        case PassthroughError::kAlreadyRunning: return "passthrough is already running";
        case PassthroughError::kNotRunning: return "passthrough is not running";
    }
    return "unknown passthrough error";
}

std::expected<std::vector<RenderDeviceInfo>, PassthroughError> enumerate_render_devices(
    std::uint32_t sample_rate) {
    const auto default_id = coreaudio::device_uid(coreaudio::default_device(/*input=*/false));

    std::vector<RenderDeviceInfo> devices;
    for (const auto device : coreaudio::device_list()) {
        if (coreaudio::channel_count(device, kAudioDevicePropertyScopeOutput) == 0) {
            continue;
        }
        const std::string uid = coreaudio::device_uid(device);
        if (uid.empty()) {
            continue;
        }
        const std::string name = coreaudio::device_name(device);

        devices.push_back(RenderDeviceInfo{
            .id = uid,
            .name = name.empty() ? coreaudio::fallback_name(uid) : name,
            .is_default = uid == default_id,
            .supports_ac3_passthrough =
                find_stream(device, coreaudio::physical_format_id(BitstreamFormat::kAc3),
                           static_cast<Float64>(
                               coreaudio::carrier_rate(BitstreamFormat::kAc3, sample_rate)))
                    .has_value(),
            .supports_eac3_passthrough =
                find_stream(device, coreaudio::physical_format_id(BitstreamFormat::kEac3),
                           static_cast<Float64>(
                               coreaudio::carrier_rate(BitstreamFormat::kEac3, sample_rate)))
                    .has_value(),
            // Every real output device publishes ordinary linear PCM among
            // its available physical formats - the control probe playing
            // the same role ALSA's own supports_exclusive_pcm does: "this
            // device exists and answers, whether or not it can bitstream."
            .supports_exclusive_pcm =
                find_stream(device, kAudioFormatLinearPCM, static_cast<Float64>(sample_rate))
                    .has_value(),
        });
    }
    return devices;
}

struct PassthroughSink::Impl {
    AudioObjectID device = kAudioObjectUnknown;
    AudioStreamID stream = 0;
    AudioDeviceIOProcID io_proc_id = nullptr;
    AudioStreamBasicDescription original_format{};
    bool have_original_format = false;
    coreaudio::HogGuard hog;
    coreaudio::MixingGuard mixing;
    std::unique_ptr<capture::ByteRingBuffer> queue;
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
    if (impl_->io_proc_id != nullptr) {
        AudioDeviceStop(impl_->device, impl_->io_proc_id);
        AudioDeviceDestroyIOProcID(impl_->device, impl_->io_proc_id);
        impl_->io_proc_id = nullptr;
    }
    if (impl_->have_original_format) {
        coreaudio::set_property(impl_->stream, coreaudio::address(kAudioStreamPropertyPhysicalFormat),
                                impl_->original_format);
        coreaudio::wait_for_physical_format(impl_->stream, impl_->original_format.mFormatID,
                                            impl_->original_format.mSampleRate,
                                            kFormatChangeTimeout);
        impl_->have_original_format = false;
    }
    impl_->mixing.reset();
    impl_->hog.reset();
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, PassthroughError> PassthroughSink::start(const std::string& device_id,
                                                              std::uint32_t sample_rate,
                                                              BitstreamFormat format_kind) {
    if (running()) {
        return std::unexpected(PassthroughError::kAlreadyRunning);
    }

    const auto format_id = coreaudio::physical_format_id(format_kind);
    const auto carrier = static_cast<Float64>(coreaudio::carrier_rate(format_kind, sample_rate));

    // Pick the device before touching anything, mirroring
    // platform/alsa/passthrough.cpp's own discipline: an empty id can only
    // mean "the first digital output that has already said yes", never a
    // system-wide default (defaulting to an analog output would emit
    // full-scale noise from the speakers).
    std::optional<Candidate> candidate;
    if (device_id.empty()) {
        for (const auto device : coreaudio::device_list()) {
            candidate = find_stream(device, format_id, carrier);
            if (candidate) {
                break;
            }
        }
        if (!candidate) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
    } else {
        const auto device = coreaudio::device_for_uid(device_id);
        if (device == kAudioObjectUnknown) {
            return std::unexpected(PassthroughError::kDeviceNotFound);
        }
        candidate = find_stream(device, format_id, carrier);
        if (!candidate) {
            return std::unexpected(PassthroughError::kFormatRejected);
        }
    }

    const auto original = coreaudio::get_property<AudioStreamBasicDescription>(
        candidate->stream, coreaudio::address(kAudioStreamPropertyPhysicalFormat));
    if (!original) {
        return std::unexpected(PassthroughError::kComFailure);
    }

    if (!impl_->hog.acquire(candidate->device)) {
        return std::unexpected(PassthroughError::kExclusiveUnavailable);
    }
    impl_->mixing.disable(candidate->device);

    if (!coreaudio::set_property(candidate->stream,
                                 coreaudio::address(kAudioStreamPropertyPhysicalFormat),
                                 candidate->format) ||
        !coreaudio::wait_for_physical_format(candidate->stream, candidate->format.mFormatID,
                                             candidate->format.mSampleRate,
                                             kFormatChangeTimeout)) {
        // Best-effort revert before giving the device back - see this
        // failure path mirrored (and waited on) in stop() for the normal
        // shutdown case.
        coreaudio::set_property(candidate->stream,
                                coreaudio::address(kAudioStreamPropertyPhysicalFormat), *original);
        coreaudio::wait_for_physical_format(candidate->stream, original->mFormatID,
                                            original->mSampleRate, kFormatChangeTimeout);
        impl_->mixing.reset();
        impl_->hog.reset();
        return std::unexpected(PassthroughError::kFormatRejected);
    }

    impl_->device = candidate->device;
    impl_->stream = candidate->stream;
    impl_->original_format = *original;
    impl_->have_original_format = true;
    impl_->burst_bytes = burst_bytes_for(format_kind);
    // Room for roughly a second of bursts, so a caller encoding slightly
    // ahead of real time never has to spin - the same sizing rationale as
    // the Windows/ALSA backends.
    impl_->queue = std::make_unique<capture::ByteRingBuffer>(impl_->burst_bytes * 40);
    impl_->submitted.store(0, std::memory_order_relaxed);
    impl_->rendered.store(0, std::memory_order_relaxed);
    impl_->underruns.store(0, std::memory_order_relaxed);

    // A captureless lambda, not a free function - see
    // platform/macos/capture.cpp's own start() for why: AudioDeviceIOProc
    // needs a plain C function pointer, but `Impl` is private to
    // PassthroughSink, so only a member function (or something lexically
    // nested inside one, which is exactly what this is) has access to it.
    const auto io_proc = [](AudioObjectID /*device*/, const AudioTimeStamp* /*now*/,
                            const AudioBufferList* /*input_data*/,
                            const AudioTimeStamp* /*input_time*/, AudioBufferList* output,
                            const AudioTimeStamp* /*output_time*/, void* client_data) -> OSStatus {
        auto* impl = static_cast<Impl*>(client_data);
        if (output == nullptr || output->mNumberBuffers == 0) {
            return noErr;
        }
        auto& buffer = output->mBuffers[0];
        const auto wanted = static_cast<std::size_t>(buffer.mDataByteSize);
        auto* dest = static_cast<std::byte*>(buffer.mData);

        std::size_t filled = 0;
        while (filled + impl->burst_bytes <= wanted) {
            const auto got = impl->queue->read(std::span(dest + filled, impl->burst_bytes));
            if (got < impl->burst_bytes) {
                // Nothing queued: emit silence for the remainder. A
                // receiver that sees a gap in the burst stream usually
                // drops lock, so this is counted, not hidden - matching the
                // Windows/ALSA backends' own underrun discipline.
                std::fill(dest + filled + got, dest + filled + impl->burst_bytes, std::byte{0});
                impl->underruns.fetch_add(1, std::memory_order_relaxed);
            } else {
                impl->rendered.fetch_add(1, std::memory_order_relaxed);
            }
            filled += impl->burst_bytes;
        }
        if (filled < wanted) {
            std::fill(dest + filled, dest + wanted, std::byte{0});
        }
        return noErr;
    };

    AudioDeviceIOProcID proc_id = nullptr;
    if (AudioDeviceCreateIOProcID(impl_->device, io_proc, impl_.get(), &proc_id) != noErr ||
        proc_id == nullptr) {
        stop();
        return std::unexpected(PassthroughError::kComFailure);
    }
    if (AudioDeviceStart(impl_->device, proc_id) != noErr) {
        AudioDeviceDestroyIOProcID(impl_->device, proc_id);
        stop();
        return std::unexpected(PassthroughError::kComFailure);
    }

    impl_->io_proc_id = proc_id;
    impl_->running.store(true, std::memory_order_release);
    return {};
}

}  // namespace ac3::sinks
