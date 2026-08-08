#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "ac3/core/tables.hpp"

// Signal analysis for the front ends: what each channel is carrying, in the
// units a meter needs. This is presentation-side work, not codec work, but it
// belongs in the library because ac3cli and ac3gui must report the same
// numbers from the same ballistics rather than each inventing its own.

namespace ac3::analysis {

// Everything at or below this reports as this, so callers never meet
// log10(0). Well under the -96 dBFS noise floor of 16-bit material.
inline constexpr double kFloorDb = -120.0;

// Full scale for a sample decoded from PCM16 is 32767/32768, not 1.0, so a
// file mastered to 0 dBFS never reaches unity. Clip detection has to meet it
// where it actually lands.
inline constexpr float kFullScale = 32767.0f / 32768.0f;

[[nodiscard]] double to_dbfs(double linear);

// Position of `db` on a meter scaled linearly in decibels from `floor_db` to
// 0 dBFS, clamped to [0, 1]. Shared so that a bar in the GUI and a bar in the
// terminal mean the same thing at the same level.
[[nodiscard]] constexpr double meter_fraction(double db, double floor_db = -60.0) {
    if (floor_db >= 0.0) {
        return 0.0;
    }
    return std::clamp((db - floor_db) / -floor_db, 0.0, 1.0);
}

[[nodiscard]] constexpr int channel_count(Acmod acmod, bool lfe) {
    return fullbw_channel_count(acmod) + (lfe ? 1 : 0);
}

// A/52 Table 5.8 channel array ordering. `index` runs over the full-bandwidth
// channels in that order, with the LFE last when present; an out-of-range
// index gives an empty view.
[[nodiscard]] std::string_view channel_name(Acmod acmod, bool lfe, int index);

// The layout in the spec's own front/rear notation, e.g. "3/2 + LFE".
[[nodiscard]] std::string_view layout_name(Acmod acmod, bool lfe);

// Loudspeaker azimuth for a channel: degrees counterclockwise from front, on
// the same ITU-R BS.775 ring the spatial renderer pans over. Empty for the
// LFE, which carries no direction, and for 1+1 dual mono, whose two channels
// are unrelated programs rather than one soundfield.
[[nodiscard]] std::optional<double> channel_azimuth_deg(Acmod acmod, bool lfe, int index);

struct MeterBallistics {
    // One-pole averaging time for the RMS bar. 300 ms is the familiar
    // programme-level integration: short enough to follow phrasing, long
    // enough not to twitch on every transient.
    double rms_integration_ms = 300.0;
    // Peak fallback in the style of an IEC 60268-10 PPM: instantaneous
    // attack, then a constant-rate descent slow enough to read.
    double peak_decay_db_per_s = 20.0;
    // The separate hold marker parks on the maximum for this long before it
    // begins descending at the same rate.
    double peak_hold_ms = 1200.0;
};

struct ChannelLevel {
    double peak_db = kFloorDb;  // ballistic peak
    double hold_db = kFloorDb;  // held maximum
    double rms_db = kFloorDb;   // integrated RMS
    bool clipped = false;       // reached full scale since the last reset
};

// Unweighted statistics over everything fed so far. A file report wants these
// rather than levels(): ballistics exist to make a moving display readable,
// and would only smear a question that has an exact answer.
struct ChannelSummary {
    double peak = 0.0;  // linear
    double sum_squares = 0.0;
    std::uint64_t samples = 0;
    std::uint64_t clipped_samples = 0;

    [[nodiscard]] double rms() const;
    [[nodiscard]] double peak_db() const;
    [[nodiscard]] double rms_db() const;
};

// Meters audio in A/52 channel order. One instance drives both the live
// display (levels(), ballistic) and the end-of-run report (summary(), exact);
// a single pass over the samples serves both, which is the point — the two
// front ends must not disagree about what a signal contains.
class LevelMeter {
public:
    LevelMeter(Acmod acmod, bool lfe, std::uint32_t sample_rate,
               const MeterBallistics& ballistics = {});

    // Meters `channels` channels instead of the acmod's own count, for a
    // layout no acmod can name: E-AC-3's dependent substreams add speakers the
    // coding mode has no word for, and a 7.1.4 access unit carries fourteen
    // coded channels against a coding mode that tops out at six.
    //
    // The acmod still names and places the first channel_count(acmod, lfe) of
    // them - those are the bed, in Table 5.8 order, and they are what the
    // soundfield ring is computed from. The rest are metered but contribute no
    // direction, which is also what channel_azimuth_deg says about them.
    // `channels` below the acmod's own count is raised to it rather than
    // truncating a layout the caller has already committed to.
    LevelMeter(Acmod acmod, bool lfe, std::uint32_t sample_rate, int channels,
               const MeterBallistics& ballistics = {});

    // Planar, one span per channel in A/52 order. The shortest span sets the
    // length; channels beyond the ones supplied are metered as silence, so a
    // caller that hands over fewer spans sees the rest fall away rather than
    // freeze.
    void process(std::span<const std::span<const float>> channels);

    // Interleaved, `stride` samples per frame. The first channel_count() of
    // each frame are metered and any extra ignored.
    void process_interleaved(std::span<const float> samples, std::size_t stride);

    [[nodiscard]] std::span<const ChannelLevel> levels() const { return levels_; }
    [[nodiscard]] std::span<const ChannelSummary> summary() const { return summary_; }
    [[nodiscard]] Acmod acmod() const { return acmod_; }
    [[nodiscard]] bool lfe() const { return lfe_; }
    [[nodiscard]] int channel_count() const { return static_cast<int>(levels_.size()); }
    [[nodiscard]] std::uint32_t sample_rate() const { return sample_rate_; }

    // Drops both the ballistic state and the accumulated summary.
    void reset();

private:
    // One channel's worth of block statistics, advanced over `seconds`.
    void advance(std::size_t channel, double block_peak, double mean_square, double seconds);

    Acmod acmod_;
    bool lfe_;
    std::uint32_t sample_rate_;
    MeterBallistics ballistics_;
    std::vector<ChannelLevel> levels_;
    std::vector<ChannelSummary> summary_;
    std::vector<double> mean_square_;   // one-pole RMS state, linear power
    std::vector<double> hold_elapsed_;  // seconds the hold marker has been parked
};

// Where the energy sits on the loudspeaker ring: the energy-weighted vector
// sum of the channel directions (Gerzon's energy vector). `azimuth_deg` is
// the perceived centre of the soundfield; `magnitude` runs from 1 (all the
// energy at one speaker) down to 0 (evenly opposed, so no direction at all).
struct SoundfieldVector {
    double azimuth_deg = 0.0;
    double magnitude = 0.0;
    double level_db = kFloorDb;  // combined RMS of the directional channels
};

// Computed from the integrated RMS of the full-bandwidth channels only: the
// LFE has no direction to contribute, and a subwoofer's level would otherwise
// swamp the sum.
[[nodiscard]] SoundfieldVector energy_vector(std::span<const ChannelLevel> levels, Acmod acmod);

}  // namespace ac3::analysis
