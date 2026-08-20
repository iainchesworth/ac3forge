#include "ac3/audio/audio_backend.hpp"

// Windows: all three capabilities are real. capture.cpp is WASAPI in shared
// mode (input endpoints plus render endpoints opened for loopback),
// passthrough.cpp is WASAPI in exclusive mode with an IEC 61937 format, and
// monitor.cpp is WASAPI in shared mode for ordinary PCM playback, so no
// Capability carries a reason - there is nothing to excuse.

namespace ac3::audio {

const AudioBackend& audio_backend() {
    static constexpr AudioBackend kBackend{
        .capture = {.available = true, .reason = {}},
        .passthrough = {.available = true, .reason = {}},
        .monitor = {.available = true, .reason = {}},
    };
    return kBackend;
}

}  // namespace ac3::audio
