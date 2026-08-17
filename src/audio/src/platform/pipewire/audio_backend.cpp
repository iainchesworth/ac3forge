#include "ac3/platform/audio_backend.hpp"

// PipeWire: all three capabilities are real, over the native pw_stream API -
// not PipeWire's ALSA-compatibility shim, and not a fallback that quietly
// reaches for libasound instead. capture.cpp and monitor.cpp are ordinary
// PCM, PipeWire's flagship case. passthrough.cpp is the one worth a longer
// account, in its own file's header comment: PipeWire has a real, current,
// documented compressed-format path (SPA_MEDIA_SUBTYPE_iec958,
// spa_format_audio_iec958_build(), PW_STREAM_FLAG_EXCLUSIVE), but whether any
// given Audio/Sink node will actually accept it is a per-node question this
// library cannot answer by reading a property - it has to ask, the same way
// ALSA's own passthrough.cpp has no IsFormatSupported() and has to open a
// device to find out. See passthrough.cpp for the rest.
//
// "Available" carries the same meaning it does on every other backend: the
// library was built against libpipewire-0.3, not that a PipeWire session is
// running on this particular machine, or that any of its outputs happen to
// have compressed passthrough enabled. enumerate_devices() and
// enumerate_render_devices() answer the per-machine, per-device questions;
// a machine with no PipeWire session running enumerates nothing, the same
// honest empty list a machine with no ALSA sound card returns.

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
