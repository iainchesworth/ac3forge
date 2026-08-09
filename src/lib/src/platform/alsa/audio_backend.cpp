#include "ac3/platform/audio_backend.hpp"

// ALSA: all three capabilities are real. capture.cpp reads from any ALSA PCM -
// including `default`, which on a desktop is PipeWire or PulseAudio -
// monitor.cpp plays ordinary PCM back through the same, and passthrough.cpp
// opens a card's S/PDIF or HDMI output directly with the IEC 60958 non-audio
// bit set. None of the three carries a reason, because there is nothing to
// excuse.
//
// "Available" means the same thing here as it does on Windows: the library was
// built with a backend, not that this particular machine has hardware that
// will do it. Whether a given output can carry a bitstream is a per-device
// question, and enumerate_render_devices() is where it is answered - a machine
// with only an analog output enumerates it with supports_ac3_passthrough
// false. A machine with no sound card at all - a container, a CI runner -
// enumerates nothing and gets an empty list, not an error.

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
