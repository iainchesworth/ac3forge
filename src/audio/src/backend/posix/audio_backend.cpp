#include "ac3/platform/audio_backend.hpp"

// This is now reachable only as ac3::audio's Linux fallback, when
// libasound's development headers are not present (see the AC3FORGE_WITH_ALSA
// AUTO/ON/OFF block in src/audio/CMakeLists.txt) - macOS gets a real
// CoreAudio backend of its own, src/audio/src/backend/macos/, unconditionally.
//
// None of the three capabilities exist here, and on a Linux host without
// ALSA that is a real absence rather than an oversight: live capture would
// add no coverage the WAV path does not already give, exclusive-mode IEC
// 61937 is the expensive half (it needs a device that will accept a
// non-PCM format under exclusive access, unverifiable without hardware on
// the other end of an optical or HDMI cable), and shared-mode monitor
// playback needs the same ALSA/PipeWire integration capture does.
//
// 'ac3cli spdif' is the substitute and needs no backend anywhere: it wraps
// frames into the same IEC 61937 bursts and writes them as a PCM16 WAV, which
// any player will push through a passthrough-capable output bit-exactly.
//
// These strings are printed verbatim when a caller is turned away, so they
// name what is missing rather than merely reporting that something is.

namespace ac3::platform {

const AudioBackend& audio_backend() {
    static constexpr AudioBackend kBackend{
        .capture = {.available = false,
                    .reason = "this build has no capture backend: install libasound2-dev "
                              "(Debian/Ubuntu) or alsa-lib-devel (Fedora) and reconfigure"},
        .passthrough = {.available = false,
                        .reason = "this build has no passthrough backend: exclusive-mode "
                                  "IEC 61937 needs direct ALSA hw: access, which needs "
                                  "libasound2-dev (Debian/Ubuntu) or alsa-lib-devel (Fedora)"},
        .monitor = {.available = false,
                   .reason = "this build has no monitor backend: shared-mode PCM playback "
                             "needs ALSA/PipeWire, which needs libasound2-dev (Debian/Ubuntu) "
                             "or alsa-lib-devel (Fedora)"},
    };
    return kBackend;
}

}  // namespace ac3::platform
