#include "ac3/capture/capture.hpp"

// The macOS capture backend. CMake compiles this directory's capture.cpp
// under APPLE and another platform directory's everywhere else, so there is
// no #ifdef here - the file's path is what says "macOS".
//
// This is the CoreAudio Audio HAL (AudioObjectID / AudioDeviceIOProc) - the
// same layer passthrough.cpp uses, not the higher-level AVAudioEngine or
// Audio Queue Services APIs. That choice mirrors why platform/alsa is ALSA
// rather than PulseAudio/PipeWire (see platform/alsa/passthrough.cpp's own
// header): passthrough's exclusive-mode physical-format switch has to
// happen at the HAL, so the HAL is where device enumeration, hog mode and
// format negotiation all live in one place (coreaudio_support.hpp), and
// capture sharing that layer means it shares that code rather than talking
// to a second, higher-level API with its own device model. AVAudioEngine
// also has no equivalent to a persistent per-endpoint UID the way
// kAudioDevicePropertyDeviceUID gives every HAL object, which every
// DeviceInfo.id in this project's other backends already assumes is stable
// across a session (see capture.hpp).
//
// ---------------------------------------------------------------------------
// Loopback
// ---------------------------------------------------------------------------
// Unlike WASAPI (any render endpoint reopened in loopback mode), there is no
// HAL-level "capture what a render device is playing". The closest is
// Apple's Core Audio Process Tap API (AudioHardwareCreateProcessTap /
// CATapDescription, added in macOS 14.4), which needs an Objective-C class
// (CATapDescription has no C entry point) and a runtime TCC consent prompt -
// neither of which fits a plain, headless-CI-buildable C++ translation unit
// the way every other file in this project is. This is the same category of
// gap platform/alsa/capture.cpp documents for a machine with no snd-aloop
// module loaded: "that is the honest answer rather than a failure."
// enumerate_devices() below reports every real input endpoint and no
// loopback ones, and start() refuses DeviceKind::kLoopback outright rather
// than silently substituting a microphone.
//
// ---------------------------------------------------------------------------
// No worker thread
// ---------------------------------------------------------------------------
// See coreaudio_support.hpp's own header comment: the IOProc callback
// start() registers below runs on a realtime thread the OS owns (between
// AudioDeviceStart and AudioDeviceStop), not one this library spawned -
// there is no jthread field in Impl the way the ALSA/Windows backends each
// have one. It is a captureless lambda defined inside start() itself rather
// than a free function - see start()'s own comment for why.

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "coreaudio_names.hpp"
#include "coreaudio_support.hpp"

namespace ac3::capture {

namespace {

// Weaves however many buffers `input` carries (one interleaved buffer, or
// one per channel) into `out`, normalised to float - mirrors
// platform/alsa/capture.cpp's own convert(), generalised over the layout
// question ALSA never has (alsa-lib always hands back one interleaved
// buffer).
void collect(const AudioBufferList* input, std::uint16_t channels, bool interleaved,
            coreaudio::SampleFormat format, std::vector<float>& out) {
    const auto bytes = coreaudio::bytes_per_sample(format);
    if (input == nullptr || input->mNumberBuffers == 0 || bytes == 0 || channels == 0) {
        out.clear();
        return;
    }
    if (interleaved) {
        const auto& buffer = input->mBuffers[0];
        const std::size_t sample_count = buffer.mDataByteSize / bytes;
        out.resize(sample_count);
        coreaudio::samples_to_float(static_cast<const std::byte*>(buffer.mData), sample_count,
                                    format, out);
        return;
    }

    const std::size_t frames = input->mBuffers[0].mDataByteSize / bytes;
    out.assign(frames * channels, 0.0f);
    std::vector<float> channel_scratch;
    const auto channel_limit = static_cast<UInt32>(channels);
    for (UInt32 ch = 0; ch < input->mNumberBuffers && ch < channel_limit; ++ch) {
        const auto& buffer = input->mBuffers[ch];
        const std::size_t ch_frames = buffer.mDataByteSize / bytes;
        channel_scratch.resize(ch_frames);
        coreaudio::samples_to_float(static_cast<const std::byte*>(buffer.mData), ch_frames, format,
                                    channel_scratch);
        for (std::size_t i = 0; i < ch_frames && i < frames; ++i) {
            out[i * channels + ch] = channel_scratch[i];
        }
    }
}

}  // namespace

std::string_view describe(CaptureError error) {
    switch (error) {
        case CaptureError::kNoBackend: return "no capture backend on this platform";
        case CaptureError::kComFailure: return "a Core Audio HAL call failed";
        case CaptureError::kDeviceNotFound: return "the requested capture device was not found";
        case CaptureError::kFormatUnsupported:
            return "the device offers no sample format this backend can read";
        case CaptureError::kAlreadyRunning: return "capture is already running";
    }
    return "unknown capture error";
}

std::expected<std::vector<DeviceInfo>, CaptureError> enumerate_devices() {
    std::vector<DeviceInfo> devices;
    const auto default_id = coreaudio::device_uid(coreaudio::default_device(/*input=*/true));

    for (const auto device : coreaudio::device_list()) {
        const auto channels = coreaudio::channel_count(device, kAudioDevicePropertyScopeInput);
        if (channels == 0) {
            continue;  // output-only, or a dead/disconnected object
        }
        const std::string uid = coreaudio::device_uid(device);
        if (uid.empty()) {
            continue;  // nothing start() could ever open again by id
        }
        const std::string name = coreaudio::device_name(device);
        devices.push_back(DeviceInfo{
            .id = uid,
            .name = name.empty() ? coreaudio::fallback_name(uid) : name,
            .kind = DeviceKind::kInput,
            .sample_rate = static_cast<std::uint32_t>(coreaudio::nominal_sample_rate(device)),
            .channels = static_cast<std::uint16_t>(channels),
            .is_default = uid == default_id,
        });
    }
    return devices;
}

struct Capture::Impl {
    AudioObjectID device = kAudioObjectUnknown;
    AudioDeviceIOProcID io_proc_id = nullptr;
    std::unique_ptr<RingBuffer> ring;
    coreaudio::SampleFormat format = coreaudio::SampleFormat::kFloat32;
    bool interleaved = true;
    std::atomic_bool running{false};
    std::atomic<std::uint64_t> frames_captured{0};
    std::atomic<std::uint64_t> frames_silence{0};
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    // Reused by the IOProc; see start()'s own comment on why the callback
    // reaches this through `client_data` rather than a lambda capture -
    // AudioDeviceIOProc's plain-function-pointer signature rules one out.
    std::vector<float> scratch;
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
    if (impl_->io_proc_id != nullptr) {
        AudioDeviceStop(impl_->device, impl_->io_proc_id);
        AudioDeviceDestroyIOProcID(impl_->device, impl_->io_proc_id);
        impl_->io_proc_id = nullptr;
    }
    impl_->running.store(false, std::memory_order_release);
}

std::expected<void, CaptureError> Capture::start(const std::string& device_id, DeviceKind kind,
                                                 std::size_t ring_capacity_samples) {
    if (running()) {
        return std::unexpected(CaptureError::kAlreadyRunning);
    }

    AudioObjectID device = kAudioObjectUnknown;
    if (device_id.empty()) {
        if (kind == DeviceKind::kLoopback) {
            // No loopback endpoint is ever enumerated (see this file's own
            // header comment) and there is no "the default one" to fall
            // back to the way ALSA's `default` PCM or a WASAPI render
            // endpoint provide, so this is refused outright rather than
            // silently opening a microphone instead.
            return std::unexpected(CaptureError::kDeviceNotFound);
        }
        device = coreaudio::default_device(/*input=*/true);
    } else {
        // Handed to device_for_uid verbatim regardless of `kind`, the same
        // discipline platform/alsa/capture.cpp documents for its own
        // device_id: a caller naming a specific endpoint (a virtual
        // loopback driver's own input, say) is trusted to know what they
        // asked for.
        device = coreaudio::device_for_uid(device_id);
    }
    if (device == kAudioObjectUnknown) {
        return std::unexpected(CaptureError::kDeviceNotFound);
    }

    const auto asbd = coreaudio::get_property<AudioStreamBasicDescription>(
        device,
        coreaudio::address(kAudioDevicePropertyStreamFormat, kAudioDevicePropertyScopeInput));
    if (!asbd || asbd->mChannelsPerFrame == 0) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }
    const auto format = coreaudio::classify_pcm(*asbd);
    if (format == coreaudio::SampleFormat::kUnsupported) {
        return std::unexpected(CaptureError::kFormatUnsupported);
    }

    impl_->ring = std::make_unique<RingBuffer>(ring_capacity_samples);
    impl_->device = device;
    impl_->format = format;
    impl_->interleaved = (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0;
    impl_->channels = static_cast<std::uint16_t>(asbd->mChannelsPerFrame);
    impl_->sample_rate = static_cast<std::uint32_t>(asbd->mSampleRate);
    impl_->frames_captured.store(0, std::memory_order_relaxed);
    impl_->frames_silence.store(0, std::memory_order_relaxed);
    impl_->scratch.reserve(static_cast<std::size_t>(impl_->channels) * 4096);

    // A captureless lambda, not a free function: AudioDeviceIOProc is a plain
    // C function pointer (a capturing closure cannot decay to one), but
    // `Impl` is private to Capture - a free function has no access to it at
    // all, only a member function or something lexically nested inside one
    // does. Defining the callback here, inside start(), gets both: the
    // implicit captureless-lambda-to-function-pointer conversion
    // AudioDeviceCreateIOProcID needs, and the same access to Capture::Impl
    // this function itself already has. Every real per-call frame of state
    // is reached through `client_data`, not a capture, so "captureless" costs
    // nothing here.
    const auto io_proc = [](AudioObjectID /*device*/, const AudioTimeStamp* /*now*/,
                            const AudioBufferList* input_data, const AudioTimeStamp* /*input_time*/,
                            AudioBufferList* /*output_data*/, const AudioTimeStamp* /*output_time*/,
                            void* client_data) -> OSStatus {
        auto* impl = static_cast<Impl*>(client_data);
        collect(input_data, impl->channels, impl->interleaved, impl->format, impl->scratch);
        if (!impl->scratch.empty() && impl->channels > 0) {
            impl->ring->write(impl->scratch);
            impl->frames_captured.fetch_add(impl->scratch.size() / impl->channels,
                                            std::memory_order_relaxed);
        }
        return noErr;
    };

    AudioDeviceIOProcID proc_id = nullptr;
    if (AudioDeviceCreateIOProcID(device, io_proc, impl_.get(), &proc_id) != noErr ||
        proc_id == nullptr) {
        return std::unexpected(CaptureError::kComFailure);
    }
    if (AudioDeviceStart(device, proc_id) != noErr) {
        AudioDeviceDestroyIOProcID(device, proc_id);
        return std::unexpected(CaptureError::kComFailure);
    }

    impl_->io_proc_id = proc_id;
    impl_->running.store(true, std::memory_order_release);
    return {};
}

}  // namespace ac3::capture
