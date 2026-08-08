#include "ac3/meta/loudness.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ac3::meta {

namespace {

// BS.1770 tabulates the K-weighting coefficients at 48 kHz only, so anything
// else has to be designed. These are the two prototypes the standard names —
// a second-order high shelf and the RLB high-pass — with the standard's own
// centre frequencies, Q values and shelf gain. Evaluated at 48 kHz the design
// reproduces the tabulated coefficients to eleven digits, which is the check
// that it is the same filter and not merely a similar one.
constexpr double kShelfHz = 1681.974450955533;
constexpr double kShelfGainDb = 3.999843853973347;
constexpr double kShelfQ = 0.7071752369554196;
// The exponent relating the shelf's mid-band gain to its high-band gain. Not
// 0.5: the standard's prototype is not exactly symmetric.
constexpr double kShelfVbExponent = 0.4996667741545416;
constexpr double kHighpassHz = 38.13547087602444;
constexpr double kHighpassQ = 0.5003270373238773;

// 400 ms windows advanced by 100 ms, i.e. 75% overlap (BS.1770 §5). Every
// legal AC-3 rate divides by ten exactly, so a step is a whole number of
// samples and the windows never drift.
constexpr int kStepsPerBlock = 4;
constexpr double kBlockOffsetDb = -0.691;  // BS.1770's -0.691 term
constexpr double kAbsoluteGateLkfs = -70.0;
constexpr double kRelativeGateLu = -10.0;

// BS.1770 Table 3: unity for left, right and centre, +1.5 dB for each
// surround, and the LFE simply does not participate.
constexpr double kSurroundWeight = 1.41;

}  // namespace

LoudnessMeter::LoudnessMeter(SampleRate rate, Acmod acmod, bool lfe) {
    const auto fs = static_cast<double>(sample_rate_hz(rate));
    step_samples_ = static_cast<int>(sample_rate_hz(rate) / 10);

    {
        const double k = std::tan(std::numbers::pi * kShelfHz / fs);
        const double vh = std::pow(10.0, kShelfGainDb / 20.0);
        const double vb = std::pow(vh, kShelfVbExponent);
        const double kq = k / kShelfQ;
        const double a0 = 1.0 + kq + k * k;
        shelf_.b = {(vh + vb * kq + k * k) / a0, 2.0 * (k * k - vh) / a0,
                    (vh - vb * kq + k * k) / a0};
        shelf_.a = {2.0 * (k * k - 1.0) / a0, (1.0 - kq + k * k) / a0};
    }
    {
        const double k = std::tan(std::numbers::pi * kHighpassHz / fs);
        const double kq = k / kHighpassQ;
        const double a0 = 1.0 + kq + k * k;
        // The standard's numerator is exactly 1, −2, 1 — undivided by a0, which
        // leaves the passband gain at 1.005 rather than 1. That is the filter
        // BS.1770 specifies, so it is the filter measured against.
        highpass_.b = {1.0, -2.0, 1.0};
        highpass_.a = {2.0 * (k * k - 1.0) / a0, (1.0 - kq + k * k) / a0};
    }

    fullbw_ = fullbw_channel_count(acmod);
    channels_ = fullbw_ + (lfe ? 1 : 0);

    weights_.assign(static_cast<std::size_t>(fullbw_), 1.0);
    // Which coded positions are surrounds depends on acmod (Table 5.8): the
    // single S of 2/1 and 3/1 sits last, and 2/2 and 3/2 end with Ls, Rs.
    switch (acmod) {
        case Acmod::k2_1:
        case Acmod::k3_1:
            weights_.back() = kSurroundWeight;
            break;
        case Acmod::k2_2:
        case Acmod::k3_2:
            weights_[weights_.size() - 2] = kSurroundWeight;
            weights_.back() = kSurroundWeight;
            break;
        default:
            break;
    }

    shelf_state_.assign(static_cast<std::size_t>(fullbw_), State{});
    highpass_state_.assign(static_cast<std::size_t>(fullbw_), State{});
    step_sum_.assign(static_cast<std::size_t>(fullbw_), 0.0);
    recent_.assign(static_cast<std::size_t>(fullbw_), std::array<double, 4>{});
}

void LoudnessMeter::push(std::span<const std::span<const float>> channels) {
    std::size_t length = 0;
    for (int ch = 0; ch < fullbw_ && static_cast<std::size_t>(ch) < channels.size(); ++ch) {
        length = std::max(length, channels[static_cast<std::size_t>(ch)].size());
    }
    for (std::size_t n = 0; n < length; ++n) {
        for (int ch = 0; ch < fullbw_ && static_cast<std::size_t>(ch) < channels.size();
             ++ch) {
            const auto& source = channels[static_cast<std::size_t>(ch)];
            const double x = n < source.size() ? static_cast<double>(source[n]) : 0.0;
            const auto slot = static_cast<std::size_t>(ch);

            auto& s1 = shelf_state_[slot];
            const double mid = shelf_.b[0] * x + shelf_.b[1] * s1.x[0] +
                               shelf_.b[2] * s1.x[1] - shelf_.a[0] * s1.y[0] -
                               shelf_.a[1] * s1.y[1];
            s1.x = {x, s1.x[0]};
            s1.y = {mid, s1.y[0]};

            auto& s2 = highpass_state_[slot];
            const double out = highpass_.b[0] * mid + highpass_.b[1] * s2.x[0] +
                               highpass_.b[2] * s2.x[1] - highpass_.a[0] * s2.y[0] -
                               highpass_.a[1] * s2.y[1];
            s2.x = {mid, s2.x[0]};
            s2.y = {out, s2.y[0]};

            step_sum_[slot] += out * out;
        }
        if (++step_filled_ == step_samples_) {
            push_block();
            step_filled_ = 0;
        }
    }
}

void LoudnessMeter::push_block() {
    for (std::size_t ch = 0; ch < recent_.size(); ++ch) {
        auto& history = recent_[ch];
        history = {history[1], history[2], history[3], step_sum_[ch]};
        step_sum_[ch] = 0.0;
    }
    if (++steps_seen_ < kStepsPerBlock) {
        return;  // the first 400 ms has not elapsed yet
    }

    const auto samples = static_cast<double>(step_samples_) * kStepsPerBlock;
    double power = 0.0;
    for (std::size_t ch = 0; ch < recent_.size(); ++ch) {
        double sum = 0.0;
        for (const double value : recent_[ch]) {
            sum += value;
        }
        power += weights_[ch] * sum / samples;
    }
    // The absolute gate is applied here rather than at the end: a block that
    // fails it can never pass the relative gate either, and dropping it now
    // keeps the stored series to one double per 100 ms of programme.
    if (power > 0.0 && kBlockOffsetDb + 10.0 * std::log10(power) > kAbsoluteGateLkfs) {
        block_power_.push_back(power);
    }
}

std::optional<double> LoudnessMeter::integrated_lkfs() const {
    if (block_power_.empty()) {
        return std::nullopt;
    }
    // The weights are constant across blocks, so the mean of the weighted sums
    // IS the weighted sum of the means — which is why one accumulated number
    // per block is enough to run both gates.
    double sum = 0.0;
    for (const double power : block_power_) {
        sum += power;
    }
    const double ungated = sum / static_cast<double>(block_power_.size());
    const double relative_gate =
        kBlockOffsetDb + 10.0 * std::log10(ungated) + kRelativeGateLu;

    double gated_sum = 0.0;
    std::size_t gated_count = 0;
    for (const double power : block_power_) {
        if (kBlockOffsetDb + 10.0 * std::log10(power) > relative_gate) {
            gated_sum += power;
            ++gated_count;
        }
    }
    if (gated_count == 0) {
        return std::nullopt;
    }
    return kBlockOffsetDb +
           10.0 * std::log10(gated_sum / static_cast<double>(gated_count));
}

int dialnorm_from_lkfs(double lkfs) {
    const auto value = static_cast<int>(std::lround(-lkfs));
    return std::clamp(value, 1, 31);
}

}  // namespace ac3::meta
