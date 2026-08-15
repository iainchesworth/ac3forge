#include "mp4/hls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>

#include "manifest_detail.hpp"

namespace mp4 {

namespace {

using manifest_detail::estimate_bandwidth_bps;
using manifest_detail::segment_seconds;

// Substitutes the FIRST "{}" in `pattern` with `number` - HlsOptions'
// documented placeholder convention. A pattern without one is returned
// verbatim (see HlsOptions::segment_uri_pattern's own comment).
[[nodiscard]] std::string apply_sequence_number(std::string_view pattern, std::uint32_t number) {
    const auto pos = pattern.find("{}");
    if (pos == std::string_view::npos) {
        return std::string{pattern};
    }
    return std::format("{}{}{}", pattern.substr(0, pos), number, pattern.substr(pos + 2));
}

}  // namespace

std::string_view hls_codec_string(const AudioTrack& track) {
    // kCodecAc3/kCodecEac3 ("ac-3"/"ec-3") ARE RFC 6381's own codec string
    // for either format unmodified - see mp4.hpp's citation on those
    // constants and this header's own comment on hls_codec_string.
    return track.codec_id;
}

std::string build_hls_media_playlist(const AudioTrack& track,
                                     std::span<const MediaSegment> segments,
                                     const HlsOptions& options) {
    double max_seconds = 0.0;
    for (const auto& segment : segments) {
        max_seconds = std::max(max_seconds, segment_seconds(segment, track.sample_rate));
    }
    // RFC 8216 §4.3.3.1: an integer number of seconds, "MUST be less than
    // or equal to the target duration" for every segment - rounding UP
    // (rather than to nearest) is what keeps that true when a segment's
    // exact duration is not itself an integer.
    const auto target_duration = static_cast<std::uint64_t>(std::ceil(max_seconds));

    std::string out;
    out += "#EXTM3U\n";
    out += std::format("#EXT-X-VERSION:{}\n", options.version);
    out += std::format("#EXT-X-TARGETDURATION:{}\n", target_duration);
    if (!segments.empty()) {
        out += std::format("#EXT-X-MEDIA-SEQUENCE:{}\n", segments.front().sequence_number);
    }
    if (options.vod) {
        out += "#EXT-X-PLAYLIST-TYPE:VOD\n";
    }
    out += std::format("#EXT-X-MAP:URI=\"{}\"\n", options.init_segment_uri);
    for (const auto& segment : segments) {
        out += std::format("#EXTINF:{:.5f},\n", segment_seconds(segment, track.sample_rate));
        out += apply_sequence_number(options.segment_uri_pattern, segment.sequence_number);
        out += "\n";
    }
    if (options.vod) {
        out += "#EXT-X-ENDLIST\n";
    }
    return out;
}

std::string build_hls_master_playlist(const AudioTrack& track,
                                      std::span<const MediaSegment> segments,
                                      std::string_view media_playlist_uri,
                                      const HlsOptions& options) {
    const auto bandwidth = estimate_bandwidth_bps(segments, track.sample_rate);
    const std::string channels = options.channels_attribute.empty()
                                     ? std::format("{}", track.channels)
                                     : options.channels_attribute;

    std::string out;
    out += "#EXTM3U\n";
    out += std::format("#EXT-X-VERSION:{}\n", options.version);
    // RFC 8216 §4.3.5.1: every Media Segment is guaranteed to carry the
    // whole of any sample it starts (true of every AC-3/E-AC-3 access unit
    // this module ever writes - see mp4.hpp), so this asset qualifies.
    out += "#EXT-X-INDEPENDENT-SEGMENTS\n";
    // Audio-only content has no separate video rendition for the variant to
    // point at, so the #EXT-X-STREAM-INF URI below is the SAME media
    // playlist this #EXT-X-MEDIA line names - real audio-only HLS assets
    // (podcasts, music) use exactly this self-referencing pattern.
    out += std::format(
        "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"audio\",NAME=\"Audio\",DEFAULT=YES,AUTOSELECT=YES,"
        "CHANNELS=\"{}\",URI=\"{}\"\n",
        channels, media_playlist_uri);
    out += std::format("#EXT-X-STREAM-INF:BANDWIDTH={},CODECS=\"{}\",AUDIO=\"audio\"\n", bandwidth,
                       hls_codec_string(track));
    out += std::format("{}\n", media_playlist_uri);
    return out;
}

}  // namespace mp4
