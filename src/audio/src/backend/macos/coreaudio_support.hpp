#pragma once

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// The handful of things capture.cpp, monitor.cpp and passthrough.cpp all
// need from the CoreAudio HAL, kept in one place so the three cannot drift -
// the same role platform/alsa/alsa_support.hpp plays for the ALSA backend.
// coreaudio_names.hpp next door is this file's pure sibling.
//
// ---------------------------------------------------------------------------
// AudioObjectID and the property-address idiom
// ---------------------------------------------------------------------------
// Every HAL object - the system itself (kAudioObjectSystemObject), a device,
// a stream - is addressed by an AudioObjectID (a plain UInt32) and every
// property read or write names an AudioObjectPropertyAddress
// {selector, scope, element}. There is no separate "open" call the way
// ALSA's snd_pcm_open or WASAPI's IAudioClient::Initialize needs before
// anything else can happen: enumerating, probing formats and reading a
// device's current configuration are all just property reads against an id
// that nothing has to be "opened" to have - which is also why
// enumerate_devices() (capture.cpp) and enumerate_render_devices()
// (passthrough.cpp) never touch a device beyond reading its properties,
// unlike platform/alsa/passthrough.cpp's probe(), which has to briefly
// snd_pcm_open() a candidate to learn the same thing.
//
// ---------------------------------------------------------------------------
// No worker thread
// ---------------------------------------------------------------------------
// The Windows and ALSA backends each drive their own std::jthread that
// blocks on a wait/poll call and copies into or out of a buffer the OS
// handed back. The HAL's own I/O primitive is the opposite shape: register a
// callback (AudioDeviceIOProcID, via AudioDeviceCreateIOProcID) and
// AudioDeviceStart() hands control to a realtime thread THE OS owns, which
// calls that callback at the device's hardware period until
// AudioDeviceStop() - there is no blocking read/write call to run a thread
// of our own around, the way AAudio's blocking AAudioStream_write() lets
// apps/android/monitor.cpp keep the jthread shape even though AAudio
// also offers a callback API. So capture.cpp/monitor.cpp/passthrough.cpp in
// this directory have no `worker` field at all: start() registers the
// IOProc and returns, stop() unregisters it, and every sample the ring
// buffers see passes through a callback running on Apple's own I/O thread
// rather than one this library spawned - RingBuffer's lock-free,
// allocation-free discipline (see its own header comment) is exactly what
// makes touching it from that thread safe.

namespace ac3::coreaudio {

// A CFTypeRef owned by scope - CFStringRef (device UID/name) is the only one
// this backend ever gets back from a property read, but the release call is
// the same for every CF type, so this stays generic rather than a
// CFStringRef-only wrapper.
template <typename T>
class CFOwned {
public:
    CFOwned() = default;
    explicit CFOwned(T value) : value_(value) {}
    ~CFOwned() { reset(); }
    CFOwned(const CFOwned&) = delete;
    CFOwned& operator=(const CFOwned&) = delete;

    [[nodiscard]] T get() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

    void reset() {
        if (value_ != nullptr) {
            CFRelease(value_);
            value_ = nullptr;
        }
    }

private:
    T value_ = nullptr;
};

// A CFStringRef, converted the only way CoreFoundation offers one that is
// always correct: ask for its length, allocate, and copy.
// CFStringGetCStringPtr is deliberately not used - it is documented to
// return null whenever the string is not already stored internally as plain
// C-string bytes, which is common enough (any string built from a format
// string, which is exactly what a driver's friendly-name property often is)
// that relying on it drops real device names.
[[nodiscard]] inline std::string to_utf8(CFStringRef value) {
    if (value == nullptr) {
        return {};
    }
    const CFIndex length = CFStringGetLength(value);
    const CFIndex max_bytes = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(max_bytes), '\0');
    if (CFStringGetCString(value, out.data(), max_bytes, kCFStringEncodingUTF8) == 0) {
        return {};
    }
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

[[nodiscard]] inline AudioObjectPropertyAddress address(
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

// A fixed-size property, e.g. a Float64 rate or a pid_t hog owner.
template <typename T>
[[nodiscard]] std::optional<T> get_property(AudioObjectID object,
                                            const AudioObjectPropertyAddress& addr) {
    T value{};
    UInt32 size = static_cast<UInt32>(sizeof(T));
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, &value) != noErr) {
        return std::nullopt;
    }
    return value;
}

// Deliberately not [[nodiscard]], unlike get_property/get_property_array:
// every caller that commits a format or a hog-mode/mixing flag checks this,
// but the two revert paths (PassthroughSink::stop() putting a stream's
// physical format back, HogGuard/MixingGuard's own reset()) call it as a
// best-effort cleanup step with nothing left to do if it fails - there is no
// more permission to ask for once a caller is already unwinding.
template <typename T>
bool set_property(AudioObjectID object, const AudioObjectPropertyAddress& addr, const T& value) {
    return AudioObjectSetPropertyData(object, &addr, 0, nullptr, static_cast<UInt32>(sizeof(T)),
                                      &value) == noErr;
}

// A variable-length property - an array of AudioObjectID/AudioStreamID, or
// of AudioStreamRangedDescription. Sized by asking the HAL how big it is
// first, the standard two-call idiom every property-array read here builds
// on.
template <typename T>
[[nodiscard]] std::vector<T> get_property_array(AudioObjectID object,
                                                 const AudioObjectPropertyAddress& addr) {
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(object, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return {};
    }
    std::vector<T> values(size / sizeof(T));
    if (AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, values.data()) != noErr) {
        return {};
    }
    values.resize(size / sizeof(T));
    return values;
}

[[nodiscard]] inline std::string device_uid(AudioObjectID device) {
    const auto value = get_property<CFStringRef>(device, address(kAudioDevicePropertyDeviceUID));
    if (!value || *value == nullptr) {
        return {};
    }
    const CFOwned<CFStringRef> owned{*value};
    return to_utf8(owned.get());
}

[[nodiscard]] inline std::string device_name(AudioObjectID device) {
    const auto value = get_property<CFStringRef>(device, address(kAudioObjectPropertyName));
    if (!value || *value == nullptr) {
        return {};
    }
    const CFOwned<CFStringRef> owned{*value};
    return to_utf8(owned.get());
}

// Total channel count across every buffer kAudioDevicePropertyStreamConfiguration
// reports for `scope` (kAudioDevicePropertyScopeInput or …Output) - zero for
// a device with no capability in that direction, which is how this backend
// tells an input-only device from an output-only one and skips a dead or
// disconnected object that answers with an empty AudioBufferList.
[[nodiscard]] inline std::uint32_t channel_count(AudioObjectID device,
                                                  AudioObjectPropertyScope scope) {
    const auto addr = address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0) {
        return 0;
    }
    // Backed by std::max_align_t rather than std::byte: AudioBufferList's
    // trailing AudioBuffer array needs at least UInt32/pointer alignment,
    // and casting up from a byte buffer to something that alignment-
    // sensitive is exactly what -Wcast-align exists to catch. max_align_t
    // has the strictest fundamental alignment on the platform by
    // definition, so this cast only ever narrows the alignment guarantee,
    // never widens it.
    std::vector<std::max_align_t> storage(
        (size + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
    auto* list = reinterpret_cast<AudioBufferList*>(storage.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) != noErr) {
        return 0;
    }
    std::uint32_t channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i) {
        channels += list->mBuffers[i].mNumberChannels;
    }
    return channels;
}

[[nodiscard]] inline Float64 nominal_sample_rate(AudioObjectID device) {
    return get_property<Float64>(device, address(kAudioDevicePropertyNominalSampleRate))
        .value_or(0.0);
}

[[nodiscard]] inline std::vector<AudioObjectID> device_list() {
    return get_property_array<AudioObjectID>(kAudioObjectSystemObject,
                                             address(kAudioHardwarePropertyDevices));
}

[[nodiscard]] inline AudioObjectID default_device(bool input) {
    return get_property<AudioObjectID>(
               kAudioObjectSystemObject,
               address(input ? kAudioHardwarePropertyDefaultInputDevice
                             : kAudioHardwarePropertyDefaultOutputDevice))
        .value_or(kAudioObjectUnknown);
}

// The AudioObjectID a persistent device UID names, or kAudioObjectUnknown if
// none of the machine's current devices carry it - a device unplugged since
// its id was captured, most often.
[[nodiscard]] inline AudioObjectID device_for_uid(const std::string& uid) {
    if (uid.empty()) {
        return kAudioObjectUnknown;
    }
    const CFOwned<CFStringRef> cf_uid{
        CFStringCreateWithCString(kCFAllocatorDefault, uid.c_str(), kCFStringEncodingUTF8)};
    if (!cf_uid) {
        return kAudioObjectUnknown;
    }
    CFStringRef in_string = cf_uid.get();
    AudioObjectID out_device = kAudioObjectUnknown;
    AudioValueTranslation translation{};
    translation.mInputData = &in_string;
    translation.mInputDataSize = static_cast<UInt32>(sizeof(in_string));
    translation.mOutputData = &out_device;
    translation.mOutputDataSize = static_cast<UInt32>(sizeof(out_device));

    const auto addr = address(kAudioHardwarePropertyDeviceForUID);
    UInt32 size = static_cast<UInt32>(sizeof(translation));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
                                   &translation) != noErr) {
        return kAudioObjectUnknown;
    }
    return out_device;
}

[[nodiscard]] inline std::vector<AudioStreamID> device_streams(AudioObjectID device,
                                                                AudioObjectPropertyScope scope) {
    return get_property_array<AudioStreamID>(device, address(kAudioDevicePropertyStreams, scope));
}

[[nodiscard]] inline std::vector<AudioStreamRangedDescription> available_physical_formats(
    AudioStreamID stream) {
    return get_property_array<AudioStreamRangedDescription>(
        stream, address(kAudioStreamPropertyAvailablePhysicalFormats));
}

// Polls a stream's CURRENT physical format until it matches `format_id`/
// `sample_rate` or `timeout` passes.
//
// A set on kAudioStreamPropertyPhysicalFormat is documented as asynchronous
// - AudioObjectSetPropertyData can return noErr before the driver has
// actually retuned the hardware - and every real implementation surveyed
// while writing this backend (mpv's ca_change_physical_format_sync,
// MythTV's audiooutputca.cpp) confirms the change with a follow-up read
// rather than trusting the set call's own result. A polling read rather
// than an AudioObjectAddPropertyListener callback: the wait only ever
// happens once, synchronously, inside start() before anything touches the
// ring buffer, so there is no realtime-thread constraint here to justify a
// listener's extra moving parts (a second callback, a mutex/condition-
// variable pair to hand its result back) over a few short sleeps on the
// thread already blocked in start().
//
// Not [[nodiscard]] - see set_property's own comment just above: the same
// two revert paths call this as a best-effort "did it settle back" check
// with no further action to take either way.
inline bool wait_for_physical_format(AudioStreamID stream, AudioFormatID format_id,
                                     Float64 sample_rate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto addr = address(kAudioStreamPropertyPhysicalFormat);
    do {
        const auto current = get_property<AudioStreamBasicDescription>(stream, addr);
        if (current && current->mFormatID == format_id &&
            std::abs(current->mSampleRate - sample_rate) < 1.0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

// Same idea as wait_for_physical_format, for monitor.cpp's nominal-rate
// negotiation (see that file's own comment on why it needs one at all).
[[nodiscard]] inline bool wait_for_nominal_rate(AudioObjectID device, Float64 rate,
                                                std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (std::abs(nominal_sample_rate(device) - rate) < 1.0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

// Exclusive access - ALSA/WASAPI's other name for what CoreAudio calls hog
// mode: kAudioDevicePropertyHogMode holds the pid_t of whichever process
// currently owns the device, or -1 when nobody does. Released on
// destruction unless the object it lives in is destroyed after a clean
// reset(), so a start() that fails partway through cannot leave the device
// hogged with nothing left to give it back.
class HogGuard {
public:
    HogGuard() = default;
    ~HogGuard() { reset(); }
    HogGuard(const HogGuard&) = delete;
    HogGuard& operator=(const HogGuard&) = delete;

    // True if this call is the one that took ownership; false if the device
    // was already hogged by some other process (its pid, read first, is
    // neither -1 nor ours) or the set itself failed.
    [[nodiscard]] bool acquire(AudioObjectID device) {
        const auto addr = address(kAudioDevicePropertyHogMode);
        const auto current = get_property<pid_t>(device, addr);
        const pid_t self = getpid();
        if (current && *current == self) {
            // Already ours - start() cannot reach this in practice (running()
            // is checked first), but acquiring twice must not release what
            // the first acquire() owns.
            device_ = device;
            owned_ = true;
            return true;
        }
        if (current && *current != -1) {
            return false;
        }
        if (!set_property(device, addr, self)) {
            return false;
        }
        device_ = device;
        owned_ = true;
        return true;
    }

    void reset() {
        if (owned_) {
            const pid_t none = -1;
            set_property(device_, address(kAudioDevicePropertyHogMode), none);
            owned_ = false;
        }
    }

private:
    AudioObjectID device_ = kAudioObjectUnknown;
    bool owned_ = false;
};

// kAudioDevicePropertySupportsMixing: best-effort, matching mpv's own
// ca_disable_mixing/ca_enable_mixing - a device with no mixer to turn off
// (most digital outputs opened for passthrough do not have one) simply
// leaves the set call failing silently, which is fine: mixing was never
// going to corrupt the bitstream on that device because there is no mixer
// stage to do it.
class MixingGuard {
public:
    MixingGuard() = default;
    ~MixingGuard() { reset(); }
    MixingGuard(const MixingGuard&) = delete;
    MixingGuard& operator=(const MixingGuard&) = delete;

    void disable(AudioObjectID device) {
        const auto addr = address(kAudioDevicePropertySupportsMixing);
        Boolean settable = 0;
        if (AudioObjectIsPropertySettable(device, &addr, &settable) != noErr || settable == 0) {
            return;
        }
        const UInt32 off = 0;
        if (set_property(device, addr, off)) {
            device_ = device;
            disabled_ = true;
        }
    }

    void reset() {
        if (disabled_) {
            const UInt32 on = 1;
            set_property(device_, address(kAudioDevicePropertySupportsMixing), on);
            disabled_ = false;
        }
    }

private:
    AudioObjectID device_ = kAudioObjectUnknown;
    bool disabled_ = false;
};

}  // namespace ac3::coreaudio
