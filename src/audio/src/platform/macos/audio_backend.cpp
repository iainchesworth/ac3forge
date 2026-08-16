#include "ac3/platform/audio_backend.hpp"

// macOS: all three capabilities are real. capture.cpp reads from any HAL
// input device via AudioDeviceCreateIOProcID, monitor.cpp plays ordinary
// float PCM back through a HAL output device the same way, and
// passthrough.cpp takes hog mode on a digital (HDMI/optical) output and
// retunes its physical stream format to a compressed IEC 60958 format
// (kAudioFormat60958AC3/kAudioFormatEnhancedAC3) before feeding it raw
// bursts - see that file's own header comment for the full mechanism, which
// is genuinely different from both WASAPI's exclusive-mode subformat and
// ALSA's channel-status device names. None of the three carries a reason,
// because there is nothing to excuse.
//
// "Available" means the same thing here as it does on Windows/Linux: the
// library was built with a backend, not that this particular machine has
// hardware that will do it - a MacBook with no digital output enumerates
// zero passthrough-capable devices rather than reporting itself unavailable,
// the same as ALSA's own documented behaviour for a machine with only an
// analog output.

namespace ac3::platform {

const AudioBackend& audio_backend() {
    static constexpr AudioBackend kBackend{
        .capture = {.available = true, .reason = {}},
        .passthrough = {.available = true, .reason = {}},
        .monitor = {.available = true, .reason = {}},
    };
    return kBackend;
}

}  // namespace ac3::platform
