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
// No edit lists, no multiple tracks. Those matter for large-file seeking and
// multi-track muxing, not for playing back what this project produces.
//
// fragment() below (ROADMAP.md's A2) is the fMP4/CMAF follow-up mux() itself
// used to defer: an initialization segment plus one or more media segments,
// built from the same opaque AudioTrack/frame shape - see its own comment
// further down. mp4/hls.hpp and mp4/dash.hpp build the HLS media playlist and
// DASH MPD snippet that point at what it produces.

namespace mp4 {

// ISOBMFF sample entry codes (ISO/IEC 14496-15 §5.5 registers 'ac-3'; ETSI
// TS 102 366 Annex F itself is what ties each to its dac3/dec3 box). These
// are also, unmodified, RFC 6381's own 'Codecs' parameter value for either
// codec (see mp4/hls.hpp's hls_codec_string) - neither registers any of the
// dot-separated profile/level fields RFC 6381 §3 makes room for, so the bare
// sample-entry fourcc IS the codec string.
inline constexpr std::string_view kCodecAc3 = "ac-3";
inline constexpr std::string_view kCodecEac3 = "ec-3";

enum class MuxError : std::uint8_t {
    kNoFrames,
    kInvalidTrack,    // zero/negative channels or sample rate, unrecognised codec id, or no
                      // codec_config payload
    kFileTooLarge,    // mdat would need a 64-bit chunk offset (co64), unsupported in this cut
    kInvalidOptions,  // e.g. FragmentOptions::frames_per_fragment == 0
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

// --- Fragmented MP4 / CMAF --------------------------------------------------
//
// fragment() lays out the same track and frames as mux(), but as a
// fragmented movie (ISO/IEC 14496-12 §8.8's moof/mfhd/traf/tfhd/tfdt/trun)
// split into CMAF-shaped pieces (ISO/IEC 23000-19): an initialization
// segment (ftyp+moov, whose one trak carries mvex/trex instead of a
// populated sample table - a fragmented track's own stbl describes zero
// samples, ISO/IEC 14496-12 §8.8.3) that a player/packager loads once, and
// one or more media segments (styp+moof+mdat, one per fragment) that carry
// the actual samples and are what an HLS/DASH segment URI ends up pointing
// at - see mp4/hls.hpp and mp4/dash.hpp.
//
// A batch API, the same shape mux() already is: every frame is known up
// front (mirrors how matroska::mux() stayed batch-only when
// matroska::Writer was added later for a true live/incremental caller - see
// that header's own comment). fragment() therefore fills in real
// durations/timestamps throughout rather than the zero/unknown placeholders
// a true live fragmenter would need; a streaming variant, if one is ever
// needed, is an incremental Writer added beside this the same way.

struct FragmentOptions {
    std::string writing_app{"ac3forge"};
    // How many frames (access units) each fragment/media segment carries. A
    // fragment boundary is also wherever a player or CDN can start an
    // independent HTTP request, so this is really "how long is one HLS/DASH
    // segment" - 48 frames of 1536 samples at 48 kHz is 1.536 s, inside the
    // 1-10 s range the CMAF/DASH-IF interoperability guidelines assume most
    // packagers and CDNs are tuned for. Every AC-3/E-AC-3 access unit this
    // project produces is independently decodable (see
    // AudioTrack::samples_per_frame's own comment, and how ac3::io::scan
    // already groups a whole access unit - independent substream plus any
    // dependents - into the one opaque frame mp4:: ever sees), so any
    // grouping is valid; this only trades segment count for
    // segment-switch/start-up latency.
    std::uint32_t frames_per_fragment = 48;
};

// One media segment: styp + moof + mdat, ready to write out as-is (e.g.
// "segment3.m4s"). The bookkeeping fields alongside `bytes` are exactly what
// mp4/hls.hpp and mp4/dash.hpp need to build a playlist/MPD without
// re-parsing the segment's own boxes back out.
struct MediaSegment {
    std::vector<std::byte> bytes;
    std::uint32_t sequence_number = 0;   // this fragment's mfhd sequence_number (1-based)
    std::uint32_t sample_count = 0;      // frames carried in this fragment
    std::uint64_t duration_samples = 0;  // sample_count * AudioTrack::samples_per_frame
};

struct FragmentedOutput {
    std::vector<std::byte> init_segment;       // ftyp + moov (mvex/trex, zero samples)
    std::vector<MediaSegment> media_segments;  // one per fragment, in sequence_number order
};

// Fragments frames into an initialization segment plus media segments. No
// file I/O, same as mux(); the caller decides file names (or byte-range
// offsets, for a single concatenated CMAF track file) for init_segment and
// each media_segments[i].
[[nodiscard]] MP4_EXPORT std::expected<FragmentedOutput, MuxError> fragment(
    const AudioTrack& track, std::span<const std::vector<std::byte>> frames,
    const FragmentOptions& options = {});

}  // namespace mp4
