#pragma once

#include <span>
#include <string>

#include "mp4/export.hpp"
#include "mp4/mp4.hpp"

// DASH signaling (ISO/IEC 23009-1, "Dynamic Adaptive Streaming over HTTP")
// for the same CMAF segments mp4::hls.hpp's helpers describe - that sharing
// is the entire point of CMAF (ISO/IEC 23000-19): one segment format, two
// manifest flavors. See mp4/hls.hpp's own header comment for the same
// codec-blindness this module keeps.

namespace mp4 {

struct DashOptions {
    std::string init_segment_uri{"init.mp4"};
    // ISO/IEC 23009-1 §5.3.9.4.3's SegmentTemplate substitution token for a
    // segment's 1-based number (MediaSegment::sequence_number) - "$Number$",
    // literally, not a "{}" placeholder like HlsOptions (DASH's own
    // template syntax, left as the DASH-native player/CDN sees it rather
    // than pre-expanded, since a real DASH SegmentTemplate is meant to stay
    // a template).
    std::string segment_uri_template{"segment$Number$.m4s"};
    std::string representation_id{"audio"};
};

// One <AdaptationSet>...</AdaptationSet> XML snippet - codecs, bandwidth,
// audioSamplingRate and a SegmentTemplate pointing at the CMAF segments
// fragment() produced - ready to nest inside a caller's own <Period>.
// Single-representation audio only, per this module's scope (see mp4.hpp's
// own header comment on mp4:: staying single-track): no ABR ladder, no
// multi-period MPD, no <MPD> document wrapper - a caller assembling a real
// manifest supplies those (mediaPresentationDuration, profiles, and
// whichever other Representations/Periods it has).
//
// Per-segment durations are exact, via a SegmentTemplate/SegmentTimeline
// (ISO/IEC 23009-1 §5.3.9.6) built from each MediaSegment's own
// duration_samples, rather than one nominal `duration` attribute assumed
// constant - fragment()'s own segments are constant-duration except (as
// usual) a possibly shorter final one, and a flat nominal duration is
// exactly what let a real player (ffmpeg's dash demuxer, while writing this
// module) compute one too many segments from mediaPresentationDuration and
// request a segment number past the end - harmless there, since it still
// recovered every real sample, but not a gap worth keeping when the exact
// alternative costs nothing extra to build. No Dolby Atmos/JOC-specific
// signaling is added here: unlike HLS's CHANNELS="<N>/JOC" (see
// mp4/hls.hpp's own citations), there is no equally-established DASH-IF
// convention this module can point at with the same confidence, so a caller
// that needs it adds its own SupplementalProperty/AudioChannelConfiguration
// to the returned snippet.
[[nodiscard]] MP4_EXPORT std::string build_dash_adaptation_set(
    const AudioTrack& track, std::span<const MediaSegment> segments,
    const DashOptions& options = {});

}  // namespace mp4
