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
// this stays testable without touching a disk. Frames arrive as views, so
// io::scan's access units pass straight through - the owned copies the old
// vector-list parameter forced on every slicing caller are gone.
[[nodiscard]] MATROSKA_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::span<const std::byte>> frames,
    const MuxOptions& options = {});

// Owned-frame-list convenience, forwarding as views - for a caller that
// built its frames (an encode loop) rather than sliced them from a stream.
[[nodiscard]] MATROSKA_EXPORT std::expected<std::vector<std::byte>, MuxError> mux(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const MuxOptions& options = {});

// Incrementally muxes frames into Matroska as they arrive, for a session
// whose length is not known up front - a live capture, where mux() above
// cannot help: it needs every frame before it can compute anything (the
// Duration element, and how many clusters to lay out). Matroska is designed
// to be streamable, so this needs no invention: Segment is written with
// EBML's own reserved "unknown size" pattern (a size vint whose value bits
// are all ones), the standard way a streamed Matroska declares a length it
// cannot know yet - real players (and other streaming muxers, e.g. a live
// WebM recording) already handle this. Duration is omitted from Info for the
// same reason - there is nothing to put in it yet. Everything else - Tracks,
// cluster layout, SimpleBlock framing - is identical to mux().
//
// No more than one cluster's worth of frames (`options.cluster_ms`, a second
// by default) is ever held at once, so a caller streaming push()'s returned
// bytes straight to disk keeps memory bounded for a session of any length -
// the property mux() cannot offer, since it needs the whole frame list
// resident to call it at all.
//
// No file I/O here either, matching mux() above: header(), push() and
// finalize() hand back bytes for the caller to write; this class never
// touches a disk, which is what keeps it testable without one.
class MATROSKA_EXPORT Writer {
public:
    // Validates the track the same way mux() does. On success, header()
    // already holds the EBML header through Tracks - everything before the
    // first cluster.
    [[nodiscard]] static std::expected<Writer, MuxError> create(const AudioTrack& track,
                                                                 const MuxOptions& options = {});

    // Write this exactly once, before any bytes push() or finalize() return.
    [[nodiscard]] const std::vector<std::byte>& header() const { return header_; }

    // Buffers one frame into the writer's current (in-progress) cluster.
    // Returns the bytes of a cluster that just CLOSED to make room for this
    // frame - empty on most calls, since a cluster spans about
    // `options.cluster_ms` of audio; write whatever comes back, in order, as
    // it comes back.
    [[nodiscard]] std::expected<std::vector<std::byte>, MuxError> push(
        std::span<const std::byte> frame);

    // Flushes whatever partial cluster remains - call exactly once, when the
    // session ends. Nothing else needs closing: Segment's size is unknown by
    // design, so there is no length field left to patch. Safe to call with
    // zero frames pushed (returns empty).
    [[nodiscard]] std::vector<std::byte> finalize();

    [[nodiscard]] std::size_t frames_written() const { return index_; }

private:
    Writer(AudioTrack track, MuxOptions options, std::vector<std::byte> header);

    [[nodiscard]] std::uint64_t stamp_ms(std::size_t index) const;
    [[nodiscard]] std::vector<std::byte> close_cluster();

    AudioTrack track_;
    MuxOptions options_;
    std::size_t index_ = 0;
    std::vector<std::byte> header_;
    std::vector<std::byte> cluster_body_;
    std::uint64_t cluster_base_ms_ = 0;
    bool cluster_open_ = false;
};

}  // namespace matroska
