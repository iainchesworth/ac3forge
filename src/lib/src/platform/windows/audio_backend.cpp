#include "ac3/platform/audio_backend.hpp"

// Windows: both capabilities are real. capture.cpp is WASAPI in shared mode
// (input endpoints plus render endpoints opened for loopback) and
// passthrough.cpp is WASAPI in exclusive mode with an IEC 61937 format, so
// neither Capability carries a reason - there is nothing to excuse.

namespace ac3::platform {

const AudioBackend& audio_backend() {
    static constexpr AudioBackend kBackend{
        .capture = {.available = true, .reason = {}},
        .passthrough = {.available = true, .reason = {}},
    };
    return kBackend;
}

}  // namespace ac3::platform
