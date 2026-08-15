#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "mp4/export.hpp"

// A minimal MP4 (ISOBMFF) muxer, per ISO/IEC 14496-12 (the ISO Base Media
// File Format).
//
// This is a container writer and nothing more, in exactly the sense
// matroska::matroska is: it knows how to nest ISOBMFF boxes and lay out one
// audio track's samples, and it takes each frame - and the codec's own
// configuration box payload - as opaque bytes. It has NO dependency on
// ac3::forge and no knowledge of AC-3: a caller muxing E-AC-3 hands over
// whole access units plus a ready-made 'dec3' payload (see
// ac3::io::build_codec_config_box, ac3/io/dec3.hpp); a caller muxing
// something else hands over whatever its own frames and sample-entry config
// box are. Keeping the codec-specific box payload opaque here is exactly why
// matroska::matroska stays codec-blind too, applied to the one place MP4
// needs codec-specific bytes that Matroska's plain CodecID string does not:
// see mp4::AudioTrack::codec_config below.
//
// Deliberately small. Enough to produce a file a player will open with the
// right channel layout, duration and codec signalling:
//   - one audio track, ftyp/moov/mdat only,
//   - one sample per chunk (no interleaving/multi-track concerns to solve),
//   - stts/stsz/stco built straight off the frame sizes handed in.
// No fragmentation, no edit lists, no multiple tracks. Those matter for
// streaming/adaptive delivery, not for playing back what this project
// produces - see ROADMAP.md's A2 for fMP4/CMAF as a deliberate follow-up.

namespace mp4 {

// ISOBMFF sample entry codes (ISO/IEC 14496-15 §5.5 registers 'ac-3'; ETSI
// TS 102 366 Annex F itself is what ties each to its dac3/dec3 box).
inline constexpr std::string_view kCodecAc3 = "ac-3";
inline constexpr std::string_view kCodecEac3 = "ec-3";

enum class MuxError : std::uint8_t {
    kNoFrames,
    kInvalidTrack,    // zero/negative channels or sample rate, unrecognised codec id, or no
                       // codec_config payload
    kFileTooLarge,     // mdat would need a 64-bit chunk offset (co64), unsupported in this cut
};

[[nodiscard]] MP4_EXPORT std::string_view describe(MuxError error);

struct AudioTrack {
    // Selects the sample entry box ('ac-3' or 'ec-3') and, through it, which
    // configuration box wraps `codec_config` ('dac3' or 'dec3' respectively -
    // see ETSI TS 102 366 Annex F). Any other value is kInvalidTrack: this
    // module only knows how to describe an AC-3/E-AC-3 sample entry, the same
    // way matroska::AudioTrack::codec_id is free-form but this one is not -
    // an MP4 sample entry's box layout genuinely depends on which codec it
    // is, unlike Matroska's CodecID string.
    std::string codec_id{kCodecEac3};
    std::uint32_t sample_rate = 48000;
    int channels = 2;
    // Samples one frame represents, used to build stts and to compute the
    // track's duration. An E-AC-3/AC-3 access unit is 1536.
    std::uint32_t samples_per_frame = 1536;
    // The sample entry's one child configuration box, PAYLOAD ONLY (this
    // module writes the box's own size+FourCC header, choosing 'dac3' or
    // 'dec3' from codec_id above). Opaque to this module by design - see
    // ac3::io::build_codec_config_box (ac3/io/dec3.hpp) for how AC-3/E-AC-3
    // callers produce it, and examples/mux_mp4.cpp for the full round trip.
    std::vector<std::byte> codec_config;
    std::string language{"und"};
};

struct MuxOptions {
    std::string writing_app{"ac3forge"};
};

// Mux frames into a complete .mp4, returned as bytes. No file I/O here, so
// this stays testable without touching a disk - matroska::mux()'s own reason
// applies unchanged.
[[nodiscard]] MP4_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options = {});

}  // namespace mp4
