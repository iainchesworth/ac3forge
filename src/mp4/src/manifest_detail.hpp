#pragma once

#include <cmath>
#include <cstdint>
#include <span>

#include "mp4/mp4.hpp"

// Small pieces shared between hls.cpp and dash.cpp: both manifest flavors
// describe the same CMAF segments (that sharing is the entire point of
// CMAF, ISO/IEC 23000-19 - see mp4/dash.hpp's own header comment), so both
// need the same segment-duration and average-bitrate arithmetic. `inline`
// throughout: this header is included by more than one .cpp in the same
// target, so ODR requires it (matching isobmff_detail.hpp's own note).

namespace mp4::manifest_detail {

[[nodiscard]] inline double segment_seconds(const MediaSegment& segment,
                                            std::uint32_t sample_rate) {
    return static_cast<double>(segment.duration_samples) / static_cast<double>(sample_rate);
}

// The average bits/second these segments require. Both HLS's BANDWIDTH
// (RFC 8216 §4.3.4.2, REQUIRED) and DASH's Representation @bandwidth
// (ISO/IEC 23009-1 §5.3.5.2, REQUIRED) want "the minimum bandwidth... such
// that... the Representation... can be delivered" - an average across the
// whole asset is the only honest answer a single-representation helper with
// no per-segment target has; a real ABR ladder builder would want a
// measured peak instead, out of this module's single-representation scope
// (mp4.hpp's own header comment on mp4:: staying single-track).
[[nodiscard]] inline std::uint64_t estimate_bandwidth_bps(std::span<const MediaSegment> segments,
                                                          std::uint32_t sample_rate) {
    std::uint64_t total_bytes = 0;
    double total_seconds = 0.0;
    for (const auto& segment : segments) {
        total_bytes += segment.bytes.size();
        total_seconds += segment_seconds(segment, sample_rate);
    }
    if (total_seconds <= 0.0) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::llround(static_cast<double>(total_bytes) * 8.0 / total_seconds));
}

}  // namespace mp4::manifest_detail
