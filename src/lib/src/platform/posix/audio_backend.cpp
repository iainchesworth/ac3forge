#include "ac3/platform/audio_backend.hpp"

// Unix: none of the three capabilities exist, and that is a decision rather
// than an oversight. ac3forge's subject is the bitstream - everything that
// encodes, decodes, measures or wraps AC-3 is file I/O and runs identically
// here. Live capture would add no coverage the WAV path does not already
// give, exclusive-mode IEC 61937 is the expensive half (it needs a device
// that will accept a non-PCM format under exclusive access, unverifiable
// without hardware on the other end of an optical or HDMI cable), and shared
// -mode monitor playback needs the same ALSA/PipeWire/CoreAudio integration
// capture does.
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
                    .reason = "this build has no capture backend: ALSA/PipeWire (Linux) and "
                              "CoreAudio (macOS) are not implemented"},
        .passthrough = {.available = false,
                        .reason = "this build has no passthrough backend: exclusive-mode "
                                  "IEC 61937 needs direct ALSA hw: access or CoreAudio hog "
                                  "mode, and neither is implemented"},
        .monitor = {.available = false,
                   .reason = "this build has no monitor backend: shared-mode PCM playback "
                             "needs ALSA/PipeWire (Linux) or CoreAudio (macOS), and neither "
                             "is implemented"},
    };
    return kBackend;
}

}  // namespace ac3::platform
