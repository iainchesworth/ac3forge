#include "ac3/platform/audio_backend.hpp"

// Android: capture is absent by design (see capture.cpp); passthrough and
// monitor are both real, but by two different mechanisms, not one:
//
//  - monitor.cpp is genuine AAudio (the NDK's native audio API) - shared-mode
//    float PCM playback is exactly what AAudio is for, and it needs no Java.
//  - passthrough.cpp is NOT AAudio, despite that being the obvious guess for
//    an "NDK audio backend". AAudio has no support for compressed/IEC 61937
//    bitstream passthrough - it is a PCM API end to end, and would apply
//    volume scaling/format conversion that corrupts a bitstream. HDMI
//    passthrough on Android, Shield TV included, only exists through the
//    Java android.media.AudioTrack class (ENCODING_E_AC3/ENCODING_IEC61937),
//    so passthrough.cpp is a thin JNI shim to a Kotlin-owned AudioTrack, not
//    a native audio call. See docs/platforms/android.md for the full
//    background and for why this is also how Kodi/ExoPlayer do it.
//
// Both report available=true here because the backend is compiled in; per
// the Windows backend's convention, no Capability carries a reason when
// available is true. Whether a SPECIFIC receiver on the other end of the
// HDMI cable actually accepts E-AC-3/Atmos is a runtime question answered by
// enumerate_render_devices()'s RenderDeviceInfo, not by this static report -
// exactly as WASAPI's IsFormatSupported answers it per-device on Windows.

namespace ac3::platform {

const AudioBackend& audio_backend() {
    static constexpr AudioBackend kBackend{
        .capture = {.available = false,
                    .reason = "this build has no capture backend: the app this backend serves "
                              "has no microphone/loopback feature, so none was implemented"},
        .passthrough = {.available = true, .reason = {}},
        .monitor = {.available = true, .reason = {}},
    };
    return kBackend;
}

}  // namespace ac3::platform
