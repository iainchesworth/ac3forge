#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "mp4/export.hpp"
#include "mp4/mp4.hpp"

// HLS signaling (RFC 8216, "HTTP Live Streaming") for the CMAF segments
// mp4::fragment() produces.
//
// Deliberately as codec-blind as the rest of mp4:: allows: WHICH codec
// string and CHANNELS value to use is either derived straight from
// AudioTrack (already opaque-by-design - see mp4.hpp) or supplied by the
// caller through HlsOptions::channels_attribute. This module only knows HLS
// *syntax* - #EXTM3U/#EXT-X-STREAM-INF/#EXT-X-MEDIA/#EXT-X-MAP/#EXTINF - not
// AC-3/E-AC-3 semantics, the same boundary mp4::mux() itself draws around
// 'dec3'/'dac3' (see AudioTrack::codec_config).

namespace mp4 {

struct HlsOptions {
    std::string init_segment_uri{"init.mp4"};
    // "{}" is replaced with each fragment's 1-based sequence number
    // (MediaSegment::sequence_number). A pattern with no "{}" is used
    // verbatim for every segment - the caller's choice/mistake, not this
    // module's to second-guess.
    std::string segment_uri_pattern{"segment{}.m4s"};
    // #EXT-X-MEDIA's CHANNELS attribute (Apple's HLS Authoring Specification
    // for Apple Devices, "Audio" requirements
    // https://developer.apple.com/documentation/http-live-streaming/hls-authoring-specification-for-apple-devices):
    // ordinarily just the decimal channel count (e.g. "6"), but Dolby
    // Digital Plus with Atmos objects needs "<N>/JOC" instead, where N is
    // the decodable object count (this project's own
    // ac3::io::ScannedStream::oba_complexity_index, TS 103 420
    // §8.3.2's complexity_index_type_a) - reiterated, with a worked example
    // ("CHANNELS="12/JOC""), by Dolby's own Online Delivery Kit
    // documentation
    // (https://ott.dolby.com/OnDelKits/DDP/Dolby_Digital_Plus_Online_Delivery_Kit_v1.5/Documentation/Content_Creation/SDM/help_files/topics/hls_c_hls_signal_atmos_ddp.html)
    // and shown verbatim in a real manifest
    // (CODECS="avc1.64001f,ec-3" / CHANNELS="12/JOC") by AWS MediaLive's own
    // HLS+Atmos documentation. Empty means "just the track's channel count",
    // correct for plain (non-Atmos) AC-3/E-AC-3 - mp4:: itself has no
    // opinion on JOC, since that is TS 103 420 object-layer syntax this
    // module never reads; the caller (which already read
    // oba_complexity_index off the bitstream to build the dec3 box, see
    // ac3::io::build_codec_config_box) is the one that knows.
    std::string channels_attribute;
    // #EXT-X-VERSION. 7 is the first version whose #EXT-X-MAP may appear in
    // a plain (non-I-frame-only) Media Playlist, which every fMP4 media
    // playlist needs (RFC 8216 §7's compatibility table).
    std::uint32_t version = 7;
    // Whether this is the whole, final asset (RFC 8216 §4.3.3.5's VOD
    // #EXT-X-PLAYLIST-TYPE plus a closing #EXT-X-ENDLIST) rather than a live,
    // still-growing playlist. mp4::fragment() is a batch API - every
    // fragment is already known - so VOD is the only shape this module's
    // caller can honestly produce; false only omits both tags for a caller
    // building its own live playlist update loop around fragment()'s output.
    bool vod = true;
};

// The RFC 6381 'Codecs' parameter value for one AudioTrack: just the
// ISOBMFF sample entry's own four-character code (mp4::kCodecAc3/kCodecEac3,
// ISO/IEC 14496-15 §5.5's registration), since neither AC-3 nor E-AC-3
// registers any of the dot-separated profile/level fields RFC 6381 §3 makes
// room for (unlike e.g. "avc1.640028") - confirmed against every real HLS
// manifest example this module's own documentation cites, which always show
// a bare CODECS="...,ec-3"/"...,ac-3". A named function rather than just
// using track.codec_id directly at each call site exists so there is one
// place this claim, and its citation, live.
[[nodiscard]] MP4_EXPORT std::string_view hls_codec_string(const AudioTrack& track);

// A minimal master playlist (RFC 8216 §4.3.4): one #EXT-X-MEDIA audio
// rendition plus one #EXT-X-STREAM-INF variant referencing it - the
// self-referencing pattern real audio-only HLS content uses (there is no
// separate video Media Playlist for the variant to point at; the audio
// rendition IS the variant), with CODECS and, when
// HlsOptions::channels_attribute is set, CHANNELS carrying the
// Atmos-signaling worked example HlsOptions documents. BANDWIDTH (a REQUIRED
// EXT-X-STREAM-INF attribute) is the average bits/second these segments
// require - an approximation a single-representation asset has no better
// answer for.
[[nodiscard]] MP4_EXPORT std::string build_hls_master_playlist(
    const AudioTrack& track, std::span<const MediaSegment> segments,
    std::string_view media_playlist_uri, const HlsOptions& options = {});

// The media playlist itself (RFC 8216 §4.3.3): #EXTM3U, #EXT-X-MAP pointing
// at the initialization segment, then #EXTINF/segment-URI pairs for every
// fragment in order.
[[nodiscard]] MP4_EXPORT std::string build_hls_media_playlist(
    const AudioTrack& track, std::span<const MediaSegment> segments,
    const HlsOptions& options = {});

}  // namespace mp4
