#pragma once

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>

#include "ac3/audio/passthrough.hpp"

// The macOS backend's pure half: everything here is ordinary arithmetic and
// struct inspection over types <CoreAudio/CoreAudio.h> defines, with no
// AudioObjectGetPropertyData/AudioObjectSetPropertyData call - or any other
// live round-trip to the HAL - anywhere in the file. The include itself is
// the full umbrella header rather than the narrower <CoreAudio/CoreAudioTypes.h>
// only because AudioStreamRangedDescription (find_physical_format's own
// currency) is declared in AudioHardwareBase.h, not CoreAudioTypes.h - a
// function-declarations-only header is not a live device the way an actual
// AudioObjectGetPropertyData call would be, so this file's claim to being
// "pure" is about behaviour (nothing here ever touches a device), not about
// which sub-header a type happens to live in.
//
// Unlike platform/alsa/device_names.hpp, whose whole point is testability on
// a machine that might not have libasound installed, what this file is pure
// OF is a live HAL round-trip, not a library that might be missing:
// CoreAudio.framework's headers ship with every macOS SDK, so there is no
// "not installed" story here to design around. tests/backend/macos/
// test_macos_support.cpp covers this file directly, on real macOS CI, the
// same role tests/backend/alsa/test_alsa_device_names.cpp plays for
// device_names.hpp.
//
// coreaudio_support.hpp next door is the impure half: property fetching,
// device enumeration, hog mode, and the async wait a physical-format or
// nominal-rate change needs before it can be trusted.

namespace ac3::coreaudio {

// Mirrors platform/alsa/device_names.hpp's carrier_rate and
// apps/android/android_support.hpp's copy of the same logic (the same
// physical fact each time, not a coincidence of naming): a Dolby Digital
// Plus burst is four times the size of an AC-3 one and covers the same span
// of time, so the digital link has to clock four times as fast to deliver
// it - Microsoft's "Representing Formats for IEC 61937 Transmissions"
// states it as a general IEC 61937 fact, not a WASAPI-specific one, and
// CoreAudio's kAudioStreamPropertyPhysicalFormat mSampleRate field describes
// the wire in exactly the sense WASAPI's WAVEFORMATEXTENSIBLE and ALSA's
// device-name rate both do.
[[nodiscard]] constexpr std::uint32_t carrier_rate(audio::BitstreamFormat format,
                                                    std::uint32_t content_rate) {
    return format == audio::BitstreamFormat::kEac3 ? content_rate * 4 : content_rate;
}

// The physical-format fourCC a digital output stream is asked for.
//
// kAudioFormat60958AC3 ('cac3') is long-documented and exercised by every
// real-world CoreAudio passthrough implementation surveyed while writing
// this backend (MythTV's audiooutputca.cpp, mpv's ao_coreaudio_exclusive.c
// and VLC's auhal.c all probe for it the same way, via
// kAudioStreamPropertyAvailablePhysicalFormats). kAudioFormatEnhancedAC3
// ('ec-3') has no comparably long history as a *physical* (IEC 60958-wrapped)
// stream format the way kAudioFormat60958AC3 does - it is the same fourCC
// ac3::io::build_codec_config_box uses for a raw E-AC-3 *elementary* stream
// in an MP4 sample entry, not a documented S/PDIF/HDMI wire format. Apple's
// own support documentation confirms Dolby Digital Plus/Atmos HDMI
// passthrough exists on Apple Silicon Macs without documenting the HAL
// mechanism behind it, so this backend probes for kAudioFormatEnhancedAC3
// exactly as it does kAudioFormat60958AC3; on hardware or macOS versions
// where the driver does not publish it, supports_eac3_passthrough simply
// comes back false, the honest answer under the same "a platform can gain
// one and not the other" contract ac3::audio::RenderDeviceInfo already documents.
[[nodiscard]] constexpr AudioFormatID physical_format_id(audio::BitstreamFormat format) {
    return format == audio::BitstreamFormat::kEac3 ? kAudioFormatEnhancedAC3
                                                     : kAudioFormat60958AC3;
}

// Whether `formats` (as kAudioStreamPropertyAvailablePhysicalFormats returns
// them) includes `format_id` at `carrier_hz`, and if so, the exact
// AudioStreamBasicDescription the driver published for it.
//
// Returning the driver's own descriptor rather than hand-building one (the
// way platform/windows/passthrough.cpp's make_ac3_format/make_eac3_format
// construct a WAVEFORMATEXTENSIBLE_IEC61937 field by field) matters here
// specifically: WASAPI's IEC 61937 subformat GUID is a fixed, Microsoft-
// documented layout, but a physical AudioStreamBasicDescription is whatever
// the driver decided to publish - mBytesPerPacket/mFramesPerPacket/
// mBitsPerChannel can legitimately differ between drivers for the same
// compressed format (some report them as 0, "not meaningful for a non-PCM
// stream"). Handing kAudioStreamPropertyPhysicalFormat back exactly what
// kAudioStreamPropertyAvailablePhysicalFormats offered is the only way to be
// sure the fields a fussier driver does check survive intact.
//
// A small tolerance on the rate match, not exact equality: mSampleRateRange
// is a Float64 range and some drivers report it as [carrier, carrier] with
// the double arithmetic not landing on an exact bit pattern.
[[nodiscard]] inline std::optional<AudioStreamBasicDescription> find_physical_format(
    std::span<const AudioStreamRangedDescription> formats, AudioFormatID format_id,
    Float64 carrier_hz) {
    constexpr Float64 kTolerance = 1.0;
    for (const auto& candidate : formats) {
        if (candidate.mFormat.mFormatID != format_id) {
            continue;
        }
        if (carrier_hz >= candidate.mSampleRateRange.mMinimum - kTolerance &&
            carrier_hz <= candidate.mSampleRateRange.mMaximum + kTolerance) {
            return candidate.mFormat;
        }
    }
    return std::nullopt;
}

// A display name that is never empty, matching the Windows/ALSA backends'
// own fallback discipline (endpoint_display_name / the "default" entry) so
// a blank row in a device list is always a rendering bug rather than data
// this backend produced.
[[nodiscard]] inline std::string fallback_name(const std::string& uid) {
    return uid.empty() ? std::string{"Unnamed audio endpoint"} : "Unnamed endpoint " + uid;
}

// ---------------------------------------------------------------------------
// Ordinary (non-compressed) PCM: what capture.cpp reads and monitor.cpp
// writes.
//
// A HAL device's *virtual* format (kAudioDevicePropertyStreamFormat) is
// almost always 32-bit float on modern macOS, but the 16/32-bit integer
// cases are supported for the same reason the Windows/ALSA backends' own
// classify()/convert() pairs support them: a device that reports something
// else is a real, if rare, machine this should still work on rather than
// silently misread.
// ---------------------------------------------------------------------------

enum class SampleFormat : std::uint8_t { kFloat32, kPcm16, kPcm32, kUnsupported };

[[nodiscard]] constexpr std::size_t bytes_per_sample(SampleFormat format) {
    switch (format) {
        case SampleFormat::kFloat32: return 4;
        case SampleFormat::kPcm16: return 2;
        case SampleFormat::kPcm32: return 4;
        case SampleFormat::kUnsupported: return 0;
    }
    return 0;
}

[[nodiscard]] inline SampleFormat classify_pcm(const AudioStreamBasicDescription& asbd) {
    if (asbd.mFormatID != kAudioFormatLinearPCM) {
        return SampleFormat::kUnsupported;
    }
    const bool is_float = (asbd.mFormatFlags & kAudioFormatFlagIsFloat) != 0;
    const bool is_signed_int = (asbd.mFormatFlags & kAudioFormatFlagIsSignedInteger) != 0;
    if (is_float && asbd.mBitsPerChannel == 32) {
        return SampleFormat::kFloat32;
    }
    if (is_signed_int && asbd.mBitsPerChannel == 16) {
        return SampleFormat::kPcm16;
    }
    if (is_signed_int && asbd.mBitsPerChannel == 32) {
        return SampleFormat::kPcm32;
    }
    return SampleFormat::kUnsupported;
}

// Raw device samples turned into normalised float - the read side of the
// pair, mirroring platform/alsa/capture.cpp's own convert().
inline void samples_to_float(const std::byte* data, std::size_t count, SampleFormat format,
                             std::span<float> out) {
    switch (format) {
        case SampleFormat::kFloat32:
            std::memcpy(out.data(), data, count * sizeof(float));
            break;
        case SampleFormat::kPcm16:
            for (std::size_t i = 0; i < count; ++i) {
                std::int16_t value = 0;
                std::memcpy(&value, data + i * 2, sizeof(value));
                out[i] = static_cast<float>(value) / 32768.0f;
            }
            break;
        case SampleFormat::kPcm32:
            for (std::size_t i = 0; i < count; ++i) {
                std::int32_t value = 0;
                std::memcpy(&value, data + i * 4, sizeof(value));
                out[i] = static_cast<float>(value) / 2147483648.0f;
            }
            break;
        case SampleFormat::kUnsupported:
            std::ranges::fill(out, 0.0f);
            break;
    }
}

// Normalised float turned into raw device samples - the write side of the
// pair, mirroring platform/alsa/monitor.cpp's own convert(). Clamped for the
// same reason that one is: a decoded sample slightly outside [-1, 1] should
// sound loud, not wrap around to the opposite polarity and click.
inline void float_to_samples(std::span<const float> in, SampleFormat format, std::byte* out) {
    switch (format) {
        case SampleFormat::kFloat32:
            std::memcpy(out, in.data(), in.size() * sizeof(float));
            break;
        case SampleFormat::kPcm16:
            for (std::size_t i = 0; i < in.size(); ++i) {
                const float clamped = std::clamp(in[i], -1.0f, 1.0f);
                const auto value = static_cast<std::int16_t>(clamped * 32767.0f);
                std::memcpy(out + i * 2, &value, sizeof(value));
            }
            break;
        case SampleFormat::kPcm32:
            for (std::size_t i = 0; i < in.size(); ++i) {
                const float clamped = std::clamp(in[i], -1.0f, 1.0f);
                const auto value = static_cast<std::int32_t>(clamped * 2147483520.0f);
                std::memcpy(out + i * 4, &value, sizeof(value));
            }
            break;
        case SampleFormat::kUnsupported:
            break;
    }
}

}  // namespace ac3::coreaudio
