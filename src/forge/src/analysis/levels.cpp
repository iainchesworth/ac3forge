#include "ac3/analysis/levels.hpp"

#include <array>
#include <cmath>
#include <numbers>

#include "ac3/spatial/spatial.hpp"

namespace ac3::analysis {

namespace {

constexpr double kDegToRad = std::numbers::pi / 180.0;

// A/52 Table 5.8, "Channel Array Ordering", one row per acmod. Dual mono is
// two independent programs, so the spec names its channels Ch1/Ch2 rather
// than giving them positions.
constexpr std::array<std::array<std::string_view, 5>, 8> kChannelNames = {{
    {"Ch1", "Ch2"},                  // 0: 1+1
    {"C"},                           // 1: 1/0
    {"L", "R"},                      // 2: 2/0
    {"L", "C", "R"},                 // 3: 3/0
    {"L", "R", "S"},                 // 4: 2/1
    {"L", "C", "R", "S"},            // 5: 3/1
    {"L", "R", "SL", "SR"},          // 6: 2/2
    {"L", "C", "R", "SL", "SR"},     // 7: 3/2
}};

// The directions the spec's channel names imply, on the ITU-R BS.775 ring the
// spatial renderer already pans over. A mono surround sits behind the
// listener; the 3/2 entries come straight from the renderer's geometry so the
// two can never drift apart.
constexpr double kSurroundAzimuth = 180.0;

[[nodiscard]] constexpr std::optional<double> ring_azimuth(Acmod acmod, int index) {
    using ac3::spatial::kSpeakerAzimuthDeg;
    constexpr std::size_t kL = 0, kC = 1, kR = 2, kSL = 3, kSR = 4;
    const auto at = static_cast<std::size_t>(index);
    switch (acmod) {
        case Acmod::kDualMono: return std::nullopt;
        case Acmod::k1_0: return kSpeakerAzimuthDeg[kC];
        case Acmod::k2_0:
            return at == 0 ? kSpeakerAzimuthDeg[kL] : kSpeakerAzimuthDeg[kR];
        case Acmod::k3_0:
            return kSpeakerAzimuthDeg[at == 0 ? kL : (at == 1 ? kC : kR)];
        case Acmod::k2_1:
            return at == 2 ? kSurroundAzimuth
                           : kSpeakerAzimuthDeg[at == 0 ? kL : kR];
        case Acmod::k3_1:
            return at == 3 ? kSurroundAzimuth
                           : kSpeakerAzimuthDeg[at == 0 ? kL : (at == 1 ? kC : kR)];
        case Acmod::k2_2:
            return kSpeakerAzimuthDeg[at == 0 ? kL : (at == 1 ? kR : (at == 2 ? kSL : kSR))];
        case Acmod::k3_2:
            return kSpeakerAzimuthDeg[at];
    }
    return std::nullopt;
}

}  // namespace

double to_dbfs(double linear) {
    const double magnitude = std::abs(linear);
    if (magnitude <= 0.0) {
        return kFloorDb;
    }
    return std::max(kFloorDb, 20.0 * std::log10(magnitude));
}

std::string_view channel_name(Acmod acmod, bool lfe, int index) {
    const int fullbw = fullbw_channel_count(acmod);
    if (index < 0 || index >= channel_count(acmod, lfe)) {
        return {};
    }
    if (index == fullbw) {
        return "LFE";
    }
    return kChannelNames[static_cast<std::size_t>(acmod)][static_cast<std::size_t>(index)];
}

std::string_view layout_name(Acmod acmod, bool lfe) {
    // Two static tables rather than a runtime concatenation: the caller gets
    // a view it can hold, and there are only sixteen possible answers.
    constexpr std::array<std::string_view, 8> kPlain = {
        "1+1 dual mono", "1/0 mono", "2/0 stereo", "3/0", "2/1", "3/1", "2/2", "3/2",
    };
    constexpr std::array<std::string_view, 8> kWithLfe = {
        "1+1 dual mono + LFE", "1/0 mono + LFE", "2/0 stereo + LFE", "3/0 + LFE",
        "2/1 + LFE",           "3/1 + LFE",      "2/2 + LFE",        "3/2 + LFE",
    };
    const auto at = static_cast<std::size_t>(acmod);
    return lfe ? kWithLfe[at] : kPlain[at];
}

std::optional<double> channel_azimuth_deg(Acmod acmod, bool lfe, int index) {
    if (index < 0 || index >= channel_count(acmod, lfe)) {
        return std::nullopt;
    }
    if (index >= fullbw_channel_count(acmod)) {
        return std::nullopt;  // the LFE is non-directional by design
    }
    return ring_azimuth(acmod, index);
}

double ChannelSummary::rms() const {
    return samples == 0 ? 0.0 : std::sqrt(sum_squares / static_cast<double>(samples));
}

double ChannelSummary::peak_db() const { return to_dbfs(peak); }

double ChannelSummary::rms_db() const { return to_dbfs(rms()); }

LevelMeter::LevelMeter(Acmod acmod, bool lfe, std::uint32_t sample_rate,
                       const MeterBallistics& ballistics)
    : LevelMeter(acmod, lfe, sample_rate, analysis::channel_count(acmod, lfe), ballistics) {}

LevelMeter::LevelMeter(Acmod acmod, bool lfe, std::uint32_t sample_rate, int channels,
                       const MeterBallistics& ballistics)
    : acmod_(acmod),
      lfe_(lfe),
      sample_rate_(sample_rate == 0 ? 48000u : sample_rate),
      ballistics_(ballistics),
      levels_(static_cast<std::size_t>(
          std::max(channels, analysis::channel_count(acmod, lfe)))),
      summary_(levels_.size()),
      mean_square_(levels_.size(), 0.0),
      hold_elapsed_(levels_.size(), 0.0) {}

void LevelMeter::reset() {
    std::ranges::fill(levels_, ChannelLevel{});
    std::ranges::fill(summary_, ChannelSummary{});
    std::ranges::fill(mean_square_, 0.0);
    std::ranges::fill(hold_elapsed_, 0.0);
}

void LevelMeter::advance(std::size_t channel, double block_peak, double mean_square,
                         double seconds) {
    auto& level = levels_[channel];

    // Peak: instantaneous attack, constant-rate fallback. Starting from the
    // floor the decay term stays at the floor, so silence in never lifts the
    // needle.
    const double block_peak_db = to_dbfs(block_peak);
    const double decayed = level.peak_db - ballistics_.peak_decay_db_per_s * seconds;
    level.peak_db = std::max({block_peak_db, decayed, kFloorDb});

    // Hold: parks on the maximum, then descends at the peak's rate but never
    // below it, so the marker rejoins the bar instead of vanishing.
    if (block_peak_db >= level.hold_db) {
        level.hold_db = block_peak_db;
        hold_elapsed_[channel] = 0.0;
    } else {
        hold_elapsed_[channel] += seconds;
        const double over = hold_elapsed_[channel] - ballistics_.peak_hold_ms / 1000.0;
        if (over > 0.0) {
            level.hold_db = std::max(level.peak_db,
                                     level.hold_db - ballistics_.peak_decay_db_per_s * seconds);
        }
    }

    // RMS: one-pole average of the block's mean square. Over a block longer
    // than the integration time alpha saturates at 1, which is the right
    // answer — the block already contains more history than the filter holds.
    const double tau = ballistics_.rms_integration_ms / 1000.0;
    const double alpha = tau > 0.0 ? -std::expm1(-seconds / tau) : 1.0;
    mean_square_[channel] += alpha * (mean_square - mean_square_[channel]);
    level.rms_db = to_dbfs(std::sqrt(mean_square_[channel]));
}

void LevelMeter::process(std::span<const std::span<const float>> channels) {
    std::size_t length = 0;
    for (std::size_t ch = 0; ch < levels_.size() && ch < channels.size(); ++ch) {
        length = ch == 0 ? channels[ch].size() : std::min(length, channels[ch].size());
    }
    if (length == 0) {
        return;
    }
    const double seconds = static_cast<double>(length) / sample_rate_;

    for (std::size_t ch = 0; ch < levels_.size(); ++ch) {
        double block_peak = 0.0;
        double sum_squares = 0.0;
        std::uint64_t clipped = 0;
        if (ch < channels.size()) {
            for (const float sample : channels[ch].first(length)) {
                const double value = std::abs(static_cast<double>(sample));
                block_peak = std::max(block_peak, value);
                sum_squares += value * value;
                clipped += std::abs(sample) >= kFullScale ? 1u : 0u;
            }
        }
        auto& total = summary_[ch];
        total.peak = std::max(total.peak, block_peak);
        total.sum_squares += sum_squares;
        total.samples += length;
        total.clipped_samples += clipped;
        levels_[ch].clipped = levels_[ch].clipped || clipped > 0;
        advance(ch, block_peak, sum_squares / static_cast<double>(length), seconds);
    }
}

void LevelMeter::process_interleaved(std::span<const float> samples, std::size_t stride) {
    if (stride == 0) {
        return;
    }
    const std::size_t length = samples.size() / stride;
    if (length == 0) {
        return;
    }
    const double seconds = static_cast<double>(length) / sample_rate_;

    for (std::size_t ch = 0; ch < levels_.size(); ++ch) {
        double block_peak = 0.0;
        double sum_squares = 0.0;
        std::uint64_t clipped = 0;
        if (ch < stride) {
            for (std::size_t i = 0; i < length; ++i) {
                const float sample = samples[i * stride + ch];
                const double value = std::abs(static_cast<double>(sample));
                block_peak = std::max(block_peak, value);
                sum_squares += value * value;
                clipped += std::abs(sample) >= kFullScale ? 1u : 0u;
            }
        }
        auto& total = summary_[ch];
        total.peak = std::max(total.peak, block_peak);
        total.sum_squares += sum_squares;
        total.samples += length;
        total.clipped_samples += clipped;
        levels_[ch].clipped = levels_[ch].clipped || clipped > 0;
        advance(ch, block_peak, sum_squares / static_cast<double>(length), seconds);
    }
}

SoundfieldVector energy_vector(std::span<const ChannelLevel> levels, Acmod acmod) {
    SoundfieldVector result;
    const auto fullbw = static_cast<std::size_t>(fullbw_channel_count(acmod));

    double x = 0.0;
    double y = 0.0;
    double total = 0.0;
    for (std::size_t ch = 0; ch < fullbw && ch < levels.size(); ++ch) {
        const auto azimuth = ring_azimuth(acmod, static_cast<int>(ch));
        // A channel resting on the floor contributes nothing: counting it
        // would let the floor's own symmetry invent an image, and a silent
        // bed would report a phantom centre.
        if (!azimuth || levels[ch].rms_db <= kFloorDb) {
            continue;
        }
        // rms_db is a level, so 10^(dB/10) recovers the power the vector sum
        // must weight by.
        const double energy = std::pow(10.0, levels[ch].rms_db / 10.0);
        x += energy * std::cos(*azimuth * kDegToRad);
        y += energy * std::sin(*azimuth * kDegToRad);
        total += energy;
    }
    if (total <= 0.0) {
        return result;
    }

    const double length = std::hypot(x, y);
    result.magnitude = std::min(1.0, length / total);
    result.level_db = std::max(kFloorDb, 10.0 * std::log10(total));
    // Below the numerical noise of the sum the direction is meaningless;
    // reporting front-centre would claim a phantom image that is not there.
    if (length > 0.0) {
        result.azimuth_deg = std::atan2(y, x) / kDegToRad;
    }
    return result;
}

}  // namespace ac3::analysis
