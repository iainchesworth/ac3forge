#pragma once

#include <cstddef>
#include <cstdint>

#include "ac3/sinks/iec61937.hpp"
#include "ac3/sinks/passthrough.hpp"

// The Android backend's pure half, split out for the same reason
// platform/alsa/device_names.hpp is: everything here compiles and tests on a
// machine with no Android SDK/NDK at all, unlike the rest of this directory
// (passthrough.cpp, monitor.cpp), which needs <jni.h>/<aaudio/AAudio.h> and a
// real Android toolchain. Not part of the library's public interface -
// backend-internal, like alsa_support.hpp/device_names.hpp are for ALSA.

namespace ac3::android_audio {

// Mirrors platform/windows/passthrough.cpp's burst_bytes_for and
// platform/alsa's equivalent: which IEC 61937 burst size a format uses.
// AudioTrack.write() needs the caller (submit()) to know this up front so it
// can validate the burst it was handed before ever touching JNI.
[[nodiscard]] inline std::size_t burst_bytes_for(sinks::BitstreamFormat format) {
    return format == sinks::BitstreamFormat::kEac3 ? iec61937::kEac3BurstBytes
                                                    : iec61937::kBurstBytes;
}

// Mirrors platform/alsa/device_names.hpp's carrier_rate exactly (same
// physical fact, not a coincidence of naming): a Dolby Digital Plus burst is
// four times the size of an AC-3 one (kEac3BurstBytes vs kBurstBytes) and
// covers the same span of time, so the link has to clock four times as fast
// to deliver it - Microsoft's "Representing Formats for IEC 61937
// Transmissions" states it as a requirement, and it is not an
// WASAPI-specific one. The Kotlin PassthroughBridge's AudioTrack is opened
// with AudioFormat.ENCODING_IEC61937 (pre-wrapped bursts straight through,
// not ENCODING_E_AC3's "raw elementary frames, Android wraps them" contract
// - see passthrough.cpp's header comment for why), which is exactly the
// same "the declared rate describes the wire, not the content" situation
// ALSA/WASAPI are already in - so PassthroughSink::start() passes THIS
// value to PassthroughBridge.open(), not the raw content sample_rate.
[[nodiscard]] constexpr std::uint32_t carrier_rate(sinks::BitstreamFormat format,
                                                    std::uint32_t content_rate) {
    return format == sinks::BitstreamFormat::kEac3 ? content_rate * 4 : content_rate;
}

// The Kotlin side's PassthroughBridge.probeCapabilities(int) returns
// [ac3Supported, eac3Supported, pcmSupported] as 0/1 flags (see
// passthrough.cpp's header comment for the full contract) - this is the pure
// half of turning that into the RenderDeviceInfo enumerate_render_devices()
// hands back. Android has exactly one addressable output route (there is no
// WASAPI-style per-endpoint enumeration to walk - see
// enumerate_render_devices()'s own comment), so the id/name/is_default
// fields are fixed rather than read from anywhere.
[[nodiscard]] inline sinks::RenderDeviceInfo make_render_device_info(bool ac3_supported,
                                                                      bool eac3_supported,
                                                                      bool pcm_supported) {
    sinks::RenderDeviceInfo info;
    info.id = "default";
    info.name = "Android system audio output (HDMI-routed)";
    info.is_default = true;
    info.supports_ac3_passthrough = ac3_supported;
    info.supports_eac3_passthrough = eac3_supported;
    info.supports_exclusive_pcm = pcm_supported;
    return info;
}

}  // namespace ac3::android_audio
