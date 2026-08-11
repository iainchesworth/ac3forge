#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "matroska/export.hpp"

// A minimal Matroska (MKV) muxer, per the EBML and Matroska specifications.
//
// This is a container writer and nothing more: it knows how to nest EBML
// elements and lay out clusters, and it takes each frame as opaque bytes. It
// has NO dependency on ac3::forge and no knowledge of AC-3 - which is the
// point of keeping it a separate library. A caller muxing E-AC-3 hands over
// whole access units; a caller muxing something else hands over whatever its
// own frames are.
//
// Deliberately small. Enough to produce a file a player will open with the
// right channel layout and duration:
//   - one audio track,
//   - one SimpleBlock per frame, clusters closed on a time budget,
//   - Info carrying TimestampScale and Duration.
// No SeekHead, no Cues, no chapters, no tags. Those matter for seeking in
// large files, not for playing back what this project produces.

namespace matroska {

// Matroska CodecID strings (the Matroska codec mappings registry).
inline constexpr std::string_view kCodecEac3 = "A_EAC3";
inline constexpr std::string_view kCodecAc3 = "A_AC3";

enum class MuxError : std::uint8_t {
    kNoFrames,
    kInvalidTrack,   // zero/negative channels or sample rate, or an empty codec id
    kFrameTooLarge,  // a single frame beyond what one SimpleBlock can carry
};

[[nodiscard]] MATROSKA_EXPORT std::string_view describe(MuxError error);

struct AudioTrack {
    std::string codec_id{kCodecEac3};
    std::uint32_t sample_rate = 48000;
    int channels = 2;
    // Samples one frame represents, used to place block timestamps and to
    // compute the segment duration. An E-AC-3 access unit is 1536.
    std::uint32_t samples_per_frame = 1536;
    std::string language{"und"};
};

struct MuxOptions {
    // A cluster's SimpleBlocks carry a SIGNED 16-BIT timestamp relative to
    // their cluster, so a cluster can never span more than 32767 ms however
    // generous this is. One second is the conventional choice and keeps a
    // truncated file mostly playable.
    std::uint32_t cluster_ms = 1000;
    std::string writing_app{"ac3forge"};
};

// Mux frames into a complete .mkv, returned as bytes. No file I/O here, so
// this stays testable without touching a disk.
[[nodiscard]] MATROSKA_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options = {});

}  // namespace matroska
