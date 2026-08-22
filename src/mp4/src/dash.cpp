#include "mp4/dash.hpp"

#include <format>

#include "manifest_detail.hpp"

namespace mp4 {

namespace {

// ISO/IEC 23009-1 §5.3.9.6's SegmentTimeline: a run-length-encoded list of
// <S d="..." r="..."/> entries, one per run of consecutive equal-duration
// segments (r is how many times d repeats AFTER the first, so a run of N
// equal segments is r="N-1" - or the attribute omitted for N=1, since r's
// default is 0). fragment()'s own segments are constant-duration except a
// possibly shorter final one (MediaSegment::duration_samples' own comment
// in mp4.hpp), so this is normally one or two <S> lines, but the encoding
// is fully general - it stays correct for any duration pattern a future
// caller's own MediaSegment list might have.
[[nodiscard]] std::string build_segment_timeline(std::span<const MediaSegment> segments) {
    std::string out = "      <SegmentTimeline>\n";
    std::size_t i = 0;
    while (i < segments.size()) {
        const auto duration = segments[i].duration_samples;
        std::size_t run = 1;
        while (i + run < segments.size() && segments[i + run].duration_samples == duration) {
            ++run;
        }
        if (run > 1) {
            out += std::format("        <S d=\"{}\" r=\"{}\"/>\n", duration, run - 1);
        } else {
            out += std::format("        <S d=\"{}\"/>\n", duration);
        }
        i += run;
    }
    out += "      </SegmentTimeline>\n";
    return out;
}

}  // namespace

std::string build_dash_adaptation_set(const AudioTrack& track,
                                      std::span<const MediaSegment> segments,
                                      const DashOptions& options) {
    const auto bandwidth = manifest_detail::estimate_bandwidth_bps(segments, track.sample_rate);

    // mimeType "audio/mp4" (RFC 4337-family ISOBMFF audio media type) and
    // codecs the same RFC 6381 sample-entry fourcc mp4::hls_codec_string
    // documents (ISO/IEC 14496-15 §5.5's 'ac-3'/'ec-3' registration) - DASH
    // (ISO/IEC 23009-1 §5.3.7.2) uses the identical 'Codecs' parameter HLS
    // does, both deriving it from RFC 6381's general ISOBMFF-file-family
    // registration rather than defining their own.
    //
    // A SegmentTimeline (rather than a flat SegmentTemplate @duration),
    // exact per-segment durations rather than a nominal one - a real gap a
    // flat @duration leaves: with a nominal duration and a shorter final
    // segment (the normal case - see mp4.hpp's own comment on
    // MediaSegment::duration_samples), a player computing "how many
    // segments does mediaPresentationDuration/@duration imply" can overshoot
    // by one and request a segment number that does not exist. Confirmed
    // against a real decode: ffmpeg's own dash demuxer did exactly that
    // against an earlier flat-@duration version of this function, logging
    // "Failed to open fragment of playlist" for the one-past-the-end
    // request - functionally harmless there (it still recovered and decoded
    // every real sample), but not something to ship deliberately when the
    // exact, spec-correct alternative is no harder to build from the same
    // MediaSegment list.
    return std::format(
        "<AdaptationSet mimeType=\"audio/mp4\" segmentAlignment=\"true\">\n"
        "  <Representation id=\"{}\" codecs=\"{}\" bandwidth=\"{}\" audioSamplingRate=\"{}\">\n"
        "    <SegmentTemplate timescale=\"{}\" initialization=\"{}\" media=\"{}\" "
        "startNumber=\"1\">\n"
        "{}"
        "    </SegmentTemplate>\n"
        "  </Representation>\n"
        "</AdaptationSet>\n",
        options.representation_id, track.codec_id, bandwidth, track.sample_rate, track.sample_rate,
        options.init_segment_uri, options.segment_uri_template, build_segment_timeline(segments));
}

}  // namespace mp4
